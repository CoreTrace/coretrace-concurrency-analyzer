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
            return name;
        }

        bool isStdThreadCtor(llvm::StringRef name)
        {
            return name.contains("thread") &&
                   (name.contains("threadC1") || name.contains("threadC2"));
        }

        bool isStdMutexLock(llvm::StringRef name)
        {
            return name.contains("mutex") && name.contains("4lockEv");
        }

        bool isStdMutexUnlock(llvm::StringRef name)
        {
            return name.contains("mutex") && name.contains("6unlockEv");
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
        if (name == "pthread_create")
            return CallKind::PThreadCreate;
        if (name == "pthread_mutex_lock")
            return CallKind::PThreadMutexLock;
        if (name == "pthread_mutex_unlock")
            return CallKind::PThreadMutexUnlock;
        if (isStdThreadCtor(name))
            return CallKind::StdThreadCtor;
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
        case CallKind::PThreadMutexLock:
            return "pthread_mutex_lock";
        case CallKind::PThreadMutexUnlock:
            return "pthread_mutex_unlock";
        case CallKind::StdThreadCtor:
            return "std_thread_ctor";
        case CallKind::StdMutexLock:
            return "std_mutex_lock";
        case CallKind::StdMutexUnlock:
            return "std_mutex_unlock";
        }
        return "unknown";
    }
} // namespace ctrace::concurrency::internal::analysis
