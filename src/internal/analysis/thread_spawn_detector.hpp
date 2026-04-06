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

        [[nodiscard]] ThreadSpawnCollection collect(const llvm::Module& module) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
