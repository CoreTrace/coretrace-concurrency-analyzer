// SPDX-License-Identifier: Apache-2.0
#include "lock_state_propagator.hpp"

#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "lock_scope_tracker.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::set<std::string> mergeHeldLocks(const std::set<std::string>& lhs,
                                             const std::set<std::string>& rhs)
        {
            std::set<std::string> merged = lhs;
            merged.insert(rhs.begin(), rhs.end());
            return merged;
        }

        std::set<std::string> intersectHeldLocks(const std::set<std::string>& lhs,
                                                 const std::set<std::string>& rhs)
        {
            std::set<std::string> intersection;
            std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                  std::inserter(intersection, intersection.end()));
            return intersection;
        }
    } // namespace

    LockStatePropagator::LockStatePropagator(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    LockPropagationResult
    LockStatePropagator::collect(const llvm::Module& module,
                                 const std::vector<DirectCallSite>& callSites) const
    {
        std::unordered_map<const llvm::Function*, std::unordered_set<const llvm::Instruction*>>
            trackedCallsByFunction;
        std::unordered_map<std::string, std::vector<const DirectCallSite*>> incomingCallsByCallee;
        LockPropagationResult result;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            result.entryLocksByFunction.emplace(functionId(function), std::set<std::string>{});
        }

        for (const DirectCallSite& callSite : callSites)
        {
            if (callSite.call == nullptr)
                continue;

            const llvm::Function* caller = callSite.call->getFunction();
            if (caller == nullptr || caller->isDeclaration())
                continue;

            trackedCallsByFunction[caller].insert(callSite.call);
            incomingCallsByCallee[callSite.calleeFunctionId].push_back(&callSite);
        }

        LockScopeTracker lockScopeTracker(classifier_);
        std::unordered_map<const llvm::CallBase*, std::set<std::string>> localHeldLocksByCall;
        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const auto trackedCallsIt = trackedCallsByFunction.find(&function);
            if (trackedCallsIt == trackedCallsByFunction.end() || trackedCallsIt->second.empty())
                continue;

            const auto heldLocksByInstruction =
                lockScopeTracker.collectHeldLocks(function, trackedCallsIt->second);
            for (const auto& [instruction, heldLocks] : heldLocksByInstruction)
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(instruction);
                if (call == nullptr)
                    continue;

                localHeldLocksByCall.emplace(call, heldLocks);
            }
        }

        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const auto& [calleeFunctionId, incomingCalls] : incomingCallsByCallee)
            {
                bool hasIncomingState = false;
                std::set<std::string> mergedEntryLocks;

                for (const DirectCallSite* callSite : incomingCalls)
                {
                    std::set<std::string> effectiveCallLocks;
                    if (const auto localHeldLocksIt = localHeldLocksByCall.find(callSite->call);
                        localHeldLocksIt != localHeldLocksByCall.end())
                    {
                        effectiveCallLocks = localHeldLocksIt->second;
                    }

                    const auto callerEntryLocksIt =
                        result.entryLocksByFunction.find(callSite->callerFunctionId);
                    if (callerEntryLocksIt != result.entryLocksByFunction.end())
                    {
                        effectiveCallLocks =
                            mergeHeldLocks(effectiveCallLocks, callerEntryLocksIt->second);
                    }

                    if (!hasIncomingState)
                    {
                        mergedEntryLocks = std::move(effectiveCallLocks);
                        hasIncomingState = true;
                    }
                    else
                    {
                        mergedEntryLocks = intersectHeldLocks(mergedEntryLocks, effectiveCallLocks);
                    }
                }

                if (!hasIncomingState)
                    continue;

                std::set<std::string>& entryLocks = result.entryLocksByFunction[calleeFunctionId];
                if (entryLocks != mergedEntryLocks)
                {
                    entryLocks = std::move(mergedEntryLocks);
                    changed = true;
                }
            }
        }

        for (const DirectCallSite& callSite : callSites)
        {
            if (callSite.call == nullptr)
                continue;

            std::set<std::string> effectiveLocks;
            if (const auto localHeldLocksIt = localHeldLocksByCall.find(callSite.call);
                localHeldLocksIt != localHeldLocksByCall.end())
            {
                effectiveLocks = localHeldLocksIt->second;
            }

            const auto callerEntryLocksIt =
                result.entryLocksByFunction.find(callSite.callerFunctionId);
            if (callerEntryLocksIt != result.entryLocksByFunction.end())
                effectiveLocks = mergeHeldLocks(effectiveLocks, callerEntryLocksIt->second);

            result.effectiveHeldLocksByCall.emplace(callSite.call, std::move(effectiveLocks));
        }

        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
