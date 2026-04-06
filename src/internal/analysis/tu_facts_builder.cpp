// SPDX-License-Identifier: Apache-2.0
#include "tu_facts_builder.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "lock_scope_tracker.hpp"
#include "shared_access_collector.hpp"
#include "thread_spawn_detector.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <unordered_map>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    TUFacts TUFactsBuilder::build(const llvm::Module& module) const
    {
        const ConcurrencySymbolClassifier classifier;

        ThreadSpawnDetector spawnDetector(classifier);
        ThreadSpawnCollection spawnFacts = spawnDetector.collect(module);

        SharedAccessCollector accessCollector;
        std::vector<PendingAccess> pendingAccesses = accessCollector.collect(module);

        std::unordered_map<std::string, std::unordered_set<const llvm::Instruction*>>
            trackedAccessesByFunction;
        std::unordered_map<std::string, const llvm::Function*> functionsById;
        for (const PendingAccess& pendingAccess : pendingAccesses)
        {
            trackedAccessesByFunction[pendingAccess.fact.functionId].insert(
                pendingAccess.instruction);
            functionsById[pendingAccess.fact.functionId] = pendingAccess.function;
        }

        LockScopeTracker lockScopeTracker(classifier);
        std::unordered_map<const llvm::Instruction*, std::set<std::string>> heldLocksByAccess;
        for (const auto& [functionKey, trackedAccesses] : trackedAccessesByFunction)
        {
            const llvm::Function* function = functionsById[functionKey];
            if (function == nullptr)
                continue;

            std::unordered_map<const llvm::Instruction*, std::set<std::string>> functionLocks =
                lockScopeTracker.collectHeldLocks(*function, trackedAccesses);
            heldLocksByAccess.insert(functionLocks.begin(), functionLocks.end());
        }

        TUFacts facts;
        facts.spawns = std::move(spawnFacts.spawns);
        facts.entryConcurrency = std::move(spawnFacts.entryConcurrency);

        for (PendingAccess& pendingAccess : pendingAccesses)
        {
            const auto heldLocksIt = heldLocksByAccess.find(pendingAccess.instruction);
            if (heldLocksIt != heldLocksByAccess.end())
                pendingAccess.fact.heldLocks = heldLocksIt->second;
            facts.accesses.push_back(std::move(pendingAccess.fact));
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
