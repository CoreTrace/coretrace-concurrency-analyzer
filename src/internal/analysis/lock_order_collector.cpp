// SPDX-License-Identifier: Apache-2.0
#include "lock_order_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
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

        struct LockOperation
        {
            bool isAcquire = false;
            std::string lockId;
        };

        std::optional<LockOperation> lockOperation(const llvm::Instruction& instruction,
                                                   const ConcurrencySymbolClassifier& classifier)
        {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (call == nullptr || call->arg_size() == 0)
                return std::nullopt;

            const CallKind kind = classifier.classify(*call);
            const bool isAcquire =
                kind == CallKind::PThreadMutexLock || kind == CallKind::StdMutexLock;
            const bool isRelease =
                kind == CallKind::PThreadMutexUnlock || kind == CallKind::StdMutexUnlock;
            if (!isAcquire && !isRelease)
                return std::nullopt;

            const std::optional<std::string> lockId = canonicalGlobalId(*call->getArgOperand(0));
            if (!lockId.has_value())
                return std::nullopt;

            return LockOperation{
                .isAcquire = isAcquire,
                .lockId = *lockId,
            };
        }

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

    LockOrderCollector::LockOrderCollector(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::vector<LockOrderFact> LockOrderCollector::collect(const llvm::Function& function) const
    {
        std::vector<LockOrderFact> facts;
        std::unordered_set<std::string> factKeys;

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
        inStates[entryBlock] = LockSet{};

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
                    const std::optional<LockOperation> operation =
                        lockOperation(instruction, classifier_);
                    if (!operation.has_value())
                        continue;

                    if (operation->isAcquire)
                    {
                        const SourceLocation location =
                            resolveSourceLocations(instruction).userLocation;
                        for (const std::string& heldLock : currentLocks)
                        {
                            const std::string key = functionId(function) + "|" + heldLock + "|" +
                                                    operation->lockId + "|" + location.file + "|" +
                                                    std::to_string(location.line) + "|" +
                                                    std::to_string(location.column);
                            if (!factKeys.insert(key).second)
                                continue;

                            facts.push_back(LockOrderFact{
                                .functionId = functionId(function),
                                .firstLockId = heldLock,
                                .secondLockId = operation->lockId,
                                .location = location,
                            });
                        }

                        currentLocks.insert(operation->lockId);
                        continue;
                    }

                    currentLocks.erase(operation->lockId);
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
