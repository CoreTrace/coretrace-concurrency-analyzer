// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "lock_wrapper_summaries.hpp"
#include "shared_object_binding_collector.hpp"

#include <string_view>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;

    /// Stands, inside a function, for every lock its caller holds at the call. An order from it
    /// is an acquisition each call site orders after the locks it holds there.
    inline constexpr std::string_view kLocksHeldByCaller = "%caller";

    class LockOrderCollector
    {
      public:
        using LiveEntriesByInstruction =
            std::unordered_map<const llvm::Instruction*, ThreadEntrySet>;

        /// `summaries` carries the lock effects user-written wrappers have on the arguments
        /// they are given, so a lock taken inside a helper still protects the code after the
        /// call. Null leaves those calls opaque, as before.
        explicit LockOrderCollector(const ConcurrencySymbolClassifier& classifier,
                                    LlvmFunctionAnalysisProvider& analyses,
                                    const LockWrapperSummaries* summaries = nullptr,
                                    const SharedObjectBindings* sharedObjects = nullptr);

        /// The orders `function` takes. An order naming the lock of one of its parameters, or
        /// `kLocksHeldByCaller`, is open: only a call to `function` can say which locks it
        /// orders.
        [[nodiscard]] std::vector<LockOrderFact>
        collect(const llvm::Function& function, const std::set<std::string>& initialHeldLocks = {},
                const LiveEntriesByInstruction& liveEntriesByInstruction = {}) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        LlvmFunctionAnalysisProvider& analyses_;
        const LockWrapperSummaries* summaries_ = nullptr;
        const SharedObjectBindings* sharedObjects_ = nullptr;
        std::unordered_set<std::string> localObjects_;
    };
} // namespace ctrace::concurrency::internal::analysis
