// SPDX-License-Identifier: Apache-2.0
#include "thread_argument_escape_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        constexpr unsigned kHandleOperandIndex = 0;
        constexpr unsigned kEntryOperandIndex = 2;
        constexpr unsigned kArgumentOperandIndex = 3;

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
    } // namespace

    ThreadArgumentEscapeCollector::ThreadArgumentEscapeCollector(
        const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::vector<ThreadArgumentEscapeFact>
    ThreadArgumentEscapeCollector::collect(const llvm::Module& module) const
    {
        std::vector<ThreadArgumentEscapeFact> facts;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            std::vector<JoinSite> joins;
            std::vector<const llvm::CallBase*> creations;
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_size() <= kHandleOperandIndex)
                        continue;

                    const CallKind kind = classifier_.classify(*call);
                    if (kind == CallKind::PThreadJoin)
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

                    if (kind == CallKind::PThreadCreate && call->arg_size() > kArgumentOperandIndex)
                    {
                        creations.push_back(call);
                    }
                }
            }

            if (creations.empty())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            const llvm::DominatorTree dominatorTree(mutableFunction);

            for (const llvm::CallBase* creation : creations)
            {
                // getUnderlyingObject follows the indexing, so `&local` and `&local.field` and
                // `&array[0]` all lead back to the same allocation.
                const llvm::Value* argument =
                    llvm::getUnderlyingObject(creation->getArgOperand(kArgumentOperandIndex));
                const auto* local = llvm::dyn_cast_or_null<llvm::AllocaInst>(argument);
                if (local == nullptr || local->getFunction() != &function)
                    continue;

                const std::optional<std::string> handleGroupId =
                    canonicalStorageGroupId(*creation->getArgOperand(kHandleOperandIndex));
                if (handleGroupId.has_value() &&
                    joinPrecedesEveryReturn(function, *handleGroupId, joins, dominatorTree))
                {
                    continue;
                }

                const llvm::Function* entry =
                    resolveFunctionValue(*creation->getArgOperand(kEntryOperandIndex));
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
