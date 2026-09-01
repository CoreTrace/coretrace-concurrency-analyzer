// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace llvm
{
    class CallBase;
    class DataLayout;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    enum class LockEffectKind
    {
        /// Blocking acquisition: participates both in protection and in lock-order cycles.
        Acquire,
        /// Non-blocking acquisition: protects the critical section but can never deadlock, so it
        /// must not contribute an edge to the lock-order graph.
        TryAcquire,
        Release,
        /// RAII guard construction: acquires every lock in `lockIds` and binds them to `guardId`.
        GuardAcquire,
        /// RAII guard destruction: releases whatever `guardId` acquired.
        GuardRelease,
    };

    struct LockEffect
    {
        LockEffectKind kind = LockEffectKind::Acquire;
        /// Empty for GuardRelease, which resolves its locks through `guardId`.
        std::vector<std::string> lockIds;
        /// Storage identity of the guard object, for GuardAcquire and GuardRelease.
        std::string guardId;
        /// True when the acquisition targets a lock type allowing recursive acquisition.
        bool recursiveLock = false;
    };

    /// Maps a call to the synchronization effects it has on its arguments. Effects are expressed
    /// over argument positions rather than over hardcoded symbol lists, so a new lock-like API is
    /// added by describing its effect, not by teaching every checker a new name.
    class SynchronizationEffectResolver
    {
      public:
        SynchronizationEffectResolver(const ConcurrencySymbolClassifier& classifier,
                                      const llvm::DataLayout& layout);

        [[nodiscard]] std::vector<LockEffect> resolve(const llvm::CallBase& call) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        const llvm::DataLayout& layout_;
    };
} // namespace ctrace::concurrency::internal::analysis
