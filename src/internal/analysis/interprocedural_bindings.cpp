// SPDX-License-Identifier: Apache-2.0
#include "interprocedural_bindings.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <string>
#include <unordered_map>

namespace ctrace::concurrency::internal::analysis
{
    std::vector<DirectCallSite>
    collectDirectCallSites(const llvm::Module& module,
                           const ConcurrencySymbolClassifier& classifier,
                           LlvmFunctionAnalysisProvider& analyses)
    {
        std::vector<DirectCallSite> sites;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const llvm::DominatorTree& dominatorTree = analyses.getDominatorTree(function);
            const llvm::LoopInfo& loopInfo = analyses.getLoopInfo(function);

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

    std::unordered_set<const llvm::CallBase*>
    callsClosingCycles(const std::vector<DirectCallSite>& sites)
    {
        std::unordered_map<std::string, std::vector<std::string>> calleesByCaller;
        for (const DirectCallSite& site : sites)
            calleesByCaller[site.callerFunctionId].push_back(site.calleeFunctionId);

        // Tarjan's strongly connected components, iterative so that a long call chain cannot
        // exhaust the stack. A call closes a cycle when its caller and callee share a component.
        struct Visit
        {
            std::string function;
            std::size_t nextCallee = 0;
        };
        std::unordered_map<std::string, unsigned> order;
        std::unordered_map<std::string, unsigned> lowest;
        std::unordered_map<std::string, unsigned> componentOf;
        std::unordered_set<std::string> onStack;
        std::vector<std::string> stack;
        unsigned nextOrder = 0;
        unsigned nextComponent = 0;

        auto enter = [&](const std::string& function, std::vector<Visit>& visits)
        {
            order[function] = lowest[function] = nextOrder++;
            stack.push_back(function);
            onStack.insert(function);
            visits.push_back(Visit{.function = function});
        };

        for (const auto& [root, rootCallees] : calleesByCaller)
        {
            if (order.contains(root))
                continue;

            std::vector<Visit> visits;
            enter(root, visits);
            while (!visits.empty())
            {
                Visit& visit = visits.back();
                const auto callees = calleesByCaller.find(visit.function);
                if (callees != calleesByCaller.end() && visit.nextCallee < callees->second.size())
                {
                    const std::string callee = callees->second[visit.nextCallee++];
                    if (!order.contains(callee))
                        enter(callee, visits);
                    else if (onStack.contains(callee))
                        lowest[visit.function] = std::min(lowest[visit.function], order[callee]);
                    continue;
                }

                const std::string function = visit.function;
                visits.pop_back();
                if (!visits.empty())
                {
                    unsigned& callerLowest = lowest[visits.back().function];
                    callerLowest = std::min(callerLowest, lowest[function]);
                }
                if (lowest[function] != order[function])
                    continue;

                std::string member;
                do
                {
                    member = stack.back();
                    stack.pop_back();
                    onStack.erase(member);
                    componentOf[member] = nextComponent;
                } while (member != function);
                ++nextComponent;
            }
        }

        std::unordered_set<const llvm::CallBase*> closing;
        for (const DirectCallSite& site : sites)
        {
            if (site.call != nullptr &&
                componentOf.at(site.callerFunctionId) == componentOf.at(site.calleeFunctionId))
            {
                closing.insert(site.call);
            }
        }
        return closing;
    }
} // namespace ctrace::concurrency::internal::analysis
