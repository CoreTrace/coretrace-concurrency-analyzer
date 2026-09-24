// SPDX-License-Identifier: Apache-2.0
#include "thread_context_propagator.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Module.h>

#include <deque>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    ThreadContextPropagator::ThreadContextPropagator(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::unordered_map<std::string, ThreadEntrySet> ThreadContextPropagator::collect(
        const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
        const std::unordered_map<std::string, EntryConcurrencyInfo>& entryConcurrency) const
    {
        std::unordered_map<std::string, std::vector<std::string>> calleesByFunction;
        for (const DirectCallSite& site : directCallSites)
            calleesByFunction[site.callerFunctionId].push_back(site.calleeFunctionId);

        // A thread-local object's destructor runs when the thread that registered it ends, and
        // in that thread: for the question of which threads run a function, registering it is
        // calling it. It is not a call site, since its parameters are not the call's arguments.
        for (const llvm::Function& function : module)
        {
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_size() == 0 ||
                        classifier_.classify(*call) != CallKind::ThreadExitDestructorRegistration)
                    {
                        continue;
                    }

                    if (const llvm::Function* destructor =
                            resolveFunctionValue(*call->getArgOperand(0));
                        destructor != nullptr && !destructor->isDeclaration())
                    {
                        calleesByFunction[functionId(function)].push_back(functionId(*destructor));
                    }
                }
            }
        }

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
