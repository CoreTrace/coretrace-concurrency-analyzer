// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "thread_completion_analysis.hpp"

#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

    class ThreadLifecycleCollector
    {
      public:
        explicit ThreadLifecycleCollector(const ConcurrencySymbolClassifier& classifier,
                                          LlvmFunctionAnalysisProvider& analyses);

        [[nodiscard]] std::vector<ThreadLifecycleFact>
        collect(const llvm::Module& module, const ThreadCompletionMap& completions) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
    };
} // namespace ctrace::concurrency::internal::analysis
