// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "concurrency_symbol_classifier.hpp"

#include <optional>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class BasicBlock;
    class CallBase;
    class DominatorTree;
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

    /// Where a join has certainly waited for its thread: after a `std::thread::join`, which
    /// throws otherwise, and after a `pthread_join` whose result is unused; on the branch taken
    /// when its result, compared with zero, reads as success. The result may first be kept in a
    /// local. A result used any other way gives no such place, and the join then ends nothing.
    struct JoinSuccess
    {
        const llvm::Instruction* after = nullptr;
        const llvm::BasicBlock* branch = nullptr;
        const llvm::BasicBlock* successor = nullptr;

        /// Dominance, not reachability: the failure branch may rejoin the success one, and an
        /// invoked join has returned only past its normal edge.
        [[nodiscard]] bool covers(const llvm::Instruction& point,
                                  const llvm::DominatorTree& dominators) const;
    };

    /// Where `join` has certainly waited, if anywhere. A call returning the status of a join it
    /// makes is read as that join, with `PThreadJoin` as its kind.
    [[nodiscard]] std::optional<JoinSuccess> joinSuccessOf(const llvm::CallBase& join,
                                                           CallKind kind);

    /// Whether every return of the function holding `call` hands back what `call` returns: directly,
    /// or read back from a local only it is stored into. Other reads of the result, logging it for
    /// instance, return nothing else. A value computed from it, such as a comparison, is not it.
    [[nodiscard]] bool returnsResultOf(const llvm::Instruction& call);

    /// A proof that every thread a spawn starts is joined on every path to a normal return. Two
    /// questions read it. Whether the program joined each thread: the entry says so, even for a
    /// join that fails, since the join was made. Whether each thread has certainly ended: only
    /// `ended` says so. Absence means unknown, never completion. IR remains immutable.
    struct ThreadCompletion
    {
        /// Where every thread the spawn starts has certainly ended, on every path to a normal
        /// return. Absent when the result of a join leaves its success unproven there.
        std::optional<JoinSuccess> ended;
    };

    using ThreadCompletionMap = std::unordered_map<const llvm::CallBase*, ThreadCompletion>;

    /// A parameter a function joins on every normal return, by position. A `pthread_t` travels by
    /// value; a `std::thread` is handed over by reference.
    struct JoinedParameter
    {
        unsigned index = 0;
        bool byValue = false;
        /// The join also succeeds on every normal return: the thread has ended once a call to the
        /// function returns.
        bool ended = false;
        /// The function returns the join's status as its own: a caller's test of that result tells
        /// where the thread has ended.
        bool reported = false;
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

    /// The same contract for where a join has certainly waited, which may be past one edge of a
    /// branch.
    [[nodiscard]] bool completionCoversReturns(const llvm::Instruction& start,
                                               const JoinSuccess& completion);

    /// Whether every path from `start` to `point` passes one of `completions`: the thread has
    /// ended there when they are where its joins certainly waited. Without a point, every path
    /// from `start` to a normal return must.
    [[nodiscard]] bool completedOnEveryPath(const llvm::Instruction& start,
                                            const std::vector<JoinSuccess>& completions,
                                            const llvm::Instruction* point = nullptr);
} // namespace ctrace::concurrency::internal::analysis
