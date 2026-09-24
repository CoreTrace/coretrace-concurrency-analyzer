// SPDX-License-Identifier: Apache-2.0
#include "thread_argument_escape_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

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
    } // namespace

    ThreadArgumentEscapeCollector::ThreadArgumentEscapeCollector(
        const ConcurrencySymbolClassifier& classifier, LlvmFunctionAnalysisProvider& analyses)
        : classifier_(classifier), analyses_(analyses)
    {
    }

    std::vector<ThreadArgumentEscapeFact>
    ThreadArgumentEscapeCollector::collect(const llvm::Module& module,
                                           const ThreadCompletionMap& completions) const
    {
        std::vector<ThreadArgumentEscapeFact> facts;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            std::vector<JoinSite> joins;
            std::vector<std::pair<const llvm::CallBase*, CallKind>> creations;
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_size() <= kHandleOperandIndex)
                        continue;

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
                if (completions.contains(creation) ||
                    escapingLocal(*creation, kind, function) == nullptr)
                {
                    continue;
                }

                const std::optional<std::string> handleGroupId =
                    canonicalStorageGroupId(*creation->getArgOperand(kHandleOperandIndex));
                if (handleGroupId.has_value() &&
                    joinPrecedesEveryReturn(function, *handleGroupId, joins, dominatorTree))
                {
                    continue;
                }

                const llvm::Function* entry = resolveFunctionValue(*creation->getArgOperand(
                    kind == CallKind::PThreadCreate ? kEntryOperandIndex
                                                    : kStdThreadCallableOperandIndex));
                const ResolvedSourceLocations locations = resolveSourceLocations(*creation);

                facts.push_back(ThreadArgumentEscapeFact{
                    .functionId = functionId(function),
                    .entryFunctionId =
                        entry != nullptr ? functionDisplayName(*entry) : std::string(),
                    .location = locations.userLocation,
                });
            }
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
