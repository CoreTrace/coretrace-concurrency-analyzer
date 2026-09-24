// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "thread_completion_analysis.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

    /// Finds threads handed a pointer into the frame that created them.
    ///
    /// A thread argument outlives the call that passed it unless the creator waits. When the
    /// pointer is a local variable and no join stands between the creation and every return, the
    /// frame is gone while the thread still reads it — the pointer does not dangle sometimes, it
    /// dangles as soon as the creator returns.
    class ThreadArgumentEscapeCollector
    {
      public:
        explicit ThreadArgumentEscapeCollector(const ConcurrencySymbolClassifier& classifier,
                                               LlvmFunctionAnalysisProvider& analyses);

        /// `completions` are the unit's join-range proofs: a creation they cover is joined, every
        /// instance of it, before any normal return, even when no single join dominates one.
        [[nodiscard]] std::vector<ThreadArgumentEscapeFact>
        collect(const llvm::Module& module, const ThreadCompletionMap& completions) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
    };
} // namespace ctrace::concurrency::internal::analysis
