// SPDX-License-Identifier: Apache-2.0
#include "concurrency_symbol_classifier.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Value.h>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        llvm::StringRef canonicalName(const llvm::Function& function)
        {
            llvm::StringRef name = function.getName();
            if (name.starts_with("\x01"))
                name = name.drop_front();
            if (name.starts_with("\\01"))
                name = name.drop_front(3);
            return name;
        }

        bool isStdThreadCtor(llvm::StringRef name)
        {
            return name.contains("thread") &&
                   (name.contains("threadC1") || name.contains("threadC2"));
        }

        bool matchesPlainSymbol(llvm::StringRef actual, llvm::StringRef expected)
        {
            return actual == expected ||
                   (actual.starts_with("_") && actual.drop_front() == expected);
        }

        bool isStdMutexLock(llvm::StringRef name)
        {
            return name.contains("mutex") && name.contains("4lockEv");
        }

        bool isStdMutexUnlock(llvm::StringRef name)
        {
            return name.contains("mutex") && name.contains("6unlockEv");
        }

        bool isStdThreadJoin(llvm::StringRef name)
        {
            return name.contains("thread") && name.contains("4joinEv");
        }

        bool isStdThreadDetach(llvm::StringRef name)
        {
            return name.contains("thread") && name.contains("6detachEv");
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

    CallKind ConcurrencySymbolClassifier::classify(const llvm::CallBase& call) const
    {
        const llvm::Function* callee = directCallee(call);
        if (callee == nullptr)
            return CallKind::Unknown;

        const llvm::StringRef name = canonicalName(*callee);
        if (matchesPlainSymbol(name, "pthread_create"))
            return CallKind::PThreadCreate;
        if (matchesPlainSymbol(name, "pthread_join"))
            return CallKind::PThreadJoin;
        if (matchesPlainSymbol(name, "pthread_detach"))
            return CallKind::PThreadDetach;
        if (matchesPlainSymbol(name, "pthread_mutex_lock"))
            return CallKind::PThreadMutexLock;
        if (matchesPlainSymbol(name, "pthread_mutex_unlock"))
            return CallKind::PThreadMutexUnlock;
        if (isStdThreadCtor(name))
            return CallKind::StdThreadCtor;
        if (isStdThreadJoin(name))
            return CallKind::StdThreadJoin;
        if (isStdThreadDetach(name))
            return CallKind::StdThreadDetach;
        if (isStdMutexLock(name))
            return CallKind::StdMutexLock;
        if (isStdMutexUnlock(name))
            return CallKind::StdMutexUnlock;
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
        case CallKind::StdThreadCtor:
            return "std_thread_ctor";
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
