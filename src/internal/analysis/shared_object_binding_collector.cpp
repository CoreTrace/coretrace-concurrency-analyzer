// SPDX-License-Identifier: Apache-2.0
#include "shared_object_binding_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"
#include "parameter_footprints.hpp"
#include "thread_completion_analysis.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CheckedArithmetic.h>

#include <cstdio>
#include <map>
#include <string_view>
#include <optional>
#include <set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Prefix `canonicalStorageGroupId` gives an object known only as a parameter.
        constexpr std::string_view kParameterPrefix = "arg:";

        constexpr unsigned kPThreadEntryOperandIndex = 2;
        constexpr unsigned kPThreadArgumentOperandIndex = 3;
        constexpr unsigned kStdThreadCallableOperandIndex = 1;
        constexpr unsigned kStdThreadObjectOperandIndex = 2;

        /// What a spawn, or a call handing the object on, passes. `key` compares two of them;
        /// an object only a construction site further up can name has a parameter key and no
        /// binding yet.
        struct ObjectIdentity
        {
            std::string key;
            std::optional<SharedObjectBinding> binding;
        };

        struct SpawnedObject
        {
            std::string entryFunctionId;
            ObjectIdentity object;
            unsigned argumentIndex = 0;
            bool insideLoop = false;
            /// The pointer the spawn hands over, as the entry receives it.
            const llvm::Value* handed = nullptr;
            const llvm::Function* entry = nullptr;
        };

        ObjectIdentity namedIdentity(RootBinding object, SharedObjectKind kind)
        {
            std::string key;
            switch (kind)
            {
            case SharedObjectKind::Global:
                key = "global-object:";
                break;
            case SharedObjectKind::Local:
                key = "local-object:";
                break;
            case SharedObjectKind::BehindPointer:
                key = "pointed-object:";
                break;
            }
            key += object.symbol + object.region.suffix();
            return ObjectIdentity{
                .key = std::move(key),
                .binding = SharedObjectBinding{.object = std::move(object), .kind = kind},
            };
        }

        /// The object behind the temporary `std::thread` materialises for it.
        ///
        /// Its constructor takes its arguments by forwarding reference, so the address of the
        /// object is spilled into a slot of its own, and that slot is what the call receives.
        /// Reading it as the object would name the temporary instead. `pthread_create` takes its
        /// argument by value and needs none of this, which is why the step is tied to the spawn
        /// form rather than guessed at.
        const llvm::Value* throughForwardingSlot(const llvm::Value& value)
        {
            const auto* storage =
                llvm::dyn_cast<llvm::AllocaInst>(value.stripPointerCastsAndAliases());
            if (storage == nullptr || !storage->getAllocatedType()->isPointerTy())
                return &value;

            const llvm::Value* stored = nullptr;
            for (const llvm::User* user : storage->users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != storage)
                    continue;

                // More than one store leaves the slot ambiguous; the temporary this looks for is
                // written exactly once.
                if (stored != nullptr)
                    return &value;

                stored = store->getValueOperand();
            }

            return stored != nullptr ? stored : &value;
        }

        /// The parameter a slot merely holds, when that is all it holds.
        const llvm::Argument* spilledParameterOf(const llvm::AllocaInst& storage)
        {
            const llvm::Argument* spilled = nullptr;
            for (const llvm::User* user : storage.users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != &storage)
                    continue;

                // Written more than once, the slot no longer stands for the parameter.
                if (spilled != nullptr)
                    return nullptr;

                spilled = llvm::dyn_cast<llvm::Argument>(store->getValueOperand());
                if (spilled == nullptr)
                    return nullptr;
            }

            return spilled;
        }

        /// Identity of the object a value designates. An object read out of a variable is named
        /// by that variable, or by the parameter the variable merely holds. An object whose own
        /// address is passed is named the way its owner's accesses name it: a global by its name,
        /// a local by its storage. Used at the spawn and at every construction site above it, so
        /// both sides speak of the object in the same terms.
        std::optional<ObjectIdentity> objectIdentityOf(const llvm::Value& value,
                                                       const llvm::DataLayout& layout,
                                                       const ProgramDefinedGlobals* programDefined)
        {
            const llvm::Value* object = value.stripPointerCastsAndAliases();
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(object);
            if (load != nullptr)
                object = load->getPointerOperand();

            if (const auto* storage = llvm::dyn_cast<llvm::AllocaInst>(object))
            {
                if (const llvm::Argument* spilled = spilledParameterOf(*storage))
                {
                    return ObjectIdentity{.key = "arg:" + functionId(*spilled->getParent()) + ":" +
                                                 std::to_string(spilled->getArgNo())};
                }
            }

            if (load == nullptr)
            {
                if (std::optional<RootBinding> global =
                        resolveTrackedRoot(value, &layout, 0, programDefined);
                    global.has_value() && global->kind == RootBindingKind::Global)
                {
                    return namedIdentity(std::move(*global), SharedObjectKind::Global);
                }

                if (std::optional<RootBinding> local = resolveLocalStorageRoot(value, &layout, 0))
                    return namedIdentity(std::move(*local), SharedObjectKind::Local);
            }

            std::optional<std::string> storageId = canonicalStorageGroupId(*object);
            if (!storageId.has_value())
                return std::nullopt;

            if (std::string_view(*storageId).starts_with(kParameterPrefix))
                return ObjectIdentity{.key = std::move(*storageId)};

            return namedIdentity(RootBinding::global(std::move(*storageId)),
                                 SharedObjectKind::BehindPointer);
        }

        constexpr std::string_view kProgramEntryFunction = "main";

        /// No path leaves the loop and comes back to it: a second pass would hand the same
        /// elements out again while the first pass's threads may still run.
        bool runsOnce(const llvm::Loop& loop)
        {
            llvm::SmallVector<llvm::BasicBlock*, 4> exits;
            loop.getExitBlocks(exits);
            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
            std::vector<const llvm::BasicBlock*> pending(exits.begin(), exits.end());
            while (!pending.empty())
            {
                const llvm::BasicBlock* block = pending.back();
                pending.pop_back();
                if (loop.contains(block))
                    return false;
                if (!visited.insert(block).second)
                    continue;
                for (const llvm::BasicBlock* successor : llvm::successors(block))
                    pending.push_back(successor);
            }
            return true;
        }

        /// Whether `later` can run after `earlier` in the same round of `loop`, before the back
        /// edge starts the next one.
        bool followsInRound(const llvm::Instruction& earlier, const llvm::Instruction& later,
                            const llvm::Loop& loop)
        {
            if (earlier.getParent() == later.getParent() && earlier.comesBefore(&later))
                return true;

            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
            std::vector<const llvm::BasicBlock*> pending(llvm::succ_begin(earlier.getParent()),
                                                         llvm::succ_end(earlier.getParent()));
            while (!pending.empty())
            {
                const llvm::BasicBlock* block = pending.back();
                pending.pop_back();
                if (!loop.contains(block) || block == loop.getHeader() ||
                    !visited.insert(block).second)
                    continue;
                if (block == later.getParent())
                    return true;
                for (const llvm::BasicBlock* successor : llvm::successors(block))
                    pending.push_back(successor);
            }
            return false;
        }

        /// Whether `function` keeps the pointer it receives as `argumentIndex` to itself: every
        /// access through it is placed, and it is never stored where another thread could read
        /// it, nor handed to code the unit does not define.
        bool keepsPointer(const ParameterFootprint& footprint)
        {
            return !footprint.all.readsWholeObject && !footprint.all.writesWholeObject;
        }

        /// Whether the bytes a footprint places stay inside the element the pointer sits in at
        /// `position`. Whether it also reaches bytes it does not place, or lets the pointer go,
        /// is settled with the object's address: the spawn handing it over is one of its uses.
        bool placedInsideElement(const ParameterFootprint& footprint, std::int64_t position,
                                 std::uint64_t elementSize)
        {
            for (const auto* ranges : {&footprint.all.reads, &footprint.all.writes})
            {
                for (const ParameterFootprint::Range& range : *ranges)
                {
                    if (position + range.begin < 0 ||
                        position + range.end > static_cast<std::int64_t>(elementSize))
                        return false;
                }
            }
            return true;
        }

        /// The thread entry a spawn hands `argument` to, when `argument` is the object operand
        /// of that spawn: the `void*` of `pthread_create`, or the forwarding slot of a
        /// `std::thread`.
        const llvm::Function* entryReceiving(const llvm::CallBase& call, unsigned argument,
                                             const ConcurrencySymbolClassifier& classifier)
        {
            switch (classifier.classify(call))
            {
            case CallKind::PThreadCreate:
                return argument == kPThreadArgumentOperandIndex
                           ? resolveFunctionValue(*call.getArgOperand(kPThreadEntryOperandIndex))
                           : nullptr;
            case CallKind::StdThreadCtor:
            case CallKind::StdJThreadCtor:
                return argument == kStdThreadObjectOperandIndex
                           ? resolveFunctionValue(
                                 *call.getArgOperand(kStdThreadCallableOperandIndex))
                           : nullptr;
            default:
                return nullptr;
            }
        }

        /// Every pointer into `base` stays where the access paths name it: read or written
        /// through, indexed, copied into a local variable, or handed to a function or a thread
        /// entry of the unit that keeps it. Stored anywhere else, or handed to code the unit does
        /// not define, it lets some thread reach an element without naming the object, and no
        /// pair would show that access.
        bool addressStaysNamed(const llvm::Value& base,
                               const ConcurrencySymbolClassifier& classifier,
                               ParameterFootprints& footprints)
        {
            const auto handedToKeeper = [&](const llvm::CallBase& call, unsigned argument)
            {
                if (llvm::isa<llvm::DbgInfoIntrinsic>(call) || llvm::isa<llvm::MemIntrinsic>(call))
                    return true;
                if (const llvm::Function* entry = entryReceiving(call, argument, classifier))
                    return !entry->isDeclaration() && keepsPointer(footprints.of(*entry, 0));
                const llvm::Function* callee = classifier.directCallee(call);
                return classifier.classify(call) == CallKind::Unknown && callee != nullptr &&
                       !callee->isDeclaration() && keepsPointer(footprints.of(*callee, argument));
            };

            llvm::SmallPtrSet<const llvm::Value*, 32> seen;
            std::vector<const llvm::Value*> pending{&base};
            while (!pending.empty())
            {
                const llvm::Value* pointer = pending.back();
                pending.pop_back();
                if (!seen.insert(pointer).second)
                    continue;

                for (const llvm::Use& use : pointer->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (llvm::isa<llvm::GEPOperator>(user) || llvm::isa<llvm::BitCastInst>(user) ||
                        llvm::isa<llvm::AddrSpaceCastInst>(user) ||
                        llvm::isa<llvm::PHINode>(user) || llvm::isa<llvm::SelectInst>(user))
                    {
                        pending.push_back(user);
                        continue;
                    }

                    if (llvm::isa<llvm::LoadInst>(user) || llvm::isa<llvm::ICmpInst>(user))
                        continue;

                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() == pointer)
                            continue;
                        // A copy in a local variable is followed through its reads; the slot a
                        // `std::thread` forwards its argument through is handed to the spawn.
                        const auto* variable = llvm::dyn_cast<llvm::AllocaInst>(
                            store->getPointerOperand()->stripPointerCasts());
                        if (variable == nullptr)
                            return false;
                        for (const llvm::Use& variableUse : variable->uses())
                        {
                            const llvm::User* reader = variableUse.getUser();
                            if (const auto* read = llvm::dyn_cast<llvm::LoadInst>(reader))
                                pending.push_back(read);
                            else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(reader))
                            {
                                if (!call->isArgOperand(&variableUse) ||
                                    !handedToKeeper(*call, call->getArgOperandNo(&variableUse)))
                                    return false;
                            }
                            else if (!llvm::isa<llvm::StoreInst>(reader))
                                return false;
                        }
                        continue;
                    }

                    const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (call == nullptr || !call->isArgOperand(&use) ||
                        !handedToKeeper(*call, call->getArgOperandNo(&use)))
                        return false;
                }
            }
            return true;
        }

        /// The spawn hands each thread the element its loop counter picks, in a loop that runs
        /// once in the program's entry. Whether another spawn hands out the same elements is
        /// settled once every spawn is known.
        std::optional<ElementPerThread>
        elementPerThreadAt(const llvm::CallBase& spawn, const llvm::Value& object,
                           const llvm::Function& entry, const llvm::LoopInfo& loops,
                           const llvm::DominatorTree& dominators, const llvm::DataLayout& layout,
                           const ConcurrencySymbolClassifier& classifier, bool entryCalledDirectly)
        {
            if (spawn.getFunction()->getName() != llvm::StringRef(kProgramEntryFunction) ||
                entryCalledDirectly)
                return std::nullopt;

            const llvm::Loop* loop = loops.getLoopFor(spawn.getParent());
            if (loop == nullptr || !runsOnce(*loop))
                return std::nullopt;

            std::optional<VariablyIndexedPointer> handed = variablyIndexedPointer(object, layout);
            // A variable or a global is the same object in every round. A pointer read from
            // memory may move between rounds, and would need its variable followed.
            if (!handed.has_value() || (!llvm::isa<llvm::AllocaInst>(handed->base) &&
                                        !llvm::isa<llvm::GlobalVariable>(handed->base)))
                return std::nullopt;

            const auto* read = llvm::dyn_cast<llvm::LoadInst>(handed->index);
            if (read == nullptr)
                return std::nullopt;

            const std::optional<LoopCounter> counter = loopCounter(*loop, dominators);
            if (!counter.has_value() || counter->variable != read->getPointerOperand())
                return std::nullopt;

            // The thread reaches nothing but its element, and no pointer to an element goes
            // where another thread could use it unnamed.
            ParameterFootprints footprints(classifier, layout);
            if (!placedInsideElement(footprints.of(entry, 0), handed->position,
                                     handed->elementSize) ||
                !addressStaysNamed(*handed->base, classifier, footprints))
                return std::nullopt;

            return ElementPerThread{
                .spawn = &spawn,
                .loop = loop,
                .counter = counter->variable,
                .increment = counter->increment,
                .handed = *handed,
            };
        }

        std::optional<SpawnedObject> spawnedObjectAt(const llvm::CallBase& call, CallKind kind,
                                                     bool insideLoop,
                                                     const llvm::DataLayout& layout,
                                                     const ProgramDefinedGlobals* programDefined)
        {
            unsigned entryIndex = 0;
            unsigned objectIndex = 0;
            switch (kind)
            {
            case CallKind::PThreadCreate:
                entryIndex = kPThreadEntryOperandIndex;
                objectIndex = kPThreadArgumentOperandIndex;
                break;
            case CallKind::StdThreadCtor:
            case CallKind::StdJThreadCtor:
                entryIndex = kStdThreadCallableOperandIndex;
                objectIndex = kStdThreadObjectOperandIndex;
                break;
            default:
                return std::nullopt;
            }

            if (call.arg_size() <= objectIndex)
                return std::nullopt;

            const llvm::Function* entry = resolveFunctionValue(*call.getArgOperand(entryIndex));
            if (entry == nullptr || entry->isDeclaration() || entry->arg_empty())
                return std::nullopt;

            // A heap object has no name of its own, but the variable the pointer is read from
            // does, and every spawn reading the same variable receives the same object. That
            // slot is therefore used as the object's identity.
            //
            // It assumes the variable is not repointed between two spawns. Assuming otherwise
            // would leave heap-held state with no identity at all, which is the gap this closes.
            const llvm::Value* object = call.getArgOperand(objectIndex);
            if (kind != CallKind::PThreadCreate)
                object = throughForwardingSlot(*object);

            std::optional<ObjectIdentity> identity =
                objectIdentityOf(*object, layout, programDefined);
            if (!identity.has_value())
                return std::nullopt;

            return SpawnedObject{
                .entryFunctionId = functionId(*entry),
                .object = std::move(*identity),
                // Both spawn forms deliver the object on the entry's first parameter: the `void*`
                // of a pthread routine, and the `this` of a member function.
                .argumentIndex = 0,
                .insideLoop = insideLoop,
                .handed = object,
                .entry = entry,
            };
        }
    } // namespace

    SharedObjectBindingCollector::SharedObjectBindingCollector(
        const ConcurrencySymbolClassifier& classifier, LlvmFunctionAnalysisProvider& analyses)
        : classifier_(classifier), analyses_(analyses)
    {
    }

    std::unordered_set<std::string> sharedObjectNames(const SharedObjectBindings& bindings,
                                                      SharedObjectKind kind)
    {
        std::unordered_set<std::string> names;
        for (const auto& [entryFunctionId, binding] : bindings)
        {
            if (binding.kind == kind)
                names.insert(binding.object.symbol);
        }
        return names;
    }

    namespace
    {
        /// The load of `counter` indexing the element `pointer` points into, when that element is
        /// one of those the spawn hands out and the `byteSize` bytes starting `offset` bytes past
        /// `pointer` stay inside it.
        const llvm::LoadInst* elementIndexRead(const ElementPerThread& elements,
                                               const llvm::Value& pointer, std::int64_t offset,
                                               std::uint64_t byteSize, const llvm::Value& counter,
                                               const llvm::DataLayout& layout)
        {
            const std::optional<VariablyIndexedPointer> reached =
                variablyIndexedPointer(pointer, layout);
            if (!reached.has_value() || reached->elementSize != elements.handed.elementSize ||
                reached->elementStart != elements.handed.elementStart ||
                reached->base != elements.handed.base)
                return nullptr;

            const auto* read = llvm::dyn_cast<llvm::LoadInst>(reached->index);
            const std::optional<std::int64_t> start = llvm::checkedAdd(reached->position, offset);
            if (read == nullptr || read->getPointerOperand() != &counter || !start.has_value() ||
                *start < 0 || byteSize == 0 || byteSize > elements.handed.elementSize ||
                static_cast<std::uint64_t>(*start) > elements.handed.elementSize - byteSize)
                return nullptr;
            return read;
        }
    } // namespace

    bool reachesElementBeforeHandOver(const ElementPerThread& elements,
                                      const llvm::Instruction& access, const llvm::Value& pointer,
                                      std::int64_t offset, std::uint64_t byteSize,
                                      const llvm::DataLayout& layout)
    {
        if (!elements.loop->contains(&access) ||
            followsInRound(*elements.spawn, access, *elements.loop))
            return false;

        // The spawn must read the counter before this round's increment, as the access does
        // ahead of the spawn: the element is then the one this round hands over, and not the
        // one an earlier round handed to a thread that may still run.
        const auto* handedRead = llvm::cast<llvm::LoadInst>(elements.handed.index);
        return elementIndexRead(elements, pointer, offset, byteSize, *elements.counter, layout) !=
                   nullptr &&
               !followsInRound(*elements.increment, *handedRead, *elements.loop);
    }

    bool reachesElementOfJoinedThread(const ElementPerThread& elements, const RoundJoin& joins,
                                      const llvm::Instruction& access, const llvm::Value& pointer,
                                      std::int64_t offset, std::uint64_t byteSize,
                                      const llvm::DataLayout& layout,
                                      const llvm::DominatorTree& dominators)
    {
        const std::optional<LoopCounter> counter = loopCounter(*joins.loop, dominators);
        if (!counter.has_value() || !joins.loop->contains(&access) ||
            !joins.success.covers(access, dominators))
            return false;

        // Read before the increment, the counter still names the round whose thread was joined.
        const llvm::LoadInst* read =
            elementIndexRead(elements, pointer, offset, byteSize, *counter->variable, layout);
        return read != nullptr && !followsInRound(*counter->increment, *read, *joins.loop);
    }

    SharedObjectBindings SharedObjectBindingCollector::collect(
        const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
        const ProgramDefinedGlobals* programDefined,
        const std::unordered_map<std::string, EntryConcurrencyInfo>* spawnSites) const
    {
        const llvm::DataLayout& layout = module.getDataLayout();
        // Every identity seen, by key: the key is what spawns are compared on, the binding is
        // what the entry is eventually given.
        std::map<std::string, SharedObjectBinding> bindingsByKey;
        // Keyed by the pair so that the same object handed to two different entries counts as
        // shared just as much as the same entry started twice on it.
        std::map<std::pair<std::string, std::string>, std::set<const llvm::Instruction*>> sites;
        std::map<std::pair<std::string, std::string>, bool> loopedSite;
        // Keyed by spawn: whether it gives each of its threads an element of its own.
        std::map<const llvm::Instruction*, ElementPerThread> elementsBySite;
        // Every spawn handing out an object, whichever entry receives it.
        std::map<std::string, std::set<const llvm::Instruction*>> sitesByObject;
        // By entry: the classes whose constructors start it on the object they construct, and
        // whether a spawn starts it otherwise.
        std::map<std::string, std::set<std::string>> constructedClasses;
        std::set<std::string> startedOtherwise;

        bool entryCalledDirectly = false;
        for (const DirectCallSite& site : directCallSites)
        {
            const auto* callee = site.call != nullptr ? site.call->getCalledFunction() : nullptr;
            entryCalledDirectly =
                entryCalledDirectly ||
                (callee != nullptr && callee->getName() == llvm::StringRef(kProgramEntryFunction));
        }

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const llvm::DominatorTree& dominatorTree = analyses_.getDominatorTree(function);
            const llvm::LoopInfo& loopInfo = analyses_.getLoopInfo(function);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                const bool insideLoop = loopInfo.getLoopFor(&block) != nullptr;
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const std::optional<SpawnedObject> spawned = spawnedObjectAt(
                        *call, classifier_.classify(*call), insideLoop, layout, programDefined);
                    if (!spawned.has_value())
                        continue;

                    if (spawned->object.binding.has_value())
                        bindingsByKey.emplace(spawned->object.key, *spawned->object.binding);
                    const auto key = std::pair{spawned->entryFunctionId, spawned->object.key};
                    sites[key].insert(&instruction);
                    sitesByObject[spawned->object.key].insert(&instruction);
                    loopedSite[key] = loopedSite[key] || spawned->insideLoop;
                    // A constructor handing its own object to the spawn starts the thread on an
                    // object whose lifetime begins with that constructor call.
                    if (const std::optional<DemangledName> name =
                            demangleFunction(function.getName());
                        name.has_value() && name->constructor &&
                        spawned->object.key == "arg:" + functionId(function) + ":0")
                    {
                        constructedClasses[spawned->entryFunctionId].insert(name->context);
                    }
                    else
                    {
                        startedOtherwise.insert(spawned->entryFunctionId);
                    }
                    if (std::optional<ElementPerThread> elements = elementPerThreadAt(
                            *call, *spawned->handed, *spawned->entry, loopInfo, dominatorTree,
                            layout, classifier_, entryCalledDirectly))
                    {
                        elementsBySite.emplace(&instruction, *elements);
                    }
                }
            }
        }

        // Handing an object to a thread is itself the proof: from that point two flows of
        // control can reach it, the one that spawned and the one that was spawned. Requiring a
        // second spawn would miss the ordinary case, where the creator keeps using the object it
        // just handed over.
        //
        // Identity is all this grants. Whether two accesses through it actually race is decided
        // afterwards, by the same concurrency reasoning that judges a global.
        // An object whose constructor starts its own thread hands `this` to the spawn, and
        // `this` names nothing on its own. The construction sites do: each passes the object it
        // owns. The step repeats because the chain has more than one link — a complete
        // constructor calls the base one, handing its own `this` along.
        constexpr unsigned kMaxCallerRounds = 4;
        const auto callersOf = [&](const std::string& parameterId)
        {
            std::set<std::string> resolved;
            // `arg:<function>:<index>` names the parameter the object arrived on.
            const std::size_t indexSeparator = parameterId.rfind(':');
            if (indexSeparator == std::string::npos)
                return resolved;

            const std::string ownerFunctionId = parameterId.substr(
                kParameterPrefix.size(), indexSeparator - kParameterPrefix.size());
            const unsigned argumentIndex =
                static_cast<unsigned>(std::stoul(parameterId.substr(indexSeparator + 1)));

            for (const DirectCallSite& site : directCallSites)
            {
                if (site.calleeFunctionId != ownerFunctionId || site.call == nullptr ||
                    site.call->arg_size() <= argumentIndex)
                {
                    continue;
                }

                if (std::optional<ObjectIdentity> caller = objectIdentityOf(
                        *site.call->getArgOperand(argumentIndex), layout, programDefined))
                {
                    if (caller->binding.has_value())
                        bindingsByKey.emplace(caller->key, *caller->binding);
                    resolved.insert(std::move(caller->key));
                }
            }

            return resolved;
        };

        // An entry started on two different objects gets no identity at all. Its body is one
        // function, so every access in it would otherwise be attributed to whichever object was
        // seen first, and two threads working on their own copies would be reported as racing.
        std::map<std::string, std::set<std::string>> objectsByEntry;
        for (const auto& [key, instructions] : sites)
            objectsByEntry[key.first].insert(key.second);

        const auto spawnedOnce = [&](const std::string& entryFunctionId)
        {
            if (spawnSites == nullptr)
                return false;
            const auto it = spawnSites->find(entryFunctionId);
            return it != spawnSites->end() && it->second.staticSpawnCount == 1;
        };

        SharedObjectBindings bindings;
        for (const auto& [entryFunctionId, objects] : objectsByEntry)
        {
            std::set<std::string> candidates = objects;
            for (unsigned round = 0; round < kMaxCallerRounds; ++round)
            {
                std::set<std::string> next;
                bool parameterised = false;
                for (const std::string& candidate : candidates)
                {
                    if (!std::string_view(candidate).starts_with(kParameterPrefix))
                    {
                        next.insert(candidate);
                        continue;
                    }

                    parameterised = true;
                    const std::set<std::string> callers = callersOf(candidate);
                    next.insert(callers.begin(), callers.end());
                }

                candidates = std::move(next);
                if (!parameterised)
                    break;
            }

            // Anything still parameterised is an object no construction site names, and more
            // than one candidate means the object depends on the caller. Neither is an identity
            // two accesses can be judged against.
            std::erase_if(candidates, [](const std::string& candidate)
                          { return std::string_view(candidate).starts_with(kParameterPrefix); });
            if (candidates.size() != 1)
                continue;

            const auto bindingIt = bindingsByKey.find(*candidates.begin());
            if (bindingIt == bindingsByKey.end())
                continue;

            SharedObjectBinding binding = bindingIt->second;
            binding.argumentIndex = 0;
            if (const auto classes = constructedClasses.find(entryFunctionId);
                classes != constructedClasses.end() && classes->second.size() == 1 &&
                !startedOtherwise.contains(entryFunctionId))
                binding.constructedClass = *classes->second.begin();

            // Each thread has an element of its own only when this is the one spawn of the
            // entry and the one spawn handing out the object's elements: two spawns may hand
            // the same element to two threads.
            const auto objectSitesIt = sitesByObject.find(*candidates.begin());
            if (objectSitesIt != sitesByObject.end() && objectSitesIt->second.size() == 1 &&
                spawnedOnce(entryFunctionId))
            {
                if (const auto elementsIt = elementsBySite.find(*objectSitesIt->second.begin());
                    elementsIt != elementsBySite.end())
                {
                    binding.elementPerThread = elementsIt->second;
                }
            }
            bindings.emplace(entryFunctionId, std::move(binding));
        }

        return bindings;
    }
} // namespace ctrace::concurrency::internal::analysis
