// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

namespace llvm
{
    class Function;
    class AAResults;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class LlvmFunctionAnalysisProvider
    {
      public:
        LlvmFunctionAnalysisProvider();
        ~LlvmFunctionAnalysisProvider();

        LlvmFunctionAnalysisProvider(const LlvmFunctionAnalysisProvider&) = delete;
        LlvmFunctionAnalysisProvider& operator=(const LlvmFunctionAnalysisProvider&) = delete;
        LlvmFunctionAnalysisProvider(LlvmFunctionAnalysisProvider&&) = delete;
        LlvmFunctionAnalysisProvider& operator=(LlvmFunctionAnalysisProvider&&) = delete;

        [[nodiscard]] llvm::AAResults& getAAResults(const llvm::Function& function);

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace ctrace::concurrency::internal::analysis
