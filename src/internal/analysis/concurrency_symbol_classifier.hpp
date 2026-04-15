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
        StdThreadCtor,
        StdThreadJoin,
        StdThreadDetach,
        StdMutexLock,
        StdMutexUnlock,
    };

    class ConcurrencySymbolClassifier
    {
      public:
        [[nodiscard]] const llvm::Function* directCallee(const llvm::CallBase& call) const;
        [[nodiscard]] CallKind classify(const llvm::CallBase& call) const;
        [[nodiscard]] static std::string_view toString(CallKind kind);
    };
} // namespace ctrace::concurrency::internal::analysis
