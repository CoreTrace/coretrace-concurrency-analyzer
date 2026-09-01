// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "facts.hpp"

namespace ctrace::concurrency::internal::analysis
{
    /// Reports condition-variable waits that treat waking up as proof the condition holds.
    class ConditionWaitChecker
    {
      public:
        [[nodiscard]] DiagnosticReport run(const TUFacts& facts) const;
    };
} // namespace ctrace::concurrency::internal::analysis
