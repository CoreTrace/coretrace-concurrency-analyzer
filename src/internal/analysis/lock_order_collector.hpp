// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "lock_wrapper_summaries.hpp"
#include "shared_object_binding_collector.hpp"

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

    class LockOrderCollector
    {
      public:
        using LiveEntriesByInstruction =
            std::unordered_map<const llvm::Instruction*, ThreadEntrySet>;

        /// `summaries` carries the lock effects user-written wrappers have on the arguments
        /// they are given, so a lock taken inside a helper still protects the code after the
        /// call. Null leaves those calls opaque, as before.
        explicit LockOrderCollector(const ConcurrencySymbolClassifier& classifier,
                                    const LockWrapperSummaries* summaries = nullptr,
                                    const SharedObjectBindings* sharedObjects = nullptr);

        [[nodiscard]] std::vector<LockOrderFact>
        collect(const llvm::Function& function, const std::set<std::string>& initialHeldLocks = {},
                const LiveEntriesByInstruction& liveEntriesByInstruction = {}) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        const LockWrapperSummaries* summaries_ = nullptr;
        const SharedObjectBindings* sharedObjects_ = nullptr;
    };
} // namespace ctrace::concurrency::internal::analysis
