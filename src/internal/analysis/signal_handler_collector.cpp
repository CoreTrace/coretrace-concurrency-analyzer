// SPDX-License-Identifier: Apache-2.0
#include "signal_handler_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>

#include <optional>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        struct UnsafeCall
        {
            SourceLocation location;
            std::string functionId;
        };

        struct Installation
        {
            SourceLocation location;
            std::string handlerFunctionId;
        };
    } // namespace

    SignalHandlerCollector::SignalHandlerCollector(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::vector<SignalHandlerFact>
    SignalHandlerCollector::collect(const llvm::Module& module,
                                    const std::vector<DirectCallSite>& directCallSites) const
    {
        std::vector<Installation> installations;
        std::unordered_map<std::string, UnsafeCall> unsafeByFunction;

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

                    if (const llvm::Function* handler = classifier_.installedSignalHandler(*call);
                        handler != nullptr && !handler->isDeclaration())
                    {
                        installations.push_back(Installation{
                            .location = resolveSourceLocations(instruction).userLocation,
                            .handlerFunctionId = functionId(*handler),
                        });
                        continue;
                    }

                    if (classifier_.isAsyncSignalUnsafe(*call) &&
                        !unsafeByFunction.contains(functionId(function)))
                    {
                        unsafeByFunction.emplace(
                            functionId(function),
                            UnsafeCall{
                                .location = resolveSourceLocations(instruction).userLocation,
                                .functionId = functionId(function),
                            });
                    }
                }
            }
        }

        if (installations.empty() || unsafeByFunction.empty())
            return {};

        // A handler that only calls a helper is no safer than the helper, so the unsafety
        // travels back along direct calls until it reaches the handler itself.
        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const DirectCallSite& site : directCallSites)
            {
                const auto calleeIt = unsafeByFunction.find(site.calleeFunctionId);
                if (calleeIt == unsafeByFunction.end() ||
                    unsafeByFunction.contains(site.callerFunctionId))
                {
                    continue;
                }

                unsafeByFunction.emplace(site.callerFunctionId, calleeIt->second);
                changed = true;
            }
        }

        std::vector<SignalHandlerFact> facts;
        std::unordered_set<std::string> reported;
        for (const Installation& installation : installations)
        {
            const auto unsafeIt = unsafeByFunction.find(installation.handlerFunctionId);
            if (unsafeIt == unsafeByFunction.end() ||
                !reported.insert(installation.handlerFunctionId).second)
            {
                continue;
            }

            facts.push_back(SignalHandlerFact{
                .location = installation.location,
                .handlerFunctionId = installation.handlerFunctionId,
                .unsafeCallLocation = unsafeIt->second.location,
                .unsafeCallFunctionId = unsafeIt->second.functionId,
            });
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
