// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "facts.hpp"

namespace ctrace::concurrency::internal::analysis
{
    /// Reports what a program does wrong around `fork`: forking while threads run, and never
    /// collecting the children it creates.
    class ProcessLifecycleChecker
    {
      public:
        [[nodiscard]] DiagnosticReport run(const TUFacts& facts) const;
    };
} // namespace ctrace::concurrency::internal::analysis
