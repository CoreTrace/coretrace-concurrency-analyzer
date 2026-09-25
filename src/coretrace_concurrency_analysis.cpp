// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"

#include "internal/analysis/condition_wait_checker.hpp"
#include "internal/analysis/process_lifecycle_checker.hpp"
#include "internal/analysis/publication_ordering_checker.hpp"
#include "internal/analysis/cross_tu/inter_tu_coordinator.hpp"
#include "internal/analysis/data_race_checker.hpp"
#include "internal/analysis/fact_selection.hpp"
#include "internal/analysis/lock_order_analyzer.hpp"
#include "internal/analysis/missing_join_detector.hpp"
#include "internal/analysis/report_builder.hpp"
#include "internal/analysis/tu_facts_builder.hpp"
#include "internal/ir_loader.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/xxhash.h>

#include <numeric>
#include <string_view>
#include <utility>

namespace ctrace::concurrency
{
    namespace
    {
        void setBitcodeReadError(CompileError& error, std::string message)
        {
            error.code = make_error_code(CompileErrc::BitcodeReadFailed);
            error.phase = CompilePhase::IRParse;
            error.message = std::move(message);
        }

        /// Reads the whole file rather than mapping it: a mapping would fault if another process
        /// truncated the file while the parser reads it.
        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
        readBitcodeFile(const std::filesystem::path& path)
        {
            return llvm::MemoryBuffer::getFile(path.string(), /*IsText=*/false,
                                               /*RequiresNullTerminator=*/false,
                                               /*IsVolatile=*/true);
        }

        std::string cannotRead(const std::filesystem::path& path, const std::error_code& error)
        {
            return "cannot read bitcode from '" + path.string() + "': " + error.message();
        }
    } // namespace

    LoadedUnit::LoadedUnit() = default;

    LoadedUnit::LoadedUnit(std::unique_ptr<llvm::LLVMContext> context,
                           std::unique_ptr<llvm::Module> module,
                           std::unique_ptr<llvm::MemoryBuffer> bitcode)
        : bitcode(std::move(bitcode)), context(std::move(context)), module(std::move(module))
    {
    }

    LoadedUnit::~LoadedUnit() = default;

    LoadedUnit::LoadedUnit(LoadedUnit&&) noexcept = default;

    void ProjectUnitSource::add(std::string identifier, std::string bitcode)
    {
        units_.push_back(Unit{.identifier = std::move(identifier), .bitcode = std::move(bitcode)});
    }

    void ProjectUnitSource::addFile(std::string identifier, std::filesystem::path path)
    {
        FileBitcode file{.path = std::move(path)};
        // The bytes are released as soon as their size and digest are known.
        if (llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer = readBitcodeFile(file.path))
        {
            file.size = (*buffer)->getBufferSize();
            file.digest = llvm::xxh3_64bits((*buffer)->getBuffer());
        }
        else
        {
            setBitcodeReadError(file.error, cannotRead(file.path, buffer.getError()));
        }
        units_.push_back(Unit{.identifier = std::move(identifier), .file = std::move(file)});
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
        std::string_view bitcode = unit.bitcode;
        std::unique_ptr<llvm::MemoryBuffer> fileBytes;
        if (unit.file.has_value())
        {
            const FileBitcode& file = *unit.file;
            if (file.error.hasError())
            {
                error = file.error;
                return {};
            }
            llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer = readBitcodeFile(file.path);
            if (!buffer)
            {
                setBitcodeReadError(error, cannotRead(file.path, buffer.getError()));
                return {};
            }
            if ((*buffer)->getBufferSize() != file.size ||
                llvm::xxh3_64bits((*buffer)->getBuffer()) != file.digest)
            {
                setBitcodeReadError(error, "bitcode in '" + file.path.string() +
                                               "' changed since the unit was added");
                return {};
            }
            fileBytes = std::move(*buffer);
            bitcode = std::string_view(fileBytes->getBufferStart(), fileBytes->getBufferSize());
        }

        // A context of its own per load: contexts are not thread-safe, and nothing in the
        // analysis compares type pointers across units.
        auto context = std::make_unique<llvm::LLVMContext>();
        std::unique_ptr<llvm::Module> module =
            internal::LLVMIRLoader().parseBC(bitcode, unit.identifier, *context, error);
        if (module == nullptr)
            return {};

        return {std::move(context), std::move(module), std::move(fileBytes)};
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
        const internal::analysis::FactSelection selection =
            internal::analysis::FactSelection::forRules(options_.enabledRules, false);
        const internal::analysis::TUFacts facts = factsBuilder.build(module, selection);

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

        if (options_.isEnabled(RuleId::WeakPublicationOrdering))
        {
            internal::analysis::PublicationOrderingChecker publicationChecker;
            appendDiagnostics(report, publicationChecker.run(facts));
        }

        if (options_.isEnabled(RuleId::ForkAfterThreadCreation) ||
            options_.isEnabled(RuleId::UnreapedChildProcess) ||
            options_.isEnabled(RuleId::ThreadArgumentEscapesFrame) ||
            options_.isEnabled(RuleId::ThreadArgumentFreedEarly) ||
            options_.isEnabled(RuleId::ThreadLocalOutlivesThread) ||
            options_.isEnabled(RuleId::UnsafeSignalHandler))
        {
            internal::analysis::ProcessLifecycleChecker processChecker;
            DiagnosticReport processReport = processChecker.run(facts);
            std::erase_if(processReport.diagnostics, [this](const Diagnostic& diagnostic)
                          { return !options_.isEnabled(diagnostic.ruleId); });
            appendDiagnostics(report, processReport);
        }

        internal::analysis::finalizeReport(report, facts, selection.accesses);
        return report;
    }
} // namespace ctrace::concurrency
