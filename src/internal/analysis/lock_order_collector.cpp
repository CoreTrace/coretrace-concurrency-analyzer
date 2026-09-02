// SPDX-License-Identifier: Apache-2.0
#include "lock_order_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "lock_effect_application.hpp"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

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

                const auto stateIt = outStates.find(predecessor);
                if (stateIt == outStates.end() || !stateIt->second.has_value())
                    continue;

                if (!result.has_value())
                    result = *stateIt->second;
                else
                    result = intersectLockSets(*result, *stateIt->second);
            }

            if (result.has_value())
                return result;

            if (&block == &block.getParent()->getEntryBlock())
                return LockSet{};

            return std::nullopt;
        }
    } // namespace

    LockOrderCollector::LockOrderCollector(const ConcurrencySymbolClassifier& classifier,
                                           const LockWrapperSummaries* summaries,
                                           const SharedObjectBindings* sharedObjects)
        : classifier_(classifier), summaries_(summaries), sharedObjects_(sharedObjects)
    {
    }

    std::vector<LockOrderFact>
    LockOrderCollector::collect(const llvm::Function& function,
                                const std::set<std::string>& initialHeldLocks,
                                const LiveEntriesByInstruction& liveEntriesByInstruction) const
    {
        std::vector<LockOrderFact> facts;
        std::unordered_set<std::string> factKeys;

        const SharedObjectBinding* sharedObject = nullptr;
        if (sharedObjects_ != nullptr)
        {
            if (const auto it = sharedObjects_->find(functionId(function));
                it != sharedObjects_->end())
            {
                sharedObject = &it->second;
            }
        }

        const SynchronizationEffectResolver effectResolver(
            classifier_, function.getParent()->getDataLayout(), summaries_,
            /*nameParameterLocks=*/false, sharedObject);
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
        inStates[entryBlock] = initialHeldLocks;

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
                    const LockStateChange change = lockStateChangeFor(instruction, lockEffects);
                    if (change.empty())
                        continue;

                    for (const std::string& lockId : change.released)
                        currentLocks.erase(lockId);

                    if (!change.orderedAcquired.empty() && !currentLocks.empty())
                    {
                        const SourceLocation location =
                            resolveSourceLocations(instruction).userLocation;
                        ThreadEntrySet liveEntries;
                        if (const auto liveIt = liveEntriesByInstruction.find(&instruction);
                            liveIt != liveEntriesByInstruction.end())
                        {
                            liveEntries = liveIt->second;
                        }

                        for (const std::string& acquiredLock : change.orderedAcquired)
                        {
                            for (const std::string& heldLock : currentLocks)
                            {
                                const std::string key = functionId(function) + "|" + heldLock +
                                                        "|" + acquiredLock + "|" + location.file +
                                                        "|" + std::to_string(location.line) + "|" +
                                                        std::to_string(location.column);
                                if (!factKeys.insert(key).second)
                                    continue;

                                facts.push_back(LockOrderFact{
                                    .functionId = functionId(function),
                                    .firstLockId = heldLock,
                                    .secondLockId = acquiredLock,
                                    .location = location,
                                    .heldLocks = currentLocks,
                                    .liveEntries = liveEntries,
                                });
                            }
                        }
                    }

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

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
