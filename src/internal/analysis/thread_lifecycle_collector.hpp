// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    class ThreadLifecycleCollector
    {
      public:
        explicit ThreadLifecycleCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::vector<ThreadLifecycleFact> collect(const llvm::Module& module) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
