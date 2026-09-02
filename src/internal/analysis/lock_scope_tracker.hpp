// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lock_wrapper_summaries.hpp"
#include "shared_object_binding_collector.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    class LockScopeTracker
    {
      public:
        /// `summaries` carries the lock effects user-written wrappers have on the arguments
        /// they are given, so a lock taken inside a helper still protects the code after the
        /// call. Null leaves those calls opaque, as before.
        explicit LockScopeTracker(const ConcurrencySymbolClassifier& classifier,
                                  const LockWrapperSummaries* summaries = nullptr,
                                  const SharedObjectBindings* sharedObjects = nullptr);

        [[nodiscard]] std::unordered_map<const llvm::Instruction*, std::set<std::string>>
        collectHeldLocks(const llvm::Function& function,
                         const std::unordered_set<const llvm::Instruction*>& trackedAccesses) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        const LockWrapperSummaries* summaries_ = nullptr;
        const SharedObjectBindings* sharedObjects_ = nullptr;
    };
} // namespace ctrace::concurrency::internal::analysis
