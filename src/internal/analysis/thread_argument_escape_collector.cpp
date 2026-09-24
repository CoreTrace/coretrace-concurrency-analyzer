// SPDX-License-Identifier: Apache-2.0
#include "thread_argument_escape_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include <unordered_map>
#include <utility>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        constexpr unsigned kHandleOperandIndex = 0;
        constexpr unsigned kEntryOperandIndex = 2;
        constexpr unsigned kArgumentOperandIndex = 3;
        /// A `std::thread` constructor takes the thread object, then the callable, then the
        /// arguments, each by reference.
        constexpr unsigned kStdThreadCallableOperandIndex = 1;

        struct JoinSite
        {
            const llvm::Instruction* instruction = nullptr;
            std::string handleGroupId;
        };

        /// True when every way out of the function passes a join of this handle first, which is
        /// what keeps the frame alive at least as long as the thread.
        ///
        /// A function with no return at all — an infinite loop, or one that always exits the
        /// process — vacuously satisfies this: its frame never goes away.
        bool joinPrecedesEveryReturn(const llvm::Function& function,
                                     const std::string& handleGroupId,
                                     const std::vector<JoinSite>& joins,
                                     const llvm::DominatorTree& dominatorTree)
        {
            for (const llvm::BasicBlock& block : function)
            {
                const llvm::Instruction* terminator = block.getTerminator();
                if (!llvm::isa<llvm::ReturnInst>(terminator))
                    continue;

                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                const bool joined =
                    std::any_of(joins.begin(), joins.end(),
                                [&](const JoinSite& join)
                                {
                                    return join.handleGroupId == handleGroupId &&
                                           dominatorTree.dominates(join.instruction, terminator);
                                });
                if (!joined)
                    return false;
            }

            return true;
        }

        /// A local of `function` whose address is stored into `object`, directly or into one of
        /// its fields: a lambda closure capturing by reference, a temporary holding `&local`, a
        /// `std::reference_wrapper`.
        const llvm::AllocaInst* localStoredInto(const llvm::AllocaInst& object,
                                                const llvm::Function& function)
        {
            std::vector<const llvm::Value*> pending{&object};
            llvm::SmallPtrSet<const llvm::Value*, 8> visited;
            while (!pending.empty())
            {
                const llvm::Value* address = pending.back();
                pending.pop_back();
                if (!visited.insert(address).second)
                    continue;

                for (const llvm::User* user : address->users())
                {
                    if (llvm::isa<llvm::GetElementPtrInst>(user) ||
                        llvm::isa<llvm::BitCastInst>(user))
                    {
                        pending.push_back(user);
                        continue;
                    }

                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (store == nullptr || store->getPointerOperand() != address)
                        continue;

                    const auto* stored = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                        llvm::getUnderlyingObject(store->getValueOperand()));
                    if (stored != nullptr && stored != &object &&
                        stored->getFunction() == &function)
                        return stored;
                }
            }
            return nullptr;
        }

        /// The local of `function` a new thread may keep reading after the creator returns.
        ///
        /// `pthread_create` hands the thread its argument pointer as is. `std::thread` copies its
        /// callable and arguments into the new thread, so a local passed directly is copied and
        /// safe; what escapes is an address stored inside one of them.
        const llvm::AllocaInst* escapingLocal(const llvm::CallBase& creation, CallKind kind,
                                              const llvm::Function& function)
        {
            if (kind == CallKind::PThreadCreate)
            {
                // getUnderlyingObject follows the indexing, so `&local` and `&local.field` and
                // `&array[0]` all lead back to the same allocation.
                const auto* local = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                    llvm::getUnderlyingObject(creation.getArgOperand(kArgumentOperandIndex)));
                return local != nullptr && local->getFunction() == &function ? local : nullptr;
            }

            for (unsigned index = kStdThreadCallableOperandIndex; index < creation.arg_size();
                 ++index)
            {
                const auto* passed = llvm::dyn_cast_or_null<llvm::AllocaInst>(
                    llvm::getUnderlyingObject(creation.getArgOperand(index)));
                if (passed == nullptr || passed->getFunction() != &function)
                    continue;
                if (const llvm::AllocaInst* local = localStoredInto(*passed, function))
                    return local;
            }
            return nullptr;
        }

        /// True when `first` and `second` are the same pointer: the same value, or two reads of
        /// one local slot that nothing writes between them. Without optimization a pointer
        /// variable lives in such a slot and every use reloads it.
        bool samePointer(const llvm::Value& first, const llvm::Value& second,
                         const llvm::Instruction& between, const llvm::DominatorTree& dominators)
        {
            if (first.stripPointerCasts() == second.stripPointerCasts())
                return true;

            const auto* firstLoad = llvm::dyn_cast<llvm::LoadInst>(first.stripPointerCasts());
            const auto* secondLoad = llvm::dyn_cast<llvm::LoadInst>(second.stripPointerCasts());
            if (firstLoad == nullptr || secondLoad == nullptr)
                return false;

            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                firstLoad->getPointerOperand()->stripPointerCasts());
            if (slot == nullptr || secondLoad->getPointerOperand()->stripPointerCasts() != slot)
                return false;

            // Every write to the slot must come before the first read or after `between`, the
            // point that ends what the pointer designated; a slot whose address is taken may
            // change behind the analysis.
            for (const llvm::User* user : slot->users())
            {
                if (llvm::isa<llvm::LoadInst>(user))
                    continue;

                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != slot)
                    return false;
                if (!dominators.dominates(store, firstLoad) &&
                    !dominators.dominates(&between, store))
                {
                    return false;
                }
            }
            return true;
        }

        /// True when the thread `creation` starts is joined, every instance of it, before `point`:
        /// the join-range proof covers it, or a join of the same handle dominates the point.
        bool joinedBefore(const llvm::CallBase& creation, const llvm::Instruction& point,
                          const std::vector<JoinSite>& joins,
                          const ThreadCompletionMap& completions,
                          const llvm::DominatorTree& dominators)
        {
            if (const auto completion = completions.find(&creation);
                completion != completions.end() && dominators.dominates(completion->second, &point))
            {
                return true;
            }

            const std::optional<std::string> handleGroupId =
                canonicalStorageGroupId(*creation.getArgOperand(kHandleOperandIndex));
            if (!handleGroupId.has_value())
                return false;
            for (const JoinSite& join : joins)
            {
                if (join.handleGroupId == *handleGroupId &&
                    dominators.dominates(join.instruction, &point))
                {
                    return true;
                }
            }
            return false;
        }

        /// The one store to a local slot, when the slot is only stored to once and loaded from.
        /// Without optimization every temporary lives in such a slot.
        const llvm::StoreInst* soleStoreTo(const llvm::AllocaInst& slot)
        {
            const llvm::StoreInst* found = nullptr;
            for (const llvm::User* user : slot.users())
            {
                if (llvm::isa<llvm::LoadInst>(user))
                    continue;
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != &slot || found != nullptr)
                    return nullptr;
                found = store;
            }
            return found;
        }

        constexpr unsigned kMaxSlotHops = 8;

        /// The thread-local object a pointer value designates, following the local slots an
        /// unoptimized build copies it through.
        const llvm::GlobalVariable* threadLocalBehind(const llvm::Value& value)
        {
            const llvm::Value* current = &value;
            for (unsigned hop = 0; hop < kMaxSlotHops && current != nullptr; ++hop)
            {
                current = current->stripPointerCasts();
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    current = gep->getPointerOperand();
                    continue;
                }
                if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(current);
                    intrinsic != nullptr &&
                    intrinsic->getIntrinsicID() == llvm::Intrinsic::threadlocal_address)
                {
                    current = intrinsic->getArgOperand(0);
                    continue;
                }
                if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                    return global->isThreadLocal() ? global : nullptr;

                const auto* load = llvm::dyn_cast<llvm::LoadInst>(current);
                const auto* slot = load != nullptr
                                       ? llvm::dyn_cast<llvm::AllocaInst>(
                                             load->getPointerOperand()->stripPointerCasts())
                                       : nullptr;
                const llvm::StoreInst* store = slot != nullptr ? soleStoreTo(*slot) : nullptr;
                current = store != nullptr ? store->getValueOperand() : nullptr;
            }
            return nullptr;
        }

        /// True when a pointer value was read from `pointer`, directly or through the local
        /// slots an unoptimized build copies it through.
        bool readFrom(const llvm::Value& value, const llvm::GlobalVariable& pointer)
        {
            const llvm::Value* current = &value;
            for (unsigned hop = 0; hop < kMaxSlotHops && current != nullptr; ++hop)
            {
                current = current->stripPointerCasts();
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    current = gep->getPointerOperand();
                    continue;
                }
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(current);
                if (load == nullptr)
                    return false;

                const llvm::Value* source = load->getPointerOperand()->stripPointerCasts();
                if (source == &pointer)
                    return true;
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(source);
                const llvm::StoreInst* store = slot != nullptr ? soleStoreTo(*slot) : nullptr;
                current = store != nullptr ? store->getValueOperand() : nullptr;
            }
            return false;
        }

        /// A global pointer a thread entry fills with the address of one of its thread-locals.
        struct ThreadLocalPublication
        {
            const llvm::GlobalVariable* threadLocal = nullptr;
            const llvm::Function* entry = nullptr;
            const llvm::StoreInst* store = nullptr;
        };

        /// The global pointers whose only non-null value is a thread-local address stored by one
        /// function, so that a non-null value read from one designates that thread-local. A
        /// pointer whose own address is taken, or that receives anything else, may hold another
        /// object and is left out.
        std::unordered_map<const llvm::GlobalVariable*, ThreadLocalPublication>
        threadLocalPublications(const llvm::Module& module)
        {
            std::unordered_map<const llvm::GlobalVariable*, ThreadLocalPublication> publications;
            for (const llvm::GlobalVariable& pointer : module.globals())
            {
                if (pointer.isThreadLocal() || pointer.isConstant() ||
                    !pointer.getValueType()->isPointerTy())
                {
                    continue;
                }

                ThreadLocalPublication publication;
                bool qualifies = true;
                for (const llvm::User* user : pointer.users())
                {
                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        qualifies = load->getPointerOperand() == &pointer;
                    }
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() != &pointer)
                        {
                            qualifies = false;
                        }
                        else if (!llvm::isa<llvm::ConstantPointerNull>(store->getValueOperand()))
                        {
                            const llvm::GlobalVariable* threadLocal =
                                threadLocalBehind(*store->getValueOperand());
                            qualifies = threadLocal != nullptr && publication.store == nullptr;
                            publication = ThreadLocalPublication{.threadLocal = threadLocal,
                                                                 .entry = store->getFunction(),
                                                                 .store = store};
                        }
                    }
                    else
                    {
                        qualifies = false;
                    }
                    if (!qualifies)
                        break;
                }

                if (qualifies && publication.store != nullptr)
                    publications.emplace(&pointer, publication);
            }
            return publications;
        }

        /// True when a function only ever runs as a thread entry: nothing calls it directly, so
        /// its thread-locals are those of the threads it starts.
        bool onlyRunsAsThreadEntry(const llvm::Function& function)
        {
            for (const llvm::User* user : function.users())
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                if (call != nullptr && call->getCalledOperand() == &function)
                    return false;
            }
            return true;
        }

        /// True when a thread entry reads or writes through its argument, or hands it on.
        bool usesArgument(const llvm::Function& entry, const llvm::DataLayout& layout)
        {
            if (entry.isDeclaration() || entry.arg_empty())
                return false;

            auto designatesArgument = [&](const llvm::Value& pointer)
            {
                const std::optional<RootBinding> root = resolveTrackedRoot(pointer, &layout, 0);
                return root.has_value() && root->kind == RootBindingKind::Argument &&
                       root->argumentIndex == 0;
            };

            for (const llvm::BasicBlock& block : entry)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    {
                        if (designatesArgument(*load->getPointerOperand()))
                            return true;
                    }
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (designatesArgument(*store->getPointerOperand()))
                            return true;
                    }
                    else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        for (const llvm::Use& operand : call->args())
                        {
                            if (operand->getType()->isPointerTy() && designatesArgument(*operand))
                                return true;
                        }
                    }
                }
            }
            return false;
        }
    } // namespace

    ThreadArgumentEscapeCollector::ThreadArgumentEscapeCollector(
        const ConcurrencySymbolClassifier& classifier, LlvmFunctionAnalysisProvider& analyses)
        : classifier_(classifier), analyses_(analyses)
    {
    }

    ThreadLifetimeFacts
    ThreadArgumentEscapeCollector::collect(const llvm::Module& module,
                                           const ThreadCompletionMap& completions) const
    {
        ThreadLifetimeFacts facts;
        const auto publications = threadLocalPublications(module);

        // Every place the unit starts each entry, wherever it is: a thread-local is only known
        // to be gone once all the threads that could have published it are.
        std::unordered_map<const llvm::Function*, std::size_t> startsByEntry;
        for (const llvm::Function& function : module)
        {
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;
                    const CallKind kind = classifier_.classify(*call);
                    const unsigned entryIndex = kind == CallKind::PThreadCreate
                                                    ? kEntryOperandIndex
                                                    : kStdThreadCallableOperandIndex;
                    if ((kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor) &&
                        call->arg_size() > entryIndex)
                    {
                        if (const llvm::Function* entry =
                                resolveFunctionValue(*call->getArgOperand(entryIndex)))
                        {
                            ++startsByEntry[entry];
                        }
                    }
                }
            }
        }

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            std::vector<JoinSite> joins;
            std::vector<std::pair<const llvm::CallBase*, CallKind>> creations;
            std::vector<const llvm::CallBase*> frees;
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_size() <= kHandleOperandIndex)
                        continue;

                    if (classifier_.releasesMemory(*call))
                    {
                        frees.push_back(call);
                        continue;
                    }

                    const CallKind kind = classifier_.classify(*call);
                    if (kind == CallKind::PThreadJoin || kind == CallKind::StdThreadJoin)
                    {
                        if (std::optional<std::string> handleGroupId =
                                canonicalStorageGroupId(*call->getArgOperand(kHandleOperandIndex)))
                        {
                            joins.push_back(JoinSite{
                                .instruction = &instruction,
                                .handleGroupId = std::move(*handleGroupId),
                            });
                        }
                        continue;
                    }

                    if ((kind == CallKind::PThreadCreate &&
                         call->arg_size() > kArgumentOperandIndex) ||
                        (kind == CallKind::StdThreadCtor &&
                         call->arg_size() > kStdThreadCallableOperandIndex))
                    {
                        creations.emplace_back(call, kind);
                    }
                }
            }

            if (creations.empty())
                continue;

            const llvm::DominatorTree& dominatorTree = analyses_.getDominatorTree(function);

            for (const auto& [creation, kind] : creations)
            {
                const std::optional<std::string> handleGroupId =
                    canonicalStorageGroupId(*creation->getArgOperand(kHandleOperandIndex));
                const llvm::Function* entry = resolveFunctionValue(*creation->getArgOperand(
                    kind == CallKind::PThreadCreate ? kEntryOperandIndex
                                                    : kStdThreadCallableOperandIndex));
                const ResolvedSourceLocations locations = resolveSourceLocations(*creation);

                const bool escapes =
                    !completions.contains(creation) &&
                    escapingLocal(*creation, kind, function) != nullptr &&
                    !(handleGroupId.has_value() &&
                      joinPrecedesEveryReturn(function, *handleGroupId, joins, dominatorTree));
                if (escapes)
                {
                    facts.escapes.push_back(ThreadArgumentEscapeFact{
                        .functionId = functionId(function),
                        .entryFunctionId =
                            entry != nullptr ? functionDisplayName(*entry) : std::string(),
                        .location = locations.userLocation,
                    });
                }

                // A heap argument the creator frees while the thread may still use it.
                if (kind != CallKind::PThreadCreate || entry == nullptr ||
                    !usesArgument(*entry, module.getDataLayout()))
                {
                    continue;
                }

                const llvm::Value& argument = *creation->getArgOperand(kArgumentOperandIndex);
                for (const llvm::CallBase* release : frees)
                {
                    if (!dominatorTree.dominates(creation, release) ||
                        joinedBefore(*creation, *release, joins, completions, dominatorTree) ||
                        !samePointer(argument, *release->getArgOperand(0), *release, dominatorTree))
                    {
                        continue;
                    }

                    facts.frees.push_back(ThreadArgumentFreeFact{
                        .functionId = functionId(function),
                        .entryFunctionId = functionDisplayName(*entry),
                        .creationLocation = locations.userLocation,
                        .freeLocation = resolveSourceLocations(*release).userLocation,
                    });
                    break;
                }
            }

            // A thread-local reached through the pointer its thread filled, once every thread
            // that could have filled it has been joined.
            for (const auto& [pointer, publication] : publications)
            {
                std::vector<const llvm::CallBase*> entryCreations;
                for (const auto& [creation, kind] : creations)
                {
                    if (resolveFunctionValue(*creation->getArgOperand(
                            kind == CallKind::PThreadCreate ? kEntryOperandIndex
                                                            : kStdThreadCallableOperandIndex)) ==
                        publication.entry)
                    {
                        entryCreations.push_back(creation);
                    }
                }
                if (entryCreations.empty() || !onlyRunsAsThreadEntry(*publication.entry) ||
                    entryCreations.size() != startsByEntry[publication.entry])
                {
                    continue;
                }

                for (const llvm::BasicBlock& block : function)
                {
                    const llvm::Instruction* use = nullptr;
                    for (const llvm::Instruction& instruction : block)
                    {
                        const llvm::Value* address = nullptr;
                        if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                            address = load->getPointerOperand();
                        else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                            address = store->getPointerOperand();
                        if (address == nullptr || address == pointer ||
                            !readFrom(*address, *pointer))
                        {
                            continue;
                        }

                        bool everyThreadEnded = true;
                        for (const llvm::CallBase* creation : entryCreations)
                        {
                            everyThreadEnded =
                                everyThreadEnded && joinedBefore(*creation, instruction, joins,
                                                                 completions, dominatorTree);
                        }
                        if (everyThreadEnded)
                        {
                            use = &instruction;
                            break;
                        }
                    }
                    if (use == nullptr)
                        continue;

                    facts.expiredThreadLocals.push_back(ExpiredThreadLocalFact{
                        .functionId = functionId(function),
                        .entryFunctionId = functionDisplayName(*publication.entry),
                        .threadLocal = publication.threadLocal->getName().str(),
                        .pointer = pointer->getName().str(),
                        .publishLocation = resolveSourceLocations(*publication.store).userLocation,
                        .useLocation = resolveSourceLocations(*use).userLocation,
                    });
                    break;
                }
            }
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
