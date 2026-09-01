// SPDX-License-Identifier: Apache-2.0
#include "concurrency_symbol_classifier.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Value.h>

#include <cctype>
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

        bool matchesPlainSymbol(llvm::StringRef actual, llvm::StringRef expected)
        {
            return actual == expected ||
                   (actual.starts_with("_") && actual.drop_front() == expected);
        }

        bool isStdMutexLock(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("mutex"))
                return false;

            return name.contains("4lockEv");
        }

        bool isStdMutexUnlock(llvm::StringRef name)
        {
            if (!isStdNamespaceSymbol(name) || !name.contains("mutex"))
                return false;

            return name.contains("6unlockEv");
        }

        bool isStdThreadJoin(llvm::StringRef name)
        {
            return isStdNamespaceSymbol(name) && name.contains("thread") &&
                   name.contains("4joinEv");
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
        if (matchesPlainSymbol(name, "pthread_mutex_lock"))
            return CallKind::PThreadMutexLock;
        if (matchesPlainSymbol(name, "pthread_mutex_unlock"))
            return CallKind::PThreadMutexUnlock;
        if (isStdThreadMove(name))
            return CallKind::StdThreadMove;
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
