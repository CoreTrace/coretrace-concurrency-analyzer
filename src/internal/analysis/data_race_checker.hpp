// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "facts.hpp"

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    class DataRaceChecker
    {
      public:
        [[nodiscard]] DiagnosticReport run(const llvm::Module& module, const TUFacts& facts) const;
    };
} // namespace ctrace::concurrency::internal::analysis
