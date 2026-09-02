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
        /// Entries a function starts and does not wait for, so its caller keeps running beside
        /// them once the call returns. An object that owns its thread is the usual source: the
        /// constructor spawns, and nothing joins until the destructor.
        std::unordered_map<std::string, ThreadEntrySet> entriesLeftRunningByFunction;
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

        /// `includeExternalEntries` mirrors ThreadSpawnDetector: a project-wide run must decide
        /// whether two instances of an entry overlap even when its body lives in another unit,
        /// because that verdict is only observable here, at the spawn sites.
        [[nodiscard]] TaskConcurrencyResult
        analyze(const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
                bool includeExternalEntries = false) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
