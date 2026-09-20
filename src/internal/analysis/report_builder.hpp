// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "facts.hpp"

namespace ctrace::concurrency::internal::analysis
{
    [[nodiscard]] DiagnosticSummary computeSummary(const std::vector<Diagnostic>& diagnostics);
    [[nodiscard]] std::vector<FunctionSummary>
    buildFunctionSummaries(const TUFacts& facts, const std::vector<Diagnostic>& diagnostics,
                           bool accessesWereComputed);
    /// `accessesWereComputed` says whether a selected rule asked for the shared accesses. The
    /// function summaries report what was computed; without this they would report zero for
    /// counts nobody took.
    void finalizeReport(DiagnosticReport& report, const TUFacts& facts, bool accessesWereComputed);
} // namespace ctrace::concurrency::internal::analysis
