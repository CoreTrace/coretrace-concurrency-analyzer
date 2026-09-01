// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "synchronization_effects.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    /// Lock effects of a function, resolved once so that both the held-lock tracker and the
    /// lock-order collector interpret every acquisition the same way.
    struct FunctionLockEffects
    {
        /// Locks bound to an RAII guard object by its constructor, keyed by the guard's storage.
        /// The binding is function-wide rather than flow-sensitive: a guard slot holds the same
        /// mutex throughout its scope, and the destructor only needs to find what it acquired.
        std::unordered_map<std::string, std::vector<std::string>> guardBindings;
        std::unordered_map<const llvm::Instruction*, std::vector<LockEffect>> effectsByInstruction;
    };

    /// Net effect of one instruction on the held-lock set.
    struct LockStateChange
    {
        /// Every lock the instruction acquires, blocking or not: all of them protect the code that
        /// follows.
        std::vector<std::string> acquired;
        /// Acquisitions that may participate in a deadlock cycle. A failed `try_lock` returns
        /// instead of blocking, so it can never close a wait-for cycle.
        std::vector<std::string> orderedAcquired;
        std::vector<std::string> released;
        /// Locks acquired here whose type permits recursive acquisition.
        std::vector<std::string> recursivelyAcquirable;

        [[nodiscard]] bool empty() const noexcept
        {
            return acquired.empty() && released.empty();
        }
    };

    [[nodiscard]] FunctionLockEffects
    collectFunctionLockEffects(const llvm::Function& function,
                               const SynchronizationEffectResolver& resolver);

    [[nodiscard]] LockStateChange lockStateChangeFor(const llvm::Instruction& instruction,
                                                     const FunctionLockEffects& effects);
} // namespace ctrace::concurrency::internal::analysis
