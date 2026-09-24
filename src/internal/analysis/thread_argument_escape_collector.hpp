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

    /// Memory a thread may use after its lifetime ends, grouped by what ends it.
    struct ThreadLifetimeFacts
    {
        /// Locals of a frame that returns before the thread is joined.
        std::vector<ThreadArgumentEscapeFact> escapes;
        /// Heap memory the creator frees before the thread is joined.
        std::vector<ThreadArgumentFreeFact> frees;
        /// Thread-locals reached through a pointer after their thread has been joined.
        std::vector<ExpiredThreadLocalFact> expiredThreadLocals;
    };

    /// Finds threads handed memory whose lifetime ends while they may still use it.
    ///
    /// A thread argument outlives the call that passed it unless the creator waits. When the
    /// pointer is a local variable and no join stands between the creation and every return, the
    /// frame is gone while the thread still reads it — the pointer does not dangle sometimes, it
    /// dangles as soon as the creator returns. When it is memory the creator frees, the same holds
    /// from the free on, unless a join of the thread precedes it; a thread that never touches its
    /// argument is left out, since it cannot use it after the free. The converse also holds: a
    /// thread's thread-locals end with the thread, so a pointer it published to one of them dangles
    /// once every thread that could have published it has been joined.
    class ThreadArgumentEscapeCollector
    {
      public:
        explicit ThreadArgumentEscapeCollector(const ConcurrencySymbolClassifier& classifier,
                                               LlvmFunctionAnalysisProvider& analyses);

        /// `completions` are the unit's join-range proofs: a creation they cover is joined, every
        /// instance of it, before any normal return, even when no single join dominates one.
        [[nodiscard]] ThreadLifetimeFacts collect(const llvm::Module& module,
                                                  const ThreadCompletionMap& completions) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
    };
} // namespace ctrace::concurrency::internal::analysis
