// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "facts.hpp"

namespace ctrace::concurrency::internal::analysis
{
    /// Reports data published through an atomic flag with an ordering too weak to publish it: a
    /// store that does not release, or an observing load that does not acquire.
    class PublicationOrderingChecker
    {
      public:
        [[nodiscard]] DiagnosticReport run(const TUFacts& facts) const;
    };
} // namespace ctrace::concurrency::internal::analysis
