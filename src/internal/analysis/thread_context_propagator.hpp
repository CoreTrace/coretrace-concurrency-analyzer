// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <unordered_map>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    class ThreadContextPropagator
    {
      public:
        explicit ThreadContextPropagator(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::unordered_map<std::string, ThreadEntrySet> collect(
            const llvm::Module& module,
            const std::unordered_map<std::string, EntryConcurrencyInfo>& entryConcurrency) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
