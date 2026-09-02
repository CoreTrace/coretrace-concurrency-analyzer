// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;

    /// Finds condition-variable waits that check nothing when they wake.
    ///
    /// A wait may return without the condition holding: the standard allows it, and a
    /// `notify_all` wakes every waiter while only one may proceed. The predicate overload
    /// rechecks on its own; the bare one leaves that to the caller, whose only way to express it
    /// is a loop around the wait. A bare wait with no loop around it therefore treats "I woke"
    /// as "the condition holds", which is exactly the bug.
    class ConditionWaitCollector
    {
      public:
        explicit ConditionWaitCollector(const ConcurrencySymbolClassifier& classifier);

        [[nodiscard]] std::vector<ConditionWaitFact> collect(const llvm::Module& module) const;

      private:
        const ConcurrencySymbolClassifier& classifier_;
    };
} // namespace ctrace::concurrency::internal::analysis
