// SPDX-License-Identifier: Apache-2.0
#include "thread_context_propagator.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"

#include <deque>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    ThreadContextPropagator::ThreadContextPropagator(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::unordered_map<std::string, ThreadEntrySet> ThreadContextPropagator::collect(
        const llvm::Module& module,
        const std::unordered_map<std::string, EntryConcurrencyInfo>& entryConcurrency) const
    {
        const std::vector<DirectCallSite> callSites = collectDirectCallSites(module, classifier_);

        std::unordered_map<std::string, std::vector<std::string>> calleesByFunction;
        for (const DirectCallSite& site : callSites)
            calleesByFunction[site.callerFunctionId].push_back(site.calleeFunctionId);

        std::unordered_map<std::string, ThreadEntrySet> reachableEntriesByFunction;
        for (const auto& [entryFunctionId, concurrency] : entryConcurrency)
        {
            (void)concurrency;

            std::deque<std::string> queue;
            std::unordered_set<std::string> visited;
            queue.push_back(entryFunctionId);
            visited.insert(entryFunctionId);

            while (!queue.empty())
            {
                std::string functionId = std::move(queue.front());
                queue.pop_front();
                reachableEntriesByFunction[functionId].insert(entryFunctionId);

                const auto calleesIt = calleesByFunction.find(functionId);
                if (calleesIt == calleesByFunction.end())
                    continue;

                for (const std::string& calleeId : calleesIt->second)
                {
                    if (visited.insert(calleeId).second)
                        queue.push_back(calleeId);
                }
            }
        }

        return reachableEntriesByFunction;
    }
} // namespace ctrace::concurrency::internal::analysis
