// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class LlvmFunctionAnalysisProvider;
    struct DirectCallSite;

    /// The atomic flags through which an access is ordered by a release/acquire publication.
    struct PublicationOrdering
    {
        /// Flags whose publishing store this access happens before.
        std::set<std::string> beforeRelease;
        /// Flags whose observed publication this access happens after.
        std::set<std::string> afterAcquire;
    };

    using PublicationOrderingMap =
        std::unordered_map<const llvm::Instruction*, PublicationOrdering>;

    /// Finds the accesses a flag publication orders, keyed by the accessing instruction.
    ///
    /// A thread publishes by writing data and then storing a constant to an atomic flag; another
    /// thread that loads the flag, sees that constant and only then touches the data is ordered
    /// after the writes, provided the store releases and the load acquires (directly or through a
    /// fence). The ordering only holds when observing the constant proves the publishing store
    /// was the one read, so a flag counts only when:
    ///
    /// - that store is the only one to the flag in the unit, and its constant differs from the
    ///   flag's initial value;
    /// - it runs at most once: outside any loop, in a thread entry that `main` starts exactly once
    ///   and that nothing else calls;
    /// - the reader's access lies on the branch taken when the loaded value is the constant.
    ///
    /// Anything short of that orders nothing. In a project analysis only flags with internal
    /// linkage qualify, since another unit may store an external one.
    [[nodiscard]] PublicationOrderingMap
    collectPublicationOrdering(const llvm::Module& module,
                               const std::vector<const llvm::Instruction*>& accesses,
                               const std::vector<DirectCallSite>& directCallSites,
                               const std::vector<SpawnFact>& spawns,
                               LlvmFunctionAnalysisProvider& analyses, bool projectAnalysis);
} // namespace ctrace::concurrency::internal::analysis
