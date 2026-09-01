// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <unordered_map>
#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    struct ThreadSpawnCollection
    {
        std::vector<SpawnFact> spawns;
        std::unordered_map<std::string, EntryConcurrencyInfo> entryConcurrency;
    };

    class ThreadSpawnDetector
    {
      public:
        explicit ThreadSpawnDetector(const ConcurrencySymbolClassifier& classifier);

        /// `includeExternalEntries` keeps spawns whose entry is only declared here. A
        /// single-unit run drops them: nothing can be said about a body it cannot see. A
        /// project-wide run needs them, because that body is another unit's.
        [[nodiscard]] ThreadSpawnCollection collect(const llvm::Module& module,
                                                    bool includeExternalEntries = false) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
