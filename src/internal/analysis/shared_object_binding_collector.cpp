// SPDX-License-Identifier: Apache-2.0
#include "shared_object_binding_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

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

    SharedObjectBindings
    SharedObjectBindingCollector::collect(const llvm::Module& module,
                                          const std::vector<DirectCallSite>& directCallSites,
                                          const ProgramDefinedGlobals* programDefined) const
    {
        const llvm::DataLayout& layout = module.getDataLayout();
        // Every identity seen, by key: the key is what spawns are compared on, the binding is
        // what the entry is eventually given.
        std::map<std::string, SharedObjectBinding> bindingsByKey;
        // Keyed by the pair so that the same object handed to two different entries counts as
        // shared just as much as the same entry started twice on it.
        std::map<std::pair<std::string, std::string>, std::set<const llvm::Instruction*>> sites;
        std::map<std::pair<std::string, std::string>, bool> loopedSite;

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
                    loopedSite[key] = loopedSite[key] || spawned->insideLoop;
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
            bindings.emplace(entryFunctionId, std::move(binding));
        }

        return bindings;
    }
} // namespace ctrace::concurrency::internal::analysis
