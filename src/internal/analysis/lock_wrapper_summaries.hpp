// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class DataLayout;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    /// A lock effect a function has on one of its parameters, surviving the call.
    struct ParameterLockEffect
    {
        unsigned argumentIndex = 0;
        /// True when the parameter is still held on return, false when the call releases a lock
        /// the caller was holding.
        bool acquires = true;
        bool recursiveLock = false;

        [[nodiscard]] bool operator==(const ParameterLockEffect&) const = default;
    };

    /// What each function does to the locks its callers pass it.
    ///
    /// A wrapper such as `void take(pthread_mutex_t* m) { pthread_mutex_lock(m); }` leaves its
    /// caller holding a lock. Without that, the lock vanishes at the return and an inversion
    /// expressed through wrappers is invisible, which is the ordinary shape of real code.
    using LockWrapperSummaries = std::unordered_map<std::string, std::vector<ParameterLockEffect>>;

    /// Summarises every function of the module, resolving wrappers that call other wrappers by
    /// repeating until nothing new is learned.
    [[nodiscard]] LockWrapperSummaries
    collectLockWrapperSummaries(const llvm::Module& module,
                                const ConcurrencySymbolClassifier& classifier,
                                const llvm::DataLayout& layout);
} // namespace ctrace::concurrency::internal::analysis
