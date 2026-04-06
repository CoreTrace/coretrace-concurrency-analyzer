// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    class LockScopeTracker
    {
      public:
        explicit LockScopeTracker(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::unordered_map<const llvm::Instruction*, std::set<std::string>>
        collectHeldLocks(const llvm::Function& function,
                         const std::unordered_set<const llvm::Instruction*>& trackedAccesses) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
