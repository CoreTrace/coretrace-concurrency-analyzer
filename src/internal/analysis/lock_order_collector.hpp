// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <vector>

namespace llvm
{
    class Function;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    class LockOrderCollector
    {
      public:
        explicit LockOrderCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::vector<LockOrderFact>
        collect(const llvm::Function& function,
                const std::set<std::string>& initialHeldLocks = {}) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
