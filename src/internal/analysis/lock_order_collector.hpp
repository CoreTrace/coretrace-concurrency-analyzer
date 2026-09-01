// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    class LockOrderCollector
    {
      public:
        using LiveEntriesByInstruction =
            std::unordered_map<const llvm::Instruction*, ThreadEntrySet>;

        explicit LockOrderCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::vector<LockOrderFact>
        collect(const llvm::Function& function, const std::set<std::string>& initialHeldLocks = {},
                const LiveEntriesByInstruction& liveEntriesByInstruction = {}) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
