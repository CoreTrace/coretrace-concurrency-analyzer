// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    /// Finds threads handed a pointer into the frame that created them.
    ///
    /// A thread argument outlives the call that passed it unless the creator waits. When the
    /// pointer is a local variable and no join stands between the creation and every return, the
    /// frame is gone while the thread still reads it — the pointer does not dangle sometimes, it
    /// dangles as soon as the creator returns.
    class ThreadArgumentEscapeCollector
    {
      public:
        explicit ThreadArgumentEscapeCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::vector<ThreadArgumentEscapeFact>
        collect(const llvm::Module& module) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
