// SPDX-License-Identifier: Apache-2.0
#include "process_lifecycle_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    ProcessLifecycleCollector::ProcessLifecycleCollector(
        const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    ProcessLifecycleCollection ProcessLifecycleCollector::collect(const llvm::Module& module) const
    {
        ProcessLifecycleCollection collection;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    switch (classifier_.classify(*call))
                    {
                    case CallKind::ProcessFork:
                    {
                        const ResolvedSourceLocations locations =
                            resolveSourceLocations(instruction);
                        collection.forks.push_back(ProcessForkFact{
                            .functionId = functionId(function),
                            .location = locations.userLocation,
                        });
                        break;
                    }
                    case CallKind::ProcessExec:
                        collection.functionsReachingExec.insert(functionId(function));
                        break;
                    case CallKind::ProcessWait:
                        collection.reapsChildren = true;
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        return collection;
    }
} // namespace ctrace::concurrency::internal::analysis
