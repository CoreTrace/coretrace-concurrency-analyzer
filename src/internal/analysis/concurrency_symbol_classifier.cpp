// SPDX-License-Identifier: Apache-2.0
#include "concurrency_symbol_classifier.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Removes Itanium ABI tags (`B<length><chars>`, as in `threadC1B8ne200100Ev`) so that the
        /// structural substrings below match the same way on a tagged and an untagged build.
        /// libc++ tags every function hidden from its ABI, which is most of `<thread>` and
        /// `<mutex>`.
        std::string stripAbiTags(llvm::StringRef name)
        {
            std::string stripped;
            stripped.reserve(name.size());

            for (std::size_t index = 0; index < name.size();)
            {
                if (name[index] != 'B' || index + 1 >= name.size() ||
                    std::isdigit(static_cast<unsigned char>(name[index + 1])) == 0)
                {
                    stripped.push_back(name[index]);
                    ++index;
                    continue;
                }

                std::size_t digitsEnd = index + 1;
                while (digitsEnd < name.size() &&
                       std::isdigit(static_cast<unsigned char>(name[digitsEnd])) != 0)
                {
                    ++digitsEnd;
                }

                const std::size_t tagLength =
                    std::stoul(name.substr(index + 1, digitsEnd - index - 1).str());
                if (digitsEnd + tagLength > name.size())
                {
                    stripped.push_back(name[index]);
                    ++index;
                    continue;
                }

                index = digitsEnd + tagLength;
            }

            return stripped;
        }

        std::string canonicalName(const llvm::Function& function)
        {
            llvm::StringRef name = function.getName();
            if (name.starts_with("\x01"))
                name = name.drop_front();
            if (name.starts_with("\\01"))
                name = name.drop_front(3);

            // Plain C symbols carry no tags; stripping is a no-op for them.
            return name.starts_with("_Z") ? stripAbiTags(name) : name.str();
        }

        /// Itanium-mangled entities declared in namespace `std` start with `_ZNSt`/`_ZNKSt`
        /// (libstdc++) or embed the inline versioning namespace (`_ZNSt3__1`, libc++). Gating the
        /// substring heuristics on this prevents a user type whose name merely contains "thread" or
        /// "mutex" from being mistaken for a standard library primitive.
        bool isStdNamespaceSymbol(llvm::StringRef name)
        {
            return name.starts_with("_ZNSt") || name.starts_with("_ZNKSt") ||
                   name.starts_with("_ZSt") || name.starts_with("_ZNVSt");
        }

        bool isStdThreadCtor(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("thread"))
                return false;

            const bool isCtor = name.contains("threadC1") || name.contains("threadC2");
            if (!isCtor)
                return false;

            // Exclude default/copy/move constructors; only thread-starting constructors
            // should materialize lifecycle create facts.
            if (name.contains("threadC1Ev") || name.contains("threadC2Ev") ||
                name.contains("ERKS0_") || name.contains("ERKS_") || name.contains("EOS0_") ||
                name.contains("EOS_"))
            {
                return false;
            }

            return true;
        }

        /// `std::jthread` joins in its destructor, so its construction must not be reported as an
        /// unjoined handle. Its mangling contains "threadC1"/"threadC2" and would otherwise be
        /// classified as a plain `std::thread` constructor.
        bool isStdJThreadCtor(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && name.contains("jthread") &&
                   (name.contains("threadC1") || name.contains("threadC2"));
        }

        bool isStdThreadMove(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("thread") ||
                (!name.contains("EOS0_") && !name.contains("EOS_")))
            {
                return false;
            }

            const bool isMoveCtor = name.contains("threadC1") || name.contains("threadC2");
            const bool isMoveAssignment = name.contains("threadaS");
            return isMoveCtor || isMoveAssignment;
        }

        bool isLockGuardTemplate(llvm::StringRef name)
        {
            return name.contains("lock_guard") || name.contains("unique_lock") ||
                   name.contains("scoped_lock") || name.contains("shared_lock");
        }

        bool isStdLockGuardCtor(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && isLockGuardTemplate(name) &&
                   (name.contains("C1E") || name.contains("C2E"));
        }

        /// `unique_lock(m, std::defer_lock)` stores the mutex without locking it.
        bool isDeferredLockGuardCtor(llvm::StringRef name)
        {
            return name.contains("defer_lock");
        }

        bool isStdLockGuardDtor(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && isLockGuardTemplate(name) &&
                   (name.contains("D1Ev") || name.contains("D2Ev"));
        }

        bool matchesPlainSymbol(llvm::StringRef actual, llvm::StringRef expected)
        {
            return actual == expected ||
                   (actual.starts_with("_") && actual.drop_front() == expected);
        }

        bool isStdMutexLock(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("mutex"))
                return false;

            return name.contains("4lockEv") || name.contains("11lock_sharedEv");
        }

        bool isStdMutexUnlock(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("mutex"))
                return false;

            return name.contains("6unlockEv") || name.contains("13unlock_sharedEv");
        }

        bool isStdMutexTryLock(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("mutex"))
                return false;

            return name.contains("8try_lockEv") || name.contains("15try_lock_sharedEv");
        }

        /// Whether a condition-variable wait rechecks the condition itself, or leaves that to
        /// its caller. Nothing but the callee's own signature can answer it.
        ///
        /// Counting arguments does not work: the usual predicate is a captureless lambda, an
        /// empty class that clang drops from the lowered signature, so both overloads arrive
        /// with the same arity. The mangled name keeps what the lowering discards.
        std::optional<bool> conditionWaitRechecksItself(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("condition_variable"))
                return std::nullopt;

            // `wait` comes in two shapes: the bare one is an ordinary member, the predicate one
            // is a template, and the mangling marks the difference with `I` against `E`.
            if (name.contains("4waitE"))
                return false;
            if (name.contains("4waitI"))
                return true;

            // `wait_for` and `wait_until` are both templates, so template arguments separate
            // nothing. Their return type does: the bare form yields `cv_status`, the predicate
            // form yields `bool`.
            if (name.contains("wait_for") || name.contains("wait_until"))
                return !name.contains("9cv_status");

            return std::nullopt;
        }

        bool isStdThreadJoin(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && name.contains("thread") &&
                   name.contains("4joinEv");
        }

        /// `~thread()` neither joins nor detaches; it terminates when the handle is still
        /// joinable. It is recognized only so that it is not mistaken for an ownership transfer.
        bool isStdThreadDtor(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && name.contains("thread") &&
                   (name.contains("threadD1") || name.contains("threadD2"));
        }

        bool isStdThreadDetach(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && name.contains("thread") &&
                   name.contains("6detachEv");
        }
    } // namespace

    const llvm::Function*
    ConcurrencySymbolClassifier::directCallee(const llvm::CallBase& call) const
    {
        const llvm::Value* calledOperand = call.getCalledOperand();
        if (calledOperand == nullptr)
            return nullptr;

        calledOperand = calledOperand->stripPointerCasts();
        return llvm::dyn_cast<llvm::Function>(calledOperand);
    }

    bool ConcurrencySymbolClassifier::targetsRecursiveLock(const llvm::CallBase& call) const
    {
        const llvm::Function* callee = directCallee(call);
        if (callee == nullptr)
            return false;

        const std::string canonical = canonicalName(*callee);
        return llvm::StringRef(canonical).contains("recursive_mutex") ||
               llvm::StringRef(canonical).contains("recursive_timed_mutex");
    }

    namespace
    {
        /// The function a value denotes, once casts are stripped. Kept local so the classifier
        /// stays free of the IR-walking layer that depends on it.
        const llvm::Function* functionBehind(const llvm::Value& value)
        {
            return llvm::dyn_cast<llvm::Function>(value.stripPointerCastsAndAliases());
        }
    } // namespace

    bool ConcurrencySymbolClassifier::isAsyncSignalUnsafe(const llvm::CallBase& call) const
    {
        const llvm::Function* callee = directCallee(call);
        if (callee == nullptr)
            return false;

        const std::string canonical = canonicalName(*callee);
        const llvm::StringRef name = canonical;

        // Deliberately short: only calls whose unsafety is not a matter of interpretation. A
        // handler that allocates can deadlock against an interrupted allocation, one that prints
        // can corrupt a stream mid-write, and one that locks can wait on a mutex its own thread
        // already holds.
        static constexpr std::string_view kUnsafe[] = {
            "malloc",  "calloc",  "realloc",  "free",    "exit",     "printf",
            "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "puts",
            "fputs",   "putchar", "fopen",    "fclose",  "fwrite",   "fread",
        };
        for (const std::string_view unsafe : kUnsafe)
        {
            if (matchesPlainSymbol(name, unsafe))
                return true;
        }

        // Operator new and delete, in every spelling the ABI gives them.
        if (name.starts_with("_Znw") || name.starts_with("_Zna") || name.starts_with("_ZdlPv") ||
            name.starts_with("_ZdaPv"))
        {
            return true;
        }

        switch (classify(call))
        {
        case CallKind::PThreadMutexLock:
        case CallKind::PThreadMutexUnlock:
        case CallKind::PThreadMutexTryLock:
        case CallKind::PThreadRwLockAcquire:
        case CallKind::PThreadRwLockTryAcquire:
        case CallKind::PThreadRwLockUnlock:
        case CallKind::StdMutexLock:
        case CallKind::StdMutexUnlock:
        case CallKind::StdMutexTryLock:
        case CallKind::StdLockGuardCtor:
        case CallKind::StdLockGuardDtor:
        case CallKind::CondWaitWithoutPredicate:
        case CallKind::CondWaitWithPredicate:
            return true;
        default:
            return false;
        }
    }

    bool ConcurrencySymbolClassifier::ignoresChildTermination(const llvm::CallBase& call) const
    {
        constexpr unsigned kSignalNumberOperandIndex = 0;
        constexpr unsigned kSignalHandlerOperandIndex = 1;
        // SIGCHLD is not the same number everywhere: 17 on Linux, 20 on the BSDs and Darwin.
        // Both are accepted rather than derived from the target, which would tie this rule to a
        // triple it has no other reason to read.
        constexpr std::int64_t kSigChldNumbers[] = {17, 20};
        // SIG_IGN is the integer 1 given a function-pointer type.
        constexpr std::int64_t kSigIgn = 1;

        const llvm::Function* callee = directCallee(call);
        if (callee == nullptr || call.arg_size() <= kSignalHandlerOperandIndex)
            return false;

        const std::string canonical = canonicalName(*callee);
        const llvm::StringRef name = canonical;
        if (!matchesPlainSymbol(name, "signal") && !matchesPlainSymbol(name, "bsd_signal") &&
            !matchesPlainSymbol(name, "sigset"))
        {
            return false;
        }

        const auto* signalNumber =
            llvm::dyn_cast<llvm::ConstantInt>(call.getArgOperand(kSignalNumberOperandIndex));
        if (signalNumber == nullptr)
            return false;

        const bool targetsChildTermination =
            std::find(std::begin(kSigChldNumbers), std::end(kSigChldNumbers),
                      signalNumber->getSExtValue()) != std::end(kSigChldNumbers);
        if (!targetsChildTermination)
            return false;

        const auto* handler =
            llvm::dyn_cast<llvm::ConstantExpr>(call.getArgOperand(kSignalHandlerOperandIndex));
        if (handler == nullptr || handler->getOpcode() != llvm::Instruction::IntToPtr)
            return false;

        const auto* handlerValue = llvm::dyn_cast<llvm::ConstantInt>(handler->getOperand(0));
        return handlerValue != nullptr && handlerValue->getSExtValue() == kSigIgn;
    }

    const llvm::Function*
    ConcurrencySymbolClassifier::installedSignalHandler(const llvm::CallBase& call) const
    {
        constexpr unsigned kSignalHandlerOperandIndex = 1;
        constexpr unsigned kSigactionStructOperandIndex = 1;

        const llvm::Function* callee = directCallee(call);
        if (callee == nullptr)
            return nullptr;

        const std::string canonical = canonicalName(*callee);
        const llvm::StringRef name = canonical;

        if (matchesPlainSymbol(name, "signal") || matchesPlainSymbol(name, "bsd_signal") ||
            matchesPlainSymbol(name, "sigset"))
        {
            if (call.arg_size() <= kSignalHandlerOperandIndex)
                return nullptr;

            return functionBehind(*call.getArgOperand(kSignalHandlerOperandIndex));
        }

        if (!matchesPlainSymbol(name, "sigaction") ||
            call.arg_size() <= kSigactionStructOperandIndex)
            return nullptr;

        // `sigaction` takes the handler inside a struct the caller filled in, so the function
        // appears as a store into that storage rather than as an operand.
        const llvm::Value* action =
            call.getArgOperand(kSigactionStructOperandIndex)->stripPointerCastsAndAliases();
        const auto* storage = llvm::dyn_cast<llvm::AllocaInst>(action);
        if (storage == nullptr)
            return nullptr;

        for (const llvm::User* user : storage->users())
        {
            const llvm::Value* candidate = user;
            if (const auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(user))
            {
                for (const llvm::User* nested : gep->users())
                {
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(nested))
                    {
                        if (const llvm::Function* handler =
                                functionBehind(*store->getValueOperand()))
                        {
                            return handler;
                        }
                    }
                }
                continue;
            }

            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(candidate))
            {
                if (const llvm::Function* handler = functionBehind(*store->getValueOperand()))
                    return handler;
            }
        }

        return nullptr;
    }

    CallKind ConcurrencySymbolClassifier::classify(const llvm::CallBase& call) const
    {
        const llvm::Function* callee = directCallee(call);
        if (callee == nullptr)
            return CallKind::Unknown;

        const std::string canonical = canonicalName(*callee);
        const llvm::StringRef name = canonical;
        if (matchesPlainSymbol(name, "pthread_create"))
            return CallKind::PThreadCreate;
        if (matchesPlainSymbol(name, "pthread_join"))
            return CallKind::PThreadJoin;
        if (matchesPlainSymbol(name, "pthread_detach"))
            return CallKind::PThreadDetach;
        if (matchesPlainSymbol(name, "fork") || matchesPlainSymbol(name, "vfork"))
            return CallKind::ProcessFork;
        if (matchesPlainSymbol(name, "execl") || matchesPlainSymbol(name, "execlp") ||
            matchesPlainSymbol(name, "execle") || matchesPlainSymbol(name, "execv") ||
            matchesPlainSymbol(name, "execvp") || matchesPlainSymbol(name, "execvpe") ||
            matchesPlainSymbol(name, "execve") || matchesPlainSymbol(name, "posix_spawn") ||
            matchesPlainSymbol(name, "posix_spawnp"))
        {
            return CallKind::ProcessExec;
        }
        if (matchesPlainSymbol(name, "wait") || matchesPlainSymbol(name, "waitpid") ||
            matchesPlainSymbol(name, "waitid") || matchesPlainSymbol(name, "wait3") ||
            matchesPlainSymbol(name, "wait4"))
        {
            return CallKind::ProcessWait;
        }
        if (matchesPlainSymbol(name, "pthread_cond_wait") ||
            matchesPlainSymbol(name, "pthread_cond_timedwait"))
        {
            return CallKind::CondWaitWithoutPredicate;
        }
        if (matchesPlainSymbol(name, "pthread_mutex_lock"))
            return CallKind::PThreadMutexLock;
        if (matchesPlainSymbol(name, "pthread_mutex_unlock"))
            return CallKind::PThreadMutexUnlock;
        if (matchesPlainSymbol(name, "pthread_mutex_trylock") ||
            matchesPlainSymbol(name, "pthread_mutex_timedlock"))
            return CallKind::PThreadMutexTryLock;
        if (matchesPlainSymbol(name, "pthread_rwlock_rdlock") ||
            matchesPlainSymbol(name, "pthread_rwlock_wrlock"))
            return CallKind::PThreadRwLockAcquire;
        if (matchesPlainSymbol(name, "pthread_rwlock_tryrdlock") ||
            matchesPlainSymbol(name, "pthread_rwlock_trywrlock") ||
            matchesPlainSymbol(name, "pthread_rwlock_timedrdlock") ||
            matchesPlainSymbol(name, "pthread_rwlock_timedwrlock"))
            return CallKind::PThreadRwLockTryAcquire;
        if (matchesPlainSymbol(name, "pthread_rwlock_unlock"))
            return CallKind::PThreadRwLockUnlock;
        if (matchesPlainSymbol(name, "pthread_spin_lock"))
            return CallKind::PThreadSpinLock;
        if (matchesPlainSymbol(name, "pthread_spin_unlock"))
            return CallKind::PThreadSpinUnlock;
        if (matchesPlainSymbol(name, "pthread_spin_trylock"))
            return CallKind::PThreadSpinTryLock;
        if (matchesPlainSymbol(name, "pthread_mutex_init"))
            return CallKind::PThreadMutexInit;
        if (matchesPlainSymbol(name, "pthread_mutexattr_settype"))
            return CallKind::PThreadMutexAttrSetType;
        if (isStdJThreadCtor(name))
            return CallKind::StdJThreadCtor;
        if (isStdThreadMove(name))
            return CallKind::StdThreadMove;
        if (isStdThreadCtor(name))
            return CallKind::StdThreadCtor;
        if (isStdThreadJoin(name))
            return CallKind::StdThreadJoin;
        if (isStdThreadDetach(name))
            return CallKind::StdThreadDetach;
        if (isStdThreadDtor(name))
            return CallKind::StdThreadDtor;
        if (isStdMutexTryLock(name))
            return CallKind::StdMutexTryLock;
        if (isStdMutexLock(name))
            return CallKind::StdMutexLock;
        if (isStdMutexUnlock(name))
            return CallKind::StdMutexUnlock;
        if (const std::optional<bool> rechecks = conditionWaitRechecksItself(name);
            rechecks.has_value())
        {
            return *rechecks ? CallKind::CondWaitWithPredicate : CallKind::CondWaitWithoutPredicate;
        }
        if (isStdLockGuardDtor(name))
            return CallKind::StdLockGuardDtor;
        if (isStdLockGuardCtor(name))
        {
            return isDeferredLockGuardCtor(name) ? CallKind::StdLockGuardDeferredCtor
                                                 : CallKind::StdLockGuardCtor;
        }
        return CallKind::Unknown;
    }

    std::string_view ConcurrencySymbolClassifier::toString(CallKind kind)
    {
        switch (kind)
        {
        case CallKind::Unknown:
            return "unknown";
        case CallKind::PThreadCreate:
            return "pthread_create";
        case CallKind::PThreadJoin:
            return "pthread_join";
        case CallKind::PThreadDetach:
            return "pthread_detach";
        case CallKind::PThreadMutexLock:
            return "pthread_mutex_lock";
        case CallKind::PThreadMutexUnlock:
            return "pthread_mutex_unlock";
        case CallKind::PThreadMutexTryLock:
            return "pthread_mutex_trylock";
        case CallKind::PThreadRwLockAcquire:
            return "pthread_rwlock_acquire";
        case CallKind::PThreadRwLockTryAcquire:
            return "pthread_rwlock_tryacquire";
        case CallKind::PThreadRwLockUnlock:
            return "pthread_rwlock_unlock";
        case CallKind::PThreadSpinLock:
            return "pthread_spin_lock";
        case CallKind::PThreadSpinUnlock:
            return "pthread_spin_unlock";
        case CallKind::PThreadSpinTryLock:
            return "pthread_spin_trylock";
        case CallKind::PThreadMutexInit:
            return "pthread_mutex_init";
        case CallKind::PThreadMutexAttrSetType:
            return "pthread_mutexattr_settype";
        case CallKind::StdThreadDtor:
            return "std_thread_dtor";
        case CallKind::StdJThreadCtor:
            return "std_jthread_ctor";
        case CallKind::StdMutexTryLock:
            return "std_mutex_try_lock";
        case CallKind::StdLockGuardCtor:
            return "std_lock_guard_ctor";
        case CallKind::StdLockGuardDeferredCtor:
            return "std_lock_guard_deferred_ctor";
        case CallKind::StdLockGuardDtor:
            return "std_lock_guard_dtor";
        case CallKind::CondWaitWithoutPredicate:
            return "cond_wait_without_predicate";
        case CallKind::CondWaitWithPredicate:
            return "cond_wait_with_predicate";
        case CallKind::ProcessFork:
            return "fork";
        case CallKind::ProcessExec:
            return "exec";
        case CallKind::ProcessWait:
            return "wait";
        case CallKind::StdThreadCtor:
            return "std_thread_ctor";
        case CallKind::StdThreadMove:
            return "std_thread_move";
        case CallKind::StdThreadJoin:
            return "std_thread_join";
        case CallKind::StdThreadDetach:
            return "std_thread_detach";
        case CallKind::StdMutexLock:
            return "std_mutex_lock";
        case CallKind::StdMutexUnlock:
            return "std_mutex_unlock";
        }
        return "unknown";
    }
} // namespace ctrace::concurrency::internal::analysis
