// SPDX-License-Identifier: Apache-2.0
#include "condition_wait_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    ConditionWaitCollector::ConditionWaitCollector(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::vector<ConditionWaitFact> ConditionWaitCollector::collect(const llvm::Module& module) const
    {
        std::vector<ConditionWaitFact> facts;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            const llvm::DominatorTree dominatorTree(mutableFunction);
            const llvm::LoopInfo loopInfo(dominatorTree);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr ||
                        classifier_.classify(*call) != CallKind::CondWaitWithoutPredicate)
                    {
                        continue;
                    }

                    const ResolvedSourceLocations locations = resolveSourceLocations(instruction);

                    ConditionWaitFact fact;
                    fact.functionId = functionId(function);
                    fact.location = locations.userLocation;
                    fact.loweredLocation = locations.loweredLocation;
                    fact.guardedByLoop = loopInfo.getLoopFor(&block) != nullptr;
                    facts.push_back(std::move(fact));
                }
            }
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
