// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "facts.hpp"

namespace ctrace::concurrency::internal::analysis
{
    [[nodiscard]] DiagnosticSummary computeSummary(const std::vector<Diagnostic>& diagnostics);
    [[nodiscard]] std::vector<FunctionSummary>
    buildFunctionSummaries(const TUFacts& facts, const std::vector<Diagnostic>& diagnostics);
    void finalizeReport(DiagnosticReport& report, const TUFacts& facts);
} // namespace ctrace::concurrency::internal::analysis
