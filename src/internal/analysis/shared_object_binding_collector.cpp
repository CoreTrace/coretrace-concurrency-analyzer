// SPDX-License-Identifier: Apache-2.0
#include "shared_object_binding_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"
#include "parameter_footprints.hpp"
#include "thread_completion_analysis.hpp"

#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
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

#include <algorithm>
#include <cstdio>
#include <limits>
#include <map>
#include <string_view>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

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

        /// Prefix of an object designated by a pointer read out of what a parameter points to.
        constexpr std::string_view kReadPrefix = "read:";

        /// A pointer read `offset` bytes into what parameter `index` of `function` points to. It
        /// designates whatever each caller left there, which is not the parameter's own object. A
        /// read is settleable when it reads a pointer at a known offset and nothing before it in
        /// its function may write memory the function does not own: only then can a caller's
        /// store be shown to be what it reads.
        struct ReadInParameter
        {
            const llvm::Function* function = nullptr;
            unsigned index = 0;
            std::int64_t offset = 0;
            bool settleable = false;
        };

        /// What a spawn, or a call handing the object on, passes. `key` compares two of them;
        /// an object only a construction site further up can name has a parameter key and no
        /// binding yet, or a read key and the read its callers may settle.
        struct ObjectIdentity
        {
            std::string key;
            std::optional<SharedObjectBinding> binding;
            std::optional<ReadInParameter> read;
            /// Set when the object stands for a read no proof settled: the historical identity,
            /// the object the call passes for the parameter read from, kept so that no alert made
            /// before reads were told apart is lost. The thread may hold another object.
            bool keptForLackOfProof = false;
        };

        /// Whether `key` names an object only the calls above can settle: a parameter's, or the
        /// one a read of a parameter's memory designates.
        bool namedByCallers(std::string_view key)
        {
            return key.starts_with(kParameterPrefix) || key.starts_with(kReadPrefix);
        }

        ObjectIdentity readIdentity(const ReadInParameter& read)
        {
            return ObjectIdentity{
                .key = std::string(kReadPrefix) + functionId(*read.function) + ":" +
                       std::to_string(read.index) + ":" +
                       (read.settleable ? std::to_string(read.offset) : "?"),
                .read = read,
            };
        }

        struct SpawnedObject
        {
            std::string entryFunctionId;
            ObjectIdentity object;
            unsigned argumentIndex = 0;
            bool insideLoop = false;
            /// The pointer the spawn hands over, as the entry receives it.
            const llvm::Value* handed = nullptr;
            /// The entry, or the parameter of the spawning function it is taken from, which each
            /// call to that function settles.
            const llvm::Function* entry = nullptr;
            std::optional<unsigned> entryParameter;
        };

        /// A spawn whose entry is parameter `entryIndex` of `functionId`, on `object` as that
        /// function names it.
        struct ParameterEntrySpawn
        {
            const llvm::Instruction* spawn = nullptr;
            std::string functionId;
            unsigned entryIndex = 0;
            ObjectIdentity object;
        };

        /// The parameter an object key names: `arg:<function>:<index>`, followed by the path to
        /// the part of it the pointer designates, which resolving the parameter at a call drops.
        struct NamedParameter
        {
            std::string functionId;
            unsigned index = 0;
        };

        std::optional<NamedParameter> namedParameterOf(const std::string& key)
        {
            // The path holds no `:`, so the last one ends the function's name.
            const std::size_t separator = key.rfind(':');
            if (!key.starts_with(kParameterPrefix) || separator < kParameterPrefix.size())
                return std::nullopt;
            return NamedParameter{
                .functionId =
                    key.substr(kParameterPrefix.size(), separator - kParameterPrefix.size()),
                .index = static_cast<unsigned>(std::stoul(key.substr(separator + 1))),
            };
        }

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

        /// Whether `instruction` may write memory other than its function's own locals, which no
        /// caller's object can be.
        bool writesOutsideLocals(const llvm::Instruction& instruction)
        {
            if (!instruction.mayWriteToMemory())
                return false;
            const llvm::Value* written = nullptr;
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                written = store->getPointerOperand();
            else if (const auto* transfer = llvm::dyn_cast<llvm::MemIntrinsic>(&instruction))
                written = transfer->getRawDest();
            else if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&instruction);
                     intrinsic != nullptr && (intrinsic->isLifetimeStartOrEnd() ||
                                              llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic)))
                return false;
            return written == nullptr ||
                   !llvm::isa<llvm::AllocaInst>(llvm::getUnderlyingObject(written));
        }

        /// Whether anything that may run before `at` in its function may write memory other than
        /// the function's own locals: `at` included when a cycle brings it back.
        bool mayWriteBefore(const llvm::Instruction& at)
        {
            const llvm::BasicBlock* home = at.getParent();
            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> earlier;
            std::vector<const llvm::BasicBlock*> pending(llvm::pred_begin(home),
                                                         llvm::pred_end(home));
            while (!pending.empty())
            {
                const llvm::BasicBlock* block = pending.back();
                pending.pop_back();
                if (earlier.insert(block).second)
                    pending.insert(pending.end(), llvm::pred_begin(block), llvm::pred_end(block));
            }
            for (const llvm::BasicBlock* block : earlier)
                for (const llvm::Instruction& instruction : *block)
                    if (writesOutsideLocals(instruction))
                        return true;
            for (const llvm::Instruction& instruction : *home)
            {
                if (&instruction == &at)
                    return false;
                if (writesOutsideLocals(instruction))
                    return true;
            }
            return false;
        }

        /// The read `load` makes of what parameter `named` points to: settleable when it reads a
        /// pointer at a constant offset that nothing before it in its function may change.
        ObjectIdentity readIn(const llvm::LoadInst& load, const NamedParameter& named,
                              const llvm::DataLayout& layout)
        {
            const auto place = constantOffsetFrom(*load.getPointerOperand(), 0, layout);
            const auto* parameter = place ? llvm::dyn_cast<llvm::Argument>(place->first) : nullptr;
            if (parameter != nullptr && load.getType()->isPointerTy() && !mayWriteBefore(load))
            {
                return readIdentity(ReadInParameter{
                    .function = parameter->getParent(),
                    .index = parameter->getArgNo(),
                    .offset = place->second,
                    .settleable = true,
                });
            }
            return readIdentity(
                ReadInParameter{.function = load.getFunction(), .index = named.index});
        }

        /// The pointer one store left in the `size` bytes `offset` bytes into `object`, a local of
        /// the function making `call`, when `call` hands the local on. The local's address goes
        /// nowhere but into its own bytes, never read back here, and to `call`: no instruction
        /// unseen here, in this thread or another, can write it before the call. The store
        /// covers those bytes exactly, comes ahead of `call` on every path, and no other write
        /// to them can follow it before the call, which a cycle would bring back. Null otherwise.
        const llvm::Value* storedBefore(const llvm::AllocaInst& object, std::int64_t offset,
                                        std::uint64_t size, const llvm::CallBase& call,
                                        const llvm::DataLayout& layout,
                                        const llvm::DominatorTree& dominators)
        {
            struct Bytes
            {
                const llvm::Instruction* instruction = nullptr;
                std::optional<std::int64_t> begin;
                std::uint64_t size = 0;
            };
            constexpr std::uint64_t kUnbounded = std::numeric_limits<std::uint64_t>::max();
            const auto overlaps = [](const Bytes& bytes, std::int64_t begin, std::uint64_t size)
            {
                if (!bytes.begin || *bytes.begin < 0 || bytes.size == kUnbounded)
                    return true;
                const auto start = static_cast<std::uint64_t>(*bytes.begin);
                return start < static_cast<std::uint64_t>(begin) + size &&
                       static_cast<std::uint64_t>(begin) < start + bytes.size;
            };

            std::vector<Bytes> writes;
            std::vector<Bytes> reads;
            std::vector<std::int64_t> ownAddressAt;
            std::vector<std::pair<const llvm::Value*, std::optional<std::int64_t>>> pending{
                {&object, 0}};
            llvm::SmallPtrSet<const llvm::Value*, 32> seen;
            while (!pending.empty())
            {
                const auto [pointer, at] = pending.back();
                pending.pop_back();
                if (!seen.insert(pointer).second)
                    continue;
                for (const llvm::Use& use : pointer->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(user))
                    {
                        llvm::APInt step(layout.getIndexTypeSizeInBits(gep->getType()), 0);
                        const bool constant = at && gep->getPointerOperand() == pointer &&
                                              gep->accumulateConstantOffset(layout, step) &&
                                              step.getSignificantBits() <= 64;
                        const std::optional<std::int64_t> next =
                            constant ? llvm::checkedAdd(*at, step.getSExtValue()) : std::nullopt;
                        pending.emplace_back(gep, next);
                        continue;
                    }
                    if (llvm::isa<llvm::BitCastOperator, llvm::AddrSpaceCastInst>(user))
                    {
                        pending.emplace_back(user, at);
                        continue;
                    }
                    if (llvm::isa<llvm::ICmpInst>(user))
                        continue;
                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        reads.push_back(Bytes{load, at, layout.getTypeStoreSize(load->getType())});
                        continue;
                    }
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        const std::uint64_t stored =
                            layout.getTypeStoreSize(store->getValueOperand()->getType());
                        if (use.getOperandNo() == store->getPointerOperandIndex())
                        {
                            writes.push_back(Bytes{store, at, stored});
                            continue;
                        }
                        // The address kept in the local itself reaches no one the local does not.
                        const auto into =
                            constantOffsetFrom(*store->getPointerOperand(), 0, layout);
                        if (!into || into->first != &object)
                            return nullptr;
                        ownAddressAt.push_back(into->second);
                        continue;
                    }
                    const auto* other = llvm::dyn_cast<llvm::CallBase>(user);
                    if (other == nullptr || !other->isArgOperand(&use))
                        return nullptr;
                    if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(other);
                        intrinsic != nullptr && (intrinsic->isLifetimeStartOrEnd() ||
                                                 llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic)))
                        continue;
                    if (const auto* transfer = llvm::dyn_cast<llvm::MemIntrinsic>(other))
                    {
                        const auto* length =
                            llvm::dyn_cast<llvm::ConstantInt>(transfer->getLength());
                        const Bytes bytes{transfer, at,
                                          length != nullptr ? length->getZExtValue() : kUnbounded};
                        (other->getArgOperandNo(&use) == 0 ? writes : reads).push_back(bytes);
                        continue;
                    }
                    if (other != &call)
                        return nullptr;
                }
            }

            for (const Bytes& read : reads)
                for (const std::int64_t kept : ownAddressAt)
                    if (overlaps(read, kept, layout.getPointerSize()))
                        return nullptr;
            const llvm::BasicBlock* home = call.getParent();
            for (const llvm::BasicBlock* next : llvm::successors(home))
                if (llvm::isPotentiallyReachable(next, home, nullptr, &dominators))
                    return nullptr;

            const llvm::StoreInst* last = nullptr;
            for (const Bytes& write : writes)
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(write.instruction);
                if (store == nullptr || write.begin != offset || write.size != size ||
                    !dominators.dominates(store, &call))
                    continue;
                bool followed = false;
                for (const Bytes& other : writes)
                    followed =
                        followed || (other.instruction != store && overlaps(other, offset, size) &&
                                     llvm::isPotentiallyReachable(store, other.instruction, nullptr,
                                                                  &dominators) &&
                                     llvm::isPotentiallyReachable(other.instruction, &call, nullptr,
                                                                  &dominators));
                if (!followed)
                    last = store;
            }
            return last != nullptr ? last->getValueOperand()->stripPointerCasts() : nullptr;
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
            {
                // A pointer read out of what the parameter points to designates whatever the
                // caller left there, not the parameter's own object: each call settles it.
                if (const std::optional<NamedParameter> named = namedParameterOf(*storageId);
                    load != nullptr && named.has_value())
                    return readIn(*load, *named, layout);
                return ObjectIdentity{.key = std::move(*storageId)};
            }

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

            const std::optional<FunctionBinding> entry =
                resolveFunctionBinding(*call.getArgOperand(entryIndex));
            if (!entry.has_value() || (entry->function == nullptr && !entry->argumentIndex) ||
                (entry->function != nullptr &&
                 (entry->function->isDeclaration() || entry->function->arg_empty())))
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
                .entryFunctionId = entry->function != nullptr ? functionId(*entry->function) : "",
                .object = std::move(*identity),
                // Both spawn forms deliver the object on the entry's first parameter: the `void*`
                // of a pthread routine, and the `this` of a member function.
                .argumentIndex = 0,
                .insideLoop = insideLoop,
                .handed = object,
                .entry = entry->function,
                .entryParameter = entry->function != nullptr ? std::nullopt : entry->argumentIndex,
            };
        }

        /// The entry operand of each spawn whose entry is a parameter of the spawning function,
        /// by spawn and operand index: the parameter it passes.
        using ParameterSpawnOperands =
            std::map<std::pair<const llvm::CallBase*, unsigned>, const llvm::Argument*>;
        /// A parameter of a helper an entry is handed down through, by helper and index.
        using HandingParameter = std::pair<const llvm::Function*, unsigned>;
        /// A call handing an entry down, by call and the operand it hands it at.
        using HandingCall = std::pair<const llvm::CallBase*, unsigned>;

        /// Whether `entry` runs nowhere but at its direct calls and at the spawns its calls
        /// settle. Its address is followed through the slots holding it and into the parameters
        /// of the helpers it is handed to, `maxHelpers` deep along every way down, each handing
        /// call as a call settling a spawn resolves it. Anything else may run the entry where no
        /// spawn names its object: a call through a copy of its address, a store elsewhere than
        /// in a slot, a constant such as a table, code the unit does not define, a spawn no call
        /// settles, or a helper parameter handing it to a spawn on an object nothing names. A
        /// comparison runs nothing, and neither does a call of `repeatingCalls`: a recursion
        /// handing the entry back where the settling found it, with the same object, which
        /// leads to no run that parameter does not lead to already.
        bool runsOnlyWhereSettled(const llvm::Function& entry,
                                  const ParameterSpawnOperands& spawnOperands,
                                  const std::set<HandingParameter>& unnamedObjects,
                                  const std::set<HandingCall>& repeatingCalls,
                                  const ConcurrencySymbolClassifier& classifier,
                                  unsigned maxHelpers)
        {
            struct Flow
            {
                const llvm::Value* value = nullptr;
                /// The helper parameter the value carries the entry in, or null in the frame
                /// naming the entry itself.
                const llvm::Argument* parameter = nullptr;
                unsigned helpers = 0;
            };
            std::vector<Flow> pending = {Flow{.value = &entry}};
            // A value is visited once per parameter and depth: reached in fewer helpers, it would
            // vouch for a way down handing it past `maxHelpers`. The depth bounds the visits.
            std::set<std::tuple<const llvm::Value*, const llvm::Argument*, unsigned>> seen;
            while (!pending.empty())
            {
                const Flow flow = pending.back();
                pending.pop_back();
                if (!seen.emplace(flow.value, flow.parameter, flow.helpers).second)
                    continue;

                for (const llvm::Use& use : flow.value->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (llvm::isa<llvm::ICmpInst>(user))
                        continue;

                    // A slot holding the address hands it to every read of the slot.
                    if (llvm::isa<llvm::AllocaInst>(flow.value))
                    {
                        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                        {
                            pending.push_back(Flow{load, flow.parameter, flow.helpers});
                            continue;
                        }
                        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                            store != nullptr && store->getPointerOperand() == flow.value)
                        {
                            continue;
                        }
                    }
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        const auto* slot =
                            llvm::dyn_cast<llvm::AllocaInst>(store->getPointerOperand());
                        if (store->getValueOperand() != flow.value || slot == nullptr)
                            return false;
                        pending.push_back(Flow{slot, flow.parameter, flow.helpers});
                        continue;
                    }

                    const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (call == nullptr)
                        return false;
                    if (call->isCallee(&use))
                    {
                        // A direct call is a run each call names; a call through a copy is not.
                        if (flow.value == &entry)
                            continue;
                        return false;
                    }
                    if (!call->isArgOperand(&use))
                        return false;

                    const unsigned index = call->getArgOperandNo(&use);
                    if (const auto spawn = spawnOperands.find({call, index});
                        spawn != spawnOperands.end())
                    {
                        if (flow.parameter != nullptr && spawn->second == flow.parameter)
                            continue;
                        return false;
                    }
                    if (repeatingCalls.contains({call, index}))
                        continue;

                    const llvm::Function* helper = classifier.directCallee(*call);
                    const std::optional<FunctionBinding> handed =
                        resolveFunctionBinding(*call->getArgOperand(index));
                    const bool settledHere =
                        handed.has_value() &&
                        (flow.parameter == nullptr
                             ? handed->function == &entry
                             : handed->argumentIndex == flow.parameter->getArgNo() &&
                                   call->getFunction() == flow.parameter->getParent());
                    if (helper == nullptr || helper->isDeclaration() ||
                        index >= helper->arg_size() || !settledHere || flow.helpers >= maxHelpers ||
                        unnamedObjects.contains({helper, index}))
                    {
                        return false;
                    }
                    pending.push_back(
                        Flow{helper->getArg(index), helper->getArg(index), flow.helpers + 1});
                }
            }
            return true;
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
        // Every read of a parameter's memory seen, by key: what its callers settle.
        std::map<std::string, ReadInParameter> readsByKey;
        const auto remember = [&](const ObjectIdentity& identity)
        {
            if (identity.binding.has_value())
                bindingsByKey.emplace(identity.key, *identity.binding);
            if (identity.read.has_value())
                readsByKey.emplace(identity.key, *identity.read);
        };
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
        // Spawns whose entry is a parameter of the spawning function, settled at its calls, and
        // the operand each takes its entry from.
        std::vector<ParameterEntrySpawn> parameterEntrySpawns;
        ParameterSpawnOperands parameterSpawnOperands;

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

                    const CallKind kind = classifier_.classify(*call);
                    const std::optional<SpawnedObject> spawned =
                        spawnedObjectAt(*call, kind, insideLoop, layout, programDefined);
                    if (!spawned.has_value())
                        continue;

                    remember(spawned->object);
                    if (spawned->entryParameter.has_value())
                    {
                        parameterEntrySpawns.push_back(ParameterEntrySpawn{
                            .spawn = &instruction,
                            .functionId = functionId(function),
                            .entryIndex = *spawned->entryParameter,
                            .object = spawned->object,
                        });
                        const unsigned entryOperand = kind == CallKind::PThreadCreate
                                                          ? kPThreadEntryOperandIndex
                                                          : kStdThreadCallableOperandIndex;
                        parameterSpawnOperands.emplace(std::pair{call, entryOperand},
                                                       function.getArg(*spawned->entryParameter));
                        continue;
                    }

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
        // What `site` passes as its argument `index`, kept so that a binding can be made of it.
        const auto identityPassedAt = [&](const DirectCallSite& site,
                                          unsigned index) -> std::optional<ObjectIdentity>
        {
            if (site.call == nullptr || site.call->arg_size() <= index)
                return std::nullopt;
            std::optional<ObjectIdentity> identity =
                objectIdentityOf(*site.call->getArgOperand(index), layout, programDefined);
            if (identity.has_value())
                remember(*identity);
            return identity;
        };
        // What `read` designates as `site` leaves it: the pointer the caller stored there, when a
        // store of its own provably did; the same read a frame up, when the caller hands on what
        // its own parameter points to and writes nothing before the call. Otherwise the object
        // the call passes is kept for lack of proof: the identity reads had before they were told
        // apart, which the thread may not hold.
        const auto readAt = [&](const DirectCallSite& site,
                                const ReadInParameter& read) -> std::optional<ObjectIdentity>
        {
            if (site.call == nullptr || site.call->arg_size() <= read.index)
                return std::nullopt;
            std::optional<std::pair<const llvm::Value*, std::int64_t>> place;
            if (read.settleable)
                place =
                    constantOffsetFrom(*site.call->getArgOperand(read.index), read.offset, layout);
            const auto* parameter = place ? llvm::dyn_cast<llvm::Argument>(place->first) : nullptr;
            if (parameter != nullptr && !mayWriteBefore(*site.call))
            {
                ObjectIdentity above = readIdentity(ReadInParameter{
                    .function = parameter->getParent(),
                    .index = parameter->getArgNo(),
                    .offset = place->second,
                    .settleable = true,
                });
                remember(above);
                return above;
            }
            const auto* local = place ? llvm::dyn_cast<llvm::AllocaInst>(place->first) : nullptr;
            const llvm::Value* stored =
                local != nullptr
                    ? storedBefore(*local, place->second, layout.getPointerSize(), *site.call,
                                   layout, analyses_.getDominatorTree(*site.call->getFunction()))
                    : nullptr;
            if (stored != nullptr)
                if (std::optional<ObjectIdentity> identity =
                        objectIdentityOf(*stored, layout, programDefined))
                {
                    remember(*identity);
                    return identity;
                }
            std::optional<ObjectIdentity> kept = identityPassedAt(site, read.index);
            if (kept.has_value())
                kept->keptForLackOfProof = true;
            return kept;
        };
        // The objects the calls settle `parameterId` to; `kept` is set when one of them is only
        // kept for lack of proof.
        const auto callersOf = [&](const std::string& parameterId, bool& kept)
        {
            std::set<std::string> resolved;
            if (const auto read = readsByKey.find(parameterId); read != readsByKey.end())
            {
                for (const DirectCallSite& site : directCallSites)
                {
                    if (site.calleeFunctionId != functionId(*read->second.function))
                        continue;
                    if (std::optional<ObjectIdentity> caller = readAt(site, read->second))
                    {
                        kept = kept || caller->keptForLackOfProof;
                        resolved.insert(std::move(caller->key));
                    }
                }
                return resolved;
            }
            const std::optional<NamedParameter> parameter = namedParameterOf(parameterId);
            if (!parameter.has_value())
                return resolved;

            for (const DirectCallSite& site : directCallSites)
            {
                if (site.calleeFunctionId != parameter->functionId)
                    continue;

                if (std::optional<ObjectIdentity> caller = identityPassedAt(site, parameter->index))
                    resolved.insert(std::move(caller->key));
            }

            return resolved;
        };

        // An entry started on two different objects gets no identity at all. Its body is one
        // function, so every access in it would otherwise be attributed to whichever object was
        // seen first, and two threads working on their own copies would be reported as racing.
        std::map<std::string, std::set<std::string>> objectsByEntry;
        for (const auto& [key, instructions] : sites)
            objectsByEntry[key.first].insert(key.second);

        // A spawn whose entry is a parameter runs what each call passes there, on what the same
        // call passes for the object: both are settled at that call, together, so that two calls
        // handing one helper different entries and objects keep each entry on its own object. An
        // entry the call takes from a parameter of its own is settled a frame up, within the bound
        // objects are; anything else it passes names no entry, and the spawn is left out there as
        // a spawn on an unresolved entry is.
        std::map<std::string, std::set<std::string>> settledObjectsByEntry;
        std::map<std::string, const llvm::Function*> settledEntries;
        // Entries a call settles on an object kept for lack of proof.
        std::set<std::string> settledOnKeptObject;
        // Entries a call hands to a spawn on an object no call names, and the helper parameters
        // handing an entry down to one: a thread may then hold any object.
        std::set<std::string> startedOnUnnamedObject;
        std::set<HandingParameter> handingToUnnamedObject;
        // Calls of a recursion handing a spawn's entry back to the parameter it is settled from,
        // with the spawn's object: the state a frame up is the one being settled, which settles
        // nothing new however deep the recursion. A call repeating the state of one spawn and not
        // of another repeats none.
        std::set<HandingCall> repeatingCalls;
        std::set<HandingCall> advancingCalls;
        std::vector<std::pair<ParameterEntrySpawn, unsigned>> unsettled;
        for (ParameterEntrySpawn& spawn : parameterEntrySpawns)
            unsettled.emplace_back(std::move(spawn), 0);
        while (!unsettled.empty())
        {
            auto [spawn, frames] = std::move(unsettled.back());
            unsettled.pop_back();
            for (const DirectCallSite& site : directCallSites)
            {
                if (site.calleeFunctionId != spawn.functionId || site.call == nullptr ||
                    site.call->arg_size() <= spawn.entryIndex)
                    continue;
                const std::optional<FunctionBinding> entry =
                    resolveFunctionBinding(*site.call->getArgOperand(spawn.entryIndex));
                if (!entry.has_value())
                    continue;

                // The object arrived, if on a parameter, on one of the function the spawn is
                // settled in: the call passes the object itself.
                std::optional<ObjectIdentity> object = spawn.object;
                if (const std::optional<NamedParameter> parameter =
                        namedParameterOf(spawn.object.key))
                {
                    object = parameter->functionId == spawn.functionId
                                 ? identityPassedAt(site, parameter->index)
                                 : std::nullopt;
                }
                else if (spawn.object.read.has_value())
                {
                    object = functionId(*spawn.object.read->function) == spawn.functionId
                                 ? readAt(site, *spawn.object.read)
                                 : std::nullopt;
                }
                if (!object.has_value())
                {
                    // The thread may hold any object: whatever entry this call hands down is
                    // bound to none.
                    if (entry->function != nullptr)
                        startedOnUnnamedObject.insert(functionId(*entry->function));
                    else if (entry->argumentIndex.has_value())
                        handingToUnnamedObject.emplace(site.call->getFunction(),
                                                       *entry->argumentIndex);
                    continue;
                }
                // An object kept for lack of proof a frame down stays so, whatever names it here.
                object->keptForLackOfProof =
                    object->keptForLackOfProof || spawn.object.keptForLackOfProof;

                if (entry->function == nullptr)
                {
                    if (!entry->argumentIndex.has_value())
                        continue;
                    if (site.callerFunctionId == spawn.functionId &&
                        *entry->argumentIndex == spawn.entryIndex)
                    {
                        const bool repeats =
                            object->key == spawn.object.key &&
                            object->keptForLackOfProof == spawn.object.keptForLackOfProof;
                        (repeats ? repeatingCalls : advancingCalls)
                            .emplace(site.call, spawn.entryIndex);
                        if (repeats)
                            continue;
                    }
                    if (frames + 1 < kMaxCallerRounds)
                    {
                        unsettled.emplace_back(
                            ParameterEntrySpawn{
                                .spawn = spawn.spawn,
                                .functionId = site.callerFunctionId,
                                .entryIndex = *entry->argumentIndex,
                                .object = std::move(*object),
                            },
                            frames + 1);
                    }
                    continue;
                }
                if (entry->function->isDeclaration() || entry->function->arg_empty())
                    continue;

                const std::string entryFunctionId = functionId(*entry->function);
                settledObjectsByEntry[entryFunctionId].insert(object->key);
                settledEntries.emplace(entryFunctionId, entry->function);
                if (object->keptForLackOfProof)
                    settledOnKeptObject.insert(entryFunctionId);
                // It hands the object out too, which rules out an element of its own per thread.
                sitesByObject[object->key].insert(spawn.spawn);
            }
        }
        std::erase_if(repeatingCalls,
                      [&](const HandingCall& call) { return advancingCalls.contains(call); });

        // The spawns settled at calls name an entry only when no spawn names it directly. One
        // that does keeps the identity it has: a call settling the entry on another object would
        // otherwise withdraw it under the rule above, and lose the races its own threads make on
        // the object it names. The settled threads' accesses then count as made on that object,
        // as they did while their spawns were left out (#239). A settled spawn grants none of the
        // exemptions a constructor's spawn on its own object does.
        std::set<std::string> settledOnly;
        // Entries whose object is kept for lack of proof, at a call or a frame above.
        std::set<std::string> keptEntries;
        for (auto& [entryFunctionId, objects] : settledObjectsByEntry)
        {
            if (!objectsByEntry.emplace(entryFunctionId, std::move(objects)).second)
                continue;
            settledOnly.insert(entryFunctionId);
            if (settledOnKeptObject.contains(entryFunctionId))
                keptEntries.insert(entryFunctionId);
        }

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
            // A spawn a call settles binds its entry only where the unit sees every run of it:
            // the entry's address reaches nothing but its direct calls and the spawns calls
            // settle, each on an object a call names. Run anywhere else, the entry may hold
            // another object, under another lock, and is bound to none (#235).
            const bool settled = settledOnly.contains(entryFunctionId);
            if (settled && (startedOnUnnamedObject.contains(entryFunctionId) ||
                            !runsOnlyWhereSettled(*settledEntries.at(entryFunctionId),
                                                  parameterSpawnOperands, handingToUnnamedObject,
                                                  repeatingCalls, classifier_, kMaxCallerRounds)))
            {
                continue;
            }

            std::set<std::string> candidates = objects;
            // The candidates the last round started from, and the functions whose parameters
            // carried the object up to the calls naming it.
            std::set<std::string> lastRound;
            std::set<std::string> carriers;
            bool kept = keptEntries.contains(entryFunctionId);
            for (unsigned round = 0; round < kMaxCallerRounds; ++round)
            {
                std::set<std::string> next;
                bool parameterised = false;
                for (const std::string& candidate : candidates)
                {
                    if (!std::string_view(candidate).starts_with(kParameterPrefix) &&
                        !readsByKey.contains(candidate))
                    {
                        next.insert(candidate);
                        continue;
                    }

                    parameterised = true;
                    if (const auto read = readsByKey.find(candidate); read != readsByKey.end())
                        carriers.insert(functionId(*read->second.function));
                    else if (const std::optional<NamedParameter> parameter =
                                 namedParameterOf(candidate))
                        carriers.insert(parameter->functionId);
                    const std::set<std::string> callers = callersOf(candidate, kept);
                    next.insert(callers.begin(), callers.end());
                }

                lastRound = std::exchange(candidates, std::move(next));
                if (!parameterised)
                    break;
            }

            // Still moving once the rounds are spent, the object comes from callers further up,
            // which may pass any: an entry calls settle is bound to none. A candidate that only
            // stays parameterised stands for the objects already named, passed back down.
            if (settled && candidates != lastRound &&
                std::any_of(candidates.begin(), candidates.end(), namedByCallers))
            {
                continue;
            }

            // Anything still parameterised is an object no construction site names, and more
            // than one candidate means the object depends on the caller. Neither is an identity
            // two accesses can be judged against.
            std::erase_if(candidates, namedByCallers);
            if (candidates.size() != 1)
                continue;

            const auto bindingIt = bindingsByKey.find(*candidates.begin());
            if (bindingIt == bindingsByKey.end())
                continue;

            SharedObjectBinding binding = bindingIt->second;
            binding.argumentIndex = 0;
            binding.keptForLackOfProof = kept;
            if (settled)
            {
                binding.settledThrough.push_back(settledEntries.at(entryFunctionId));
                for (const llvm::Function& function : module)
                {
                    if (carriers.contains(functionId(function)))
                        binding.settledThrough.push_back(&function);
                }
            }
            // An object kept for lack of proof grants no exemption: the thread may hold another
            // one, which another spawn's elements, or a constructor's object, say nothing of.
            if (kept)
            {
                bindings.emplace(entryFunctionId, std::move(binding));
                continue;
            }
            if (const auto classes = constructedClasses.find(entryFunctionId);
                classes != constructedClasses.end() && classes->second.size() == 1 &&
                !startedOtherwise.contains(entryFunctionId))
                binding.constructedClass = *classes->second.begin();

            // Each thread has an element of its own only when this is the one spawn of the
            // entry and the one spawn handing out the object's elements: two spawns may hand
            // the same element to two threads. A spawn settled at a call is not in the program's
            // entry, where the elements are handed out.
            const auto objectSitesIt = sitesByObject.find(*candidates.begin());
            if (!settledOnly.contains(entryFunctionId) && objectSitesIt != sitesByObject.end() &&
                objectSitesIt->second.size() == 1 && spawnedOnce(entryFunctionId))
            {
                if (const auto elementsIt = elementsBySite.find(*objectSitesIt->second.begin());
                    elementsIt != elementsBySite.end())
                {
                    binding.elementPerThread = elementsIt->second;
                }
            }
            bindings.emplace(entryFunctionId, std::move(binding));
        }

        for (const DirectCallSite& site : directCallSites)
        {
            if (const auto bindingIt = bindings.find(site.calleeFunctionId);
                bindingIt != bindings.end())
            {
                bindingIt->second.runByDirectCalls = true;
            }
        }

        return bindings;
    }
} // namespace ctrace::concurrency::internal::analysis
