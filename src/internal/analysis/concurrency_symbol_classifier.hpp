// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string_view>

namespace llvm
{
    class CallBase;
    class Function;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    enum class CallKind
    {
        Unknown,
        PThreadCreate,
        PThreadJoin,
        PThreadDetach,
        PThreadMutexLock,
        PThreadMutexUnlock,
        PThreadMutexTryLock,
        PThreadRwLockAcquire,
        PThreadRwLockTryAcquire,
        PThreadRwLockUnlock,
        PThreadSpinLock,
        PThreadSpinUnlock,
        PThreadSpinTryLock,
        PThreadMutexInit,
        PThreadMutexAttrSetType,
        StdThreadCtor,
        StdThreadMove,
        StdThreadJoin,
        StdThreadDetach,
        StdThreadDtor,
        StdJThreadCtor,
        StdMutexLock,
        StdMutexUnlock,
        StdMutexTryLock,
        /// RAII guard constructor (`lock_guard`, `unique_lock`, `scoped_lock`, `shared_lock`).
        StdLockGuardCtor,
        /// RAII guard constructor with a `defer_lock` tag, which does not acquire.
        StdLockGuardDeferredCtor,
        StdLockGuardDtor,
        /// A condition-variable wait that checks nothing on wake-up. The standard permits a
        /// spurious wake-up, so the caller alone decides whether the condition really holds.
        CondWaitWithoutPredicate,
        /// The predicate overload, which rechecks the condition itself and cannot wake early.
        CondWaitWithPredicate,
    };

    class ConcurrencySymbolClassifier
    {
      public:
        [[nodiscard]] const llvm::Function* directCallee(const llvm::CallBase& call) const;
        [[nodiscard]] CallKind classify(const llvm::CallBase& call) const;
        /// True when the call targets a lock type that its owner may reacquire. Read from the
        /// callee symbol because a standard library may lower the lock to an unnamed struct,
        /// leaving the type name visible only in the mangled member function.
        [[nodiscard]] bool targetsRecursiveLock(const llvm::CallBase& call) const;
        [[nodiscard]] static std::string_view toString(CallKind kind);
    };
} // namespace ctrace::concurrency::internal::analysis
