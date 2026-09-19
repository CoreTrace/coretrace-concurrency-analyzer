// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "interprocedural_bindings.hpp"

#include <unordered_map>
#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

    struct ThreadSpawnCollection
    {
        std::vector<SpawnFact> spawns;
        std::unordered_map<std::string, EntryConcurrencyInfo> entryConcurrency;
    };

    class ThreadSpawnDetector
    {
      public:
        explicit ThreadSpawnDetector(const ConcurrencySymbolClassifier& classifier,
                                     LlvmFunctionAnalysisProvider& analyses);

        /// `directCallSites` are the unit's resolved calls, collected once by the caller and
        /// shared with every collector that follows them.
        /// `includeExternalEntries` keeps spawns whose entry is only declared here. A
        /// single-unit run drops them: nothing can be said about a body it cannot see. A
        /// project-wide run needs them, because that body is another unit's.
        [[nodiscard]] ThreadSpawnCollection
        collect(const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
                bool includeExternalEntries = false) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
    };
} // namespace ctrace::concurrency::internal::analysis
