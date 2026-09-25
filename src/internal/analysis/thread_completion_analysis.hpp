// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <unordered_map>
#include <vector>

namespace llvm
{
    class CallBase;
    class Function;
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

    /// A parameter a function joins on every normal return, by position. A `pthread_t` travels by
    /// value; a `std::thread` is handed over by reference.
    struct JoinedParameter
    {
        unsigned index = 0;
        bool byValue = false;
    };

    /// The parameters each function joins on every normal return. A call passing a thread handle
    /// there waits for that thread exactly as a join written in place does.
    using JoiningHelpers = std::unordered_map<const llvm::Function*, std::vector<JoinedParameter>>;

    /// Summarises the functions this module defines, starting from `known`: what other units
    /// established about functions this module only declares.
    [[nodiscard]] JoiningHelpers
    collectJoiningHelpers(const llvm::Module& module, const ConcurrencySymbolClassifier& classifier,
                          JoiningHelpers known = {});

    [[nodiscard]] ThreadCompletionMap
    collectThreadCompletions(const llvm::Module& module,
                             const ConcurrencySymbolClassifier& classifier,
                             LlvmFunctionAnalysisProvider& analyses, const JoiningHelpers& helpers);

    /// Normal-return contract shared by lifecycle and contained-entry summaries. Exceptional
    /// exits are deliberately not normal returns (the existing lifecycle contract).
    [[nodiscard]] bool completionCoversReturns(const llvm::Instruction& start,
                                               const llvm::Instruction& completion);
} // namespace ctrace::concurrency::internal::analysis
