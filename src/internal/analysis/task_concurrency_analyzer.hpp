// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace llvm
{
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    struct DirectCallSite;

    struct TaskConcurrencyResult
    {
        /// Functions executed by the initial thread, reached from `main` through direct calls.
        std::unordered_set<std::string> rootTaskFunctions;
        /// Spawned entries already running when an instruction of a root-task function executes.
        /// An instruction placed before any spawn, or after the matching join, maps to an empty set
        /// and therefore races with nothing.
        std::unordered_map<const llvm::Instruction*, ThreadEntrySet> liveEntriesAtInstruction;
        /// Entries whose lifetimes are provably disjoint because one is joined before the other is
        /// spawned.
        std::unordered_set<EntryPair, EntryPairHash> sequencedEntryPairs;
        /// Entries with at least two instances alive at once, established by observing a spawn
        /// while a previous instance of the same entry is still live.
        std::unordered_set<std::string> overlappingSpawnEntries;
    };

    /// Builds the may-happen-in-parallel relation of the translation unit. Unlike a plain
    /// reachability query, it models the initial thread as a task and uses spawn/join dominance to
    /// bound each spawned task's lifetime.
    class TaskConcurrencyAnalyzer
    {
      public:
        explicit TaskConcurrencyAnalyzer(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] TaskConcurrencyResult
        analyze(const llvm::Module& module,
                const std::vector<DirectCallSite>& directCallSites) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
