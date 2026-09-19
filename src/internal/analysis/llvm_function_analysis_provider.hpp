// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>

namespace llvm
{
    class Function;
    class AAResults;
    class DominatorTree;
    class LoopInfo;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    /// Owns LLVM's per-function analyses for one translation-unit analysis. Each result is
    /// computed once per function and reused by every collector that asks for it; the module
    /// is never mutated during an analysis, so nothing is ever invalidated. One instance per
    /// module per thread: the analysis managers underneath are not synchronized.
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
        [[nodiscard]] const llvm::DominatorTree& getDominatorTree(const llvm::Function& function);
        [[nodiscard]] const llvm::LoopInfo& getLoopInfo(const llvm::Function& function);

      private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace ctrace::concurrency::internal::analysis
