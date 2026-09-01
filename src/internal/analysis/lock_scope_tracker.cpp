// SPDX-License-Identifier: Apache-2.0
#include "lock_scope_tracker.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "lock_effect_application.hpp"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        using LockSet = std::set<std::string>;
        using StateMap = std::unordered_map<const llvm::BasicBlock*, std::optional<LockSet>>;

        LockSet intersectLockSets(const LockSet& lhs, const LockSet& rhs)
        {
            LockSet result;
            std::set_intersection(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
                                  std::inserter(result, result.end()));
            return result;
        }

        std::optional<LockSet> meetPredecessorStates(const llvm::BasicBlock& block,
                                                     const StateMap& outStates,
                                                     const llvm::DominatorTree& dominatorTree)
        {
            std::optional<LockSet> result;
            for (const llvm::BasicBlock* predecessor : llvm::predecessors(&block))
            {
                if (!dominatorTree.isReachableFromEntry(predecessor))
                    continue;

                const auto it = outStates.find(predecessor);
                if (it == outStates.end() || !it->second.has_value())
                    continue;

                if (!result.has_value())
                    result = *it->second;
                else
                    result = intersectLockSets(*result, *it->second);
            }

            if (result.has_value())
                return result;

            if (&block == &block.getParent()->getEntryBlock())
                return LockSet{};

            return std::nullopt;
        }
    } // namespace

    LockScopeTracker::LockScopeTracker(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::unordered_map<const llvm::Instruction*, std::set<std::string>>
    LockScopeTracker::collectHeldLocks(
        const llvm::Function& function,
        const std::unordered_set<const llvm::Instruction*>& trackedAccesses) const
    {
        std::unordered_map<const llvm::Instruction*, std::set<std::string>> heldLocksByAccess;
        if (trackedAccesses.empty())
            return heldLocksByAccess;

        const SynchronizationEffectResolver effectResolver(classifier_,
                                                           function.getParent()->getDataLayout());
        const FunctionLockEffects lockEffects =
            collectFunctionLockEffects(function, effectResolver);

        llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
        llvm::DominatorTree dominatorTree(mutableFunction);

        std::vector<const llvm::BasicBlock*> reachableBlocks;
        for (const llvm::BasicBlock& block : function)
        {
            if (dominatorTree.isReachableFromEntry(&block))
                reachableBlocks.push_back(&block);
        }

        StateMap inStates;
        StateMap outStates;
        for (const llvm::BasicBlock* block : reachableBlocks)
        {
            inStates.emplace(block, std::nullopt);
            outStates.emplace(block, std::nullopt);
        }

        const llvm::BasicBlock* entryBlock = &function.getEntryBlock();
        inStates[entryBlock] = std::set<std::string>{};

        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const llvm::BasicBlock* block : reachableBlocks)
            {
                std::optional<LockSet> newInState = inStates[block];
                if (block != entryBlock)
                    newInState = meetPredecessorStates(*block, outStates, dominatorTree);

                if (!newInState.has_value())
                    continue;

                LockSet currentLocks = *newInState;
                for (const llvm::Instruction& instruction : *block)
                {
                    if (trackedAccesses.contains(&instruction))
                        heldLocksByAccess[&instruction] = currentLocks;

                    const LockStateChange change = lockStateChangeFor(instruction, lockEffects);
                    for (const std::string& lockId : change.released)
                        currentLocks.erase(lockId);
                    for (const std::string& lockId : change.acquired)
                        currentLocks.insert(lockId);
                }

                if (inStates[block] != newInState)
                {
                    inStates[block] = std::move(newInState);
                    changed = true;
                }

                if (outStates[block] != currentLocks)
                {
                    outStates[block] = std::move(currentLocks);
                    changed = true;
                }
            }
        }

        return heldLocksByAccess;
    }
} // namespace ctrace::concurrency::internal::analysis
