// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"

#include "internal/analysis/data_race_checker.hpp"
#include "internal/analysis/lock_order_analyzer.hpp"
#include "internal/analysis/missing_join_detector.hpp"
#include "internal/analysis/report_builder.hpp"
#include "internal/analysis/tu_facts_builder.hpp"

#include <utility>

namespace ctrace::concurrency
{
    SingleTUConcurrencyAnalyzer::SingleTUConcurrencyAnalyzer(AnalysisOptions options) :
        options_(std::move(options))
    {
    }

    DiagnosticReport SingleTUConcurrencyAnalyzer::analyze(const llvm::Module& module) const
    {
        internal::analysis::TUFactsBuilder factsBuilder;
        const internal::analysis::TUFacts facts = factsBuilder.build(module);

        auto appendDiagnostics = [](DiagnosticReport& report,
                                    const DiagnosticReport& partialReport) -> void
        {
            report.diagnostics.insert(report.diagnostics.end(), partialReport.diagnostics.begin(),
                                      partialReport.diagnostics.end());
        };

        DiagnosticReport report;
        if (options_.isEnabled(RuleId::DataRaceGlobal))
        {
            internal::analysis::DataRaceChecker dataRaceChecker;
            appendDiagnostics(report, dataRaceChecker.run(module, facts));
        }

        if (options_.isEnabled(RuleId::DeadlockLockOrder))
        {
            internal::analysis::LockOrderAnalyzer lockOrderAnalyzer;
            appendDiagnostics(report, lockOrderAnalyzer.run(facts));
        }

        if (options_.isEnabled(RuleId::MissingJoin))
        {
            internal::analysis::MissingJoinDetector missingJoinDetector;
            appendDiagnostics(report, missingJoinDetector.run(facts));
        }

        internal::analysis::finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency
