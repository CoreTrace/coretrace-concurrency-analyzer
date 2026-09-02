// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "lock_wrapper_summaries.hpp"
#include "shared_object_binding_collector.hpp"

#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class CallBase;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    struct DirectCallSite;

    struct LockPropagationResult
    {
        std::unordered_map<std::string, std::set<std::string>> entryLocksByFunction;
        std::unordered_map<const llvm::CallBase*, std::set<std::string>> effectiveHeldLocksByCall;
    };

    class LockStatePropagator
    {
      public:
        explicit LockStatePropagator(const ConcurrencySymbolClassifier& classifier,
                                     const LockWrapperSummaries* summaries = nullptr,
                                     const SharedObjectBindings* sharedObjects = nullptr);

        [[nodiscard]] LockPropagationResult
        collect(const llvm::Module& module, const std::vector<DirectCallSite>& callSites) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
        const LockWrapperSummaries* summaries_ = nullptr;
        const SharedObjectBindings* sharedObjects_ = nullptr;
    };
} // namespace ctrace::concurrency::internal::analysis
