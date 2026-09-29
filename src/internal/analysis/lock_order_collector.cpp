// SPDX-License-Identifier: Apache-2.0
#include "lock_order_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"
#include "lock_effect_application.hpp"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <unordered_map>

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

        /// The global a comparison reads the address of, directly or converted to an integer. Null
        /// for anything else, which leaves the orders it chooses between as they are.
        const llvm::GlobalVariable* comparedObject(const llvm::Value& operand)
        {
            if (const auto* toInteger = llvm::dyn_cast<llvm::PtrToIntOperator>(&operand))
                return resolveBaseGlobal(*toInteger->getPointerOperand());
            return resolveBaseGlobal(operand);
        }

        /// The global holding the lock `lockId` that `site` acquires: the one its operand naming
        /// that lock points into.
        const llvm::GlobalVariable* lockedObject(const llvm::Instruction& site,
                                                 const std::string& lockId,
                                                 const SynchronizationEffectResolver& locks)
        {
            for (const llvm::Value* operand : llvm::cast<llvm::CallBase>(site).args())
            {
                if (locks.lockIdOf(*operand) == lockId)
                    return resolveBaseGlobal(*operand);
            }
            return nullptr;
        }

        /// True when each of `sites` runs only after `edge`.
        bool allFollow(const llvm::BasicBlockEdge& edge,
                       const std::vector<const llvm::Instruction*>& sites,
                       const llvm::DominatorTree& dominatorTree)
        {
            return std::all_of(sites.begin(), sites.end(), [&](const llvm::Instruction* site)
                               { return dominatorTree.dominates(edge, site->getParent()); });
        }

        /// True when every acquisition of `forward`, which takes `first` then `second`, runs only
        /// on the side of a branch where the address of `first`'s object is the lower of the two,
        /// and every one of `backward`, which takes them the other way round, only on the other.
        bool ordersLowerAddressFirst(const std::vector<const llvm::Instruction*>& forward,
                                     const std::vector<const llvm::Instruction*>& backward,
                                     const std::string& first, const std::string& second,
                                     const SynchronizationEffectResolver& locks,
                                     const llvm::DominatorTree& dominatorTree)
        {
            const llvm::GlobalVariable* firstObject = lockedObject(*backward.front(), first, locks);
            const llvm::GlobalVariable* secondObject =
                lockedObject(*forward.front(), second, locks);
            if (firstObject == nullptr || secondObject == nullptr)
                return false;

            for (const llvm::BasicBlock& block : *forward.front()->getFunction())
            {
                const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
                if (branch == nullptr || !branch->isConditional())
                    continue;

                const auto* comparison = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
                if (comparison == nullptr || !comparison->isRelational())
                    continue;

                // Read the comparison with `first`'s object on the left. It must tell the two
                // objects apart, which one object holding both locks would not.
                const llvm::GlobalVariable* lhs = comparedObject(*comparison->getOperand(0));
                const llvm::GlobalVariable* rhs = comparedObject(*comparison->getOperand(1));
                const bool straight = lhs == firstObject && rhs == secondObject;
                const bool swapped = lhs == secondObject && rhs == firstObject;
                if (straight == swapped)
                    continue;

                const llvm::CmpInst::Predicate predicate =
                    straight ? comparison->getPredicate() : comparison->getSwappedPredicate();
                const bool firstLowerWhenTrue =
                    llvm::ICmpInst::isLT(predicate) || llvm::ICmpInst::isLE(predicate);
                const llvm::BasicBlockEdge firstLower(
                    &block, branch->getSuccessor(firstLowerWhenTrue ? 0 : 1));
                const llvm::BasicBlockEdge secondLower(
                    &block, branch->getSuccessor(firstLowerWhenTrue ? 1 : 0));
                if (allFollow(firstLower, forward, dominatorTree) &&
                    allFollow(secondLower, backward, dominatorTree))
                {
                    return true;
                }
            }

            return false;
        }
    } // namespace

    LockOrderCollector::LockOrderCollector(const ConcurrencySymbolClassifier& classifier,
                                           LlvmFunctionAnalysisProvider& analyses,
                                           const LockWrapperSummaries* summaries,
                                           const SharedObjectBindings* sharedObjects)
        : classifier_(classifier), analyses_(analyses), summaries_(summaries),
          sharedObjects_(sharedObjects)
    {
        if (sharedObjects_ != nullptr)
            localObjects_ = sharedObjectNames(*sharedObjects_, SharedObjectKind::Local);
    }

    std::vector<LockOrderFact>
    LockOrderCollector::collect(const llvm::Function& function,
                                const std::set<std::string>& initialHeldLocks,
                                const LiveEntriesByInstruction& liveEntriesByInstruction) const
    {
        std::vector<LockOrderFact> facts;
        // The acquisitions each fact stands for: every one at its source location, which a macro
        // shares between all the code it expands to.
        std::vector<std::vector<const llvm::Instruction*>> factSites;
        std::unordered_map<std::string, std::size_t> factIndexByKey;

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
            /*nameParameterLocks=*/false, sharedObject, &localObjects_);
        const FunctionLockEffects lockEffects =
            collectFunctionLockEffects(function, effectResolver);

        const llvm::DominatorTree& dominatorTree = analyses_.getDominatorTree(function);

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
                                const auto [keyIt, inserted] =
                                    factIndexByKey.try_emplace(key, facts.size());
                                if (!inserted)
                                {
                                    std::vector<const llvm::Instruction*>& sites =
                                        factSites[keyIt->second];
                                    if (std::find(sites.begin(), sites.end(), &instruction) ==
                                        sites.end())
                                    {
                                        sites.push_back(&instruction);
                                    }
                                    continue;
                                }

                                facts.push_back(LockOrderFact{
                                    .functionId = functionId(function),
                                    .firstLockId = heldLock,
                                    .secondLockId = acquiredLock,
                                    .location = location,
                                    .heldLocks = currentLocks,
                                    .liveEntries = liveEntries,
                                });
                                factSites.push_back({&instruction});
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

        // Locking the lower of two addresses first, with an if/else on their comparison, takes
        // both orders; which one runs follows the addresses, not the thread. Anything else that
        // picks between two orders, a flag or other pointers, may pick differently in each thread.
        for (std::size_t lhs = 0; lhs < facts.size(); ++lhs)
        {
            for (std::size_t rhs = lhs + 1; rhs < facts.size(); ++rhs)
            {
                const bool opposite = facts[lhs].firstLockId == facts[rhs].secondLockId &&
                                      facts[lhs].secondLockId == facts[rhs].firstLockId;
                if (!opposite || !ordersLowerAddressFirst(
                                     factSites[lhs], factSites[rhs], facts[lhs].firstLockId,
                                     facts[lhs].secondLockId, effectResolver, dominatorTree))
                {
                    continue;
                }

                facts[lhs].lowerAddressFirst = true;
                facts[rhs].lowerAddressFirst = true;
            }
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
