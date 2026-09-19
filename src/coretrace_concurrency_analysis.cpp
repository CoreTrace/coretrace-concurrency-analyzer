// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"

#include "internal/analysis/condition_wait_checker.hpp"
#include "internal/analysis/process_lifecycle_checker.hpp"
#include "internal/analysis/cross_tu/inter_tu_coordinator.hpp"
#include "internal/analysis/data_race_checker.hpp"
#include "internal/analysis/lock_order_analyzer.hpp"
#include "internal/analysis/missing_join_detector.hpp"
#include "internal/analysis/report_builder.hpp"
#include "internal/analysis/tu_facts_builder.hpp"
#include "internal/ir_loader.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <numeric>
#include <utility>

namespace ctrace::concurrency
{
    LoadedUnit::LoadedUnit() = default;

    LoadedUnit::LoadedUnit(std::unique_ptr<llvm::LLVMContext> context,
                           std::unique_ptr<llvm::Module> module)
        : context(std::move(context)), module(std::move(module))
    {
    }

    LoadedUnit::~LoadedUnit() = default;

    LoadedUnit::LoadedUnit(LoadedUnit&&) noexcept = default;

    void ProjectUnitSource::add(std::string identifier, std::string bitcode)
    {
        units_.push_back(Unit{.identifier = std::move(identifier), .bitcode = std::move(bitcode)});
    }

    std::size_t ProjectUnitSource::bitcodeBytes() const noexcept
    {
        return std::accumulate(units_.begin(), units_.end(), std::size_t{0},
                               [](std::size_t total, const Unit& unit)
                               { return total + unit.bitcode.size(); });
    }

    LoadedUnit ProjectUnitSource::load(std::size_t index, CompileError& error) const
    {
        const Unit& unit = units_.at(index);
        // A context of its own per load: contexts are not thread-safe, and nothing in the
        // analysis compares type pointers across units.
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module =
            internal::LLVMIRLoader().parseBC(unit.bitcode, unit.identifier, *context, error);
        if (module == nullptr)
            return {};

        return {std::move(context), std::move(module)};
    }

    ProjectConcurrencyAnalyzer::ProjectConcurrencyAnalyzer(AnalysisOptions options)
        : options_(std::move(options))
    {
    }

    ProjectAnalysisReport ProjectConcurrencyAnalyzer::analyze(const ProjectUnitSource& units) const
    {
        internal::analysis::cross_tu::InterTUCoordinator coordinator(options_);
        internal::analysis::cross_tu::ProjectAnalysis analysis = coordinator.analyze(units);

        return ProjectAnalysisReport{
            .report = std::move(analysis.report),
            .skippedIncompatibleModules = std::move(analysis.skippedIncompatibleModules),
            .reanalyzedUnitCount = analysis.reanalyzedUnitCount,
            .failedUnits = std::move(analysis.failedUnits),
            .peakLiveUnits = analysis.peakLiveUnits,
            .loadMilliseconds = analysis.loadMilliseconds,
        };
    }

    SingleTUConcurrencyAnalyzer::SingleTUConcurrencyAnalyzer(AnalysisOptions options)
        : options_(std::move(options))
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
            appendDiagnostics(report, dataRaceChecker.run(facts));
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

        if (options_.isEnabled(RuleId::ConditionWaitWithoutPredicate))
        {
            internal::analysis::ConditionWaitChecker conditionWaitChecker;
            appendDiagnostics(report, conditionWaitChecker.run(facts));
        }

        if (options_.isEnabled(RuleId::ForkAfterThreadCreation) ||
            options_.isEnabled(RuleId::UnreapedChildProcess) ||
            options_.isEnabled(RuleId::ThreadArgumentEscapesFrame) ||
            options_.isEnabled(RuleId::UnsafeSignalHandler))
        {
            internal::analysis::ProcessLifecycleChecker processChecker;
            DiagnosticReport processReport = processChecker.run(facts);
            std::erase_if(processReport.diagnostics, [this](const Diagnostic& diagnostic)
                          { return !options_.isEnabled(diagnostic.ruleId); });
            appendDiagnostics(report, processReport);
        }

        internal::analysis::finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency
