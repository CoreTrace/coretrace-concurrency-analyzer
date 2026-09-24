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

    struct PublicationAnalysis
    {
        /// The accesses a release/acquire publication orders, keyed by the accessing instruction.
        PublicationOrderingMap ordering;
        /// Publications whose store does not release or whose observing load does not acquire.
        std::vector<WeakPublicationFact> weakPublications;
    };

    /// Finds the publications through atomic flags in a unit.
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
    /// Such a publication orders the accesses it covers, and is reported as weak when the data
    /// both sides touch is left uncovered by a relaxed store or load. In a project analysis only
    /// flags with internal linkage qualify, since another unit may store an external one.
    [[nodiscard]] PublicationAnalysis
    analyzePublications(const llvm::Module& module, const std::vector<PendingAccess>& accesses,
                        const std::vector<DirectCallSite>& directCallSites,
                        const std::vector<SpawnFact>& spawns,
                        LlvmFunctionAnalysisProvider& analyses, bool projectAnalysis);

    /// Orders a function-local static's initialization before every other access to it.
    ///
    /// The language initializes such a static once: the thread that wins its guard runs the
    /// constructor while holding it, and every other thread either finds it done or waits for
    /// the release. The object is only reachable through its function, past that guard, so
    /// whatever the initialization wrote happens before anything else touches the object. An
    /// access holding the guard is initialization and is tagged `beforeRelease`; any other
    /// access to the object is tagged `afterAcquire`. Accesses outside the initialization are
    /// still paired with each other.
    void orderStaticInitialization(const llvm::Module& module, std::vector<AccessFact>& accesses,
                                   bool projectAnalysis);
} // namespace ctrace::concurrency::internal::analysis
