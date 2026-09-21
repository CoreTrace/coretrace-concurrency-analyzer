// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <unordered_map>

namespace llvm
{
    class CallBase;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

    /// A successful proof names the program point after every instance from a spawn has
    /// been joined. Absence means unknown, never completion. IR remains immutable.
    using ThreadCompletionMap = std::unordered_map<const llvm::CallBase*, const llvm::Instruction*>;

    [[nodiscard]] ThreadCompletionMap
    collectThreadCompletions(const llvm::Module& module,
                             const ConcurrencySymbolClassifier& classifier,
                             LlvmFunctionAnalysisProvider& analyses);

    /// Normal-return contract shared by lifecycle and contained-entry summaries. Exceptional
    /// exits are deliberately not normal returns (the existing lifecycle contract).
    [[nodiscard]] bool completionCoversReturns(const llvm::Instruction& start,
                                               const llvm::Instruction& completion);
} // namespace ctrace::concurrency::internal::analysis
