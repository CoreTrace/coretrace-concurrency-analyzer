// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <optional>
#include <string_view>
#include <unordered_map>

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
        /// Duplicates the calling process. The copy inherits the address space but only the
        /// calling thread, which is what makes it delicate in a threaded program.
        ProcessFork,
        /// Replaces the process image, which discards everything the fork inherited.
        ProcessExec,
        /// Collects a terminated child, releasing its entry in the process table.
        ProcessWait,
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
        /// True for a call a signal handler may not make. POSIX allows only async-signal-safe
        /// functions there: a handler interrupts its own thread mid-operation, so allocating,
        /// printing or locking can re-enter a structure the interrupted code left inconsistent.
        [[nodiscard]] bool isAsyncSignalUnsafe(const llvm::CallBase& call) const;
        /// True when the call hands child termination to the system, which reaps the children
        /// itself. A program that does this leaves no zombie behind and owes no wait.
        [[nodiscard]] bool ignoresChildTermination(const llvm::CallBase& call) const;
        /// The function a `signal` or `sigaction` call installs, when it can be read from the
        /// call site. Null for any other call, or an installation this analysis cannot follow.
        [[nodiscard]] const llvm::Function*
        installedSignalHandler(const llvm::CallBase& call) const;
        [[nodiscard]] static std::string_view toString(CallKind kind);

      private:
        /// What classify() answers depends only on the callee, and a translation unit calls the
        /// same handful of functions over and over. Mutable because the answer is a memo, not a
        /// change of state: a classifier remains logically const.
        ///
        /// Not synchronised, and does not need to be: cross-TU analysis parallelises per unit,
        /// and each unit's facts are built with their own classifier.
        mutable std::unordered_map<const llvm::Function*, CallKind> classificationCache_;
    };
} // namespace ctrace::concurrency::internal::analysis
