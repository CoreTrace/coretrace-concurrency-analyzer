// SPDX-License-Identifier: Apache-2.0
#include "interprocedural_bindings.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    std::vector<DirectCallSite>
    collectDirectCallSites(const llvm::Module& module,
                           const ConcurrencySymbolClassifier& classifier)
    {
        std::vector<DirectCallSite> sites;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            llvm::DominatorTree dominatorTree(mutableFunction);
            llvm::LoopInfo loopInfo(dominatorTree);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const llvm::Function* callee = classifier.directCallee(*call);
                    if (callee == nullptr || callee->isDeclaration())
                        continue;

                    const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
                    sites.push_back(DirectCallSite{
                        .call = call,
                        .callerFunctionId = functionId(function),
                        .calleeFunctionId = functionId(*callee),
                        .loweredLocation = locations.loweredLocation,
                        .userLocation = locations.userLocation,
                        .insideLoop = loopInfo.getLoopFor(call->getParent()) != nullptr,
                    });
                }
            }
        }

        return sites;
    }
} // namespace ctrace::concurrency::internal::analysis
