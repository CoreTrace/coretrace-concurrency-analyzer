// SPDX-License-Identifier: Apache-2.0
#include "inter_tu_coordinator.hpp"

#include "internal/analysis/condition_wait_checker.hpp"
#include "internal/analysis/process_lifecycle_checker.hpp"
#include "internal/analysis/publication_ordering_checker.hpp"
#include "internal/analysis/data_race_checker.hpp"
#include "internal/analysis/fact_selection.hpp"
#include "internal/analysis/facts.hpp"
#include "internal/analysis/lock_order_analyzer.hpp"
#include "internal/analysis/missing_join_detector.hpp"
#include "internal/analysis/report_builder.hpp"
#include "internal/analysis/tu_facts_builder.hpp"
#include "program_symbol_index.hpp"

#include <llvm/IR/Module.h>
#include <llvm/Support/ThreadPool.h>
#include <llvm/Support/Threading.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>

namespace ctrace::concurrency::internal::analysis::cross_tu
{
    namespace
    {
        /// Stable identity of a diagnostic, used to drop the copies produced when several units
        /// include the same header and analyse the same inline body.
        std::string diagnosticKey(const Diagnostic& diagnostic)
        {
            return diagnostic.id + '|' + diagnostic.location.file + ':' +
                   std::to_string(diagnostic.location.line) + ':' +
                   std::to_string(diagnostic.location.column) + '|' + diagnostic.message;
        }

        /// True when the program-wide view contradicts what this unit concluded alone. Only these
        /// units are worth analysing a second time. Decided from the facts alone: the unit's
        /// module need not be live.
        bool crossTUChangesFacts(const TUFacts& facts, const ProgramSymbolIndex& index)
        {
            const ProgramSymbolFacts& program = facts.program;

            // An `extern` this unit dropped as unresolved turns out to name real storage.
            for (const std::string& global : program.declaredGlobals)
            {
                if (index.isDefinedSomewhere(global))
                    return true;
            }

            // This unit forks, and the threads that make the fork unsafe are started elsewhere.
            if (!facts.processForks.empty() && !facts.programCreatesThreads &&
                index.programCreatesThreads())
            {
                return true;
            }

            // A handle this unit creates is joined by another.
            for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
            {
                if (fact.action != ThreadLifecycleAction::Create)
                    continue;

                const auto handle = program.handleSymbolsByGroupId.find(fact.handleGroupId);
                if (handle != program.handleSymbolsByGroupId.end() &&
                    index.resolvesHandleElsewhere(handle->second))
                {
                    return true;
                }
            }

            // A helper this unit only sees declared turns out to take a lock for its caller.
            for (const std::string& function : program.declaredFunctions)
            {
                if (index.lockSummaryFor(function) != nullptr)
                    return true;
            }

            for (const auto& [id, symbol] : program.functionSymbolsById)
            {
                if (program.declaredFunctions.contains(symbol) || !index.isThreadEntry(symbol))
                    continue;

                const auto it = facts.entryConcurrency.find(id);
                if (it == facts.entryConcurrency.end())
                    return true;

                if (index.spawnCount(symbol) > it->second.staticSpawnCount ||
                    (index.spawnedInLoop(symbol) && !it->second.hasSpawnInLoop))
                {
                    return true;
                }
            }

            return false;
        }
    } // namespace

    namespace
    {
        /// Counts the units held in memory at once, including one still being parsed, and
        /// remembers the most it ever saw.
        class LiveUnitGauge
        {
          public:
            void enter() noexcept
            {
                const std::size_t now = ++live_;
                std::size_t peak = peak_.load();
                while (now > peak && !peak_.compare_exchange_weak(peak, now))
                {
                }
            }
            void leave() noexcept
            {
                --live_;
            }
            [[nodiscard]] std::size_t peak() const noexcept
            {
                return peak_.load();
            }

          private:
            std::atomic<std::size_t> live_{0};
            std::atomic<std::size_t> peak_{0};
        };

        /// Holds a slot in the gauge for the lifetime of the enclosing scope. Declared before
        /// the unit it accounts for, so it is released after that unit is destroyed.
        class LiveUnitSlot
        {
          public:
            explicit LiveUnitSlot(LiveUnitGauge& gauge) : gauge_(gauge)
            {
                gauge_.enter();
            }
            ~LiveUnitSlot()
            {
                gauge_.leave();
            }
            LiveUnitSlot(const LiveUnitSlot&) = delete;
            LiveUnitSlot& operator=(const LiveUnitSlot&) = delete;

          private:
            LiveUnitGauge& gauge_;
        };

        std::size_t liveUnitBound(const AnalysisOptions& options) noexcept
        {
            if (options.maxLiveUnits != 0)
                return options.maxLiveUnits;
            const unsigned hardware = std::thread::hardware_concurrency();
            return hardware == 0 ? 1 : hardware;
        }
    } // namespace

    InterTUCoordinator::InterTUCoordinator(AnalysisOptions options) : options_(std::move(options))
    {
    }

    ProjectAnalysis InterTUCoordinator::analyze(const ProjectUnitSource& units) const
    {
        ProjectAnalysis analysis;
        const std::size_t unitCount = units.size();
        if (unitCount == 0)
            return analysis;

        // Each worker loads one unit, analyses it and lets it go, so the number of units in
        // memory never exceeds the number of workers.
        const std::size_t bound = liveUnitBound(options_);
        LiveUnitGauge gauge;
        std::atomic<std::int64_t> loadNanoseconds{0};
        std::mutex failureMutex;
        std::vector<std::pair<std::size_t, FailedUnit>> failures;
        std::vector<char> failed(unitCount, 0);

        const TUFactsBuilder factsBuilder;
        const FactSelection selection = FactSelection::forRules(options_.enabledRules, true);
        std::vector<TUFacts> factsByUnit(unitCount);

        // Loads a unit, analyses it against `program`, and records a failure to load in place
        // of facts. A failure in the second pass discards the first pass's facts as well: they
        // are what that pass was about to correct.
        const auto analyseUnit = [&](std::size_t index, const ProgramSymbolIndex& program)
        {
            const LiveUnitSlot slot(gauge);
            const auto loadStarted = std::chrono::steady_clock::now();
            CompileError error;
            const LoadedUnit unit = units.load(index, error);
            loadNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now() - loadStarted)
                                   .count();
            if (!unit.ok())
            {
                const std::lock_guard<std::mutex> lock(failureMutex);
                failed[index] = 1;
                factsByUnit[index] = TUFacts{};
                failures.emplace_back(
                    index, FailedUnit{.identifier = units.identifier(index), .error = error});
                return;
            }

            factsByUnit[index] = factsBuilder.build(*unit.module, selection, &program);
        };

        // Pass A: every unit on its own, but already in project mode. A spawn naming a worker
        // defined elsewhere has to be recorded here, or the index built from these facts would
        // never learn that the worker is a thread entry at all. The index is empty, so nothing
        // is seeded back yet.
        const ProgramSymbolIndex nothingKnownYet;
        {
            llvm::DefaultThreadPool pool(llvm::hardware_concurrency(bound));
            for (std::size_t index = 0; index < unitCount; ++index)
                pool.async([&, index] { analyseUnit(index, nothingKnownYet); });
            pool.wait();
        }

        // Keep the largest ABI group: a project normally has one, and a stray unit compiled for
        // another target is a configuration mistake worth naming rather than silently mixing
        // in. Decided after the first pass, from the facts, so no unit has to stay loaded for
        // it; ties go to the smallest key, which keeps the choice independent of unit order.
        std::map<std::string, std::size_t> unitCountByAbi;
        for (std::size_t index = 0; index < unitCount; ++index)
        {
            if (!failed[index])
                ++unitCountByAbi[factsByUnit[index].program.abiKey];
        }
        if (unitCountByAbi.empty())
        {
            for (auto& [index, failure] : failures)
                analysis.failedUnits.push_back(std::move(failure));
            analysis.peakLiveUnits = gauge.peak();
            analysis.loadMilliseconds = loadNanoseconds / 1'000'000;
            return analysis;
        }

        const auto dominantAbi = std::max_element(unitCountByAbi.begin(), unitCountByAbi.end(),
                                                  [](const auto& lhs, const auto& rhs)
                                                  { return lhs.second < rhs.second; });

        std::vector<std::size_t> compatible;
        for (std::size_t index = 0; index < unitCount; ++index)
        {
            if (failed[index])
                continue;
            if (factsByUnit[index].program.abiKey == dominantAbi->first)
                compatible.push_back(index);
            else
                analysis.skippedIncompatibleModules.push_back(units.identifier(index));
        }

        ProgramSymbolIndex programIndex;
        for (const std::size_t index : compatible)
            programIndex.addUnit(factsByUnit[index]);

        // Pass B: only the units whose conclusions the program-wide view changes.
        {
            llvm::DefaultThreadPool pool(llvm::hardware_concurrency(bound));
            for (const std::size_t index : compatible)
            {
                if (!crossTUChangesFacts(factsByUnit[index], programIndex))
                    continue;

                ++analysis.reanalyzedUnitCount;
                pool.async([&, index] { analyseUnit(index, programIndex); });
            }
            pool.wait();
        }

        std::sort(failures.begin(), failures.end(),
                  [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
        for (auto& [index, failure] : failures)
            analysis.failedUnits.push_back(std::move(failure));
        analysis.peakLiveUnits = gauge.peak();
        analysis.loadMilliseconds = loadNanoseconds / 1'000'000;

        std::set<std::string> reportedDiagnostics;
        for (const std::size_t index : compatible)
        {
            if (failed[index])
                continue;

            const TUFacts& facts = factsByUnit[index];

            DiagnosticReport moduleReport;
            const auto append = [&moduleReport](const DiagnosticReport& partial)
            {
                moduleReport.diagnostics.insert(moduleReport.diagnostics.end(),
                                                partial.diagnostics.begin(),
                                                partial.diagnostics.end());
            };

            if (options_.isEnabled(RuleId::DataRaceGlobal))
                append(DataRaceChecker().run(facts));

            if (options_.isEnabled(RuleId::DeadlockLockOrder))
                append(LockOrderAnalyzer().run(facts));

            if (options_.isEnabled(RuleId::MissingJoin))
                append(MissingJoinDetector().run(facts));

            if (options_.isEnabled(RuleId::ConditionWaitWithoutPredicate))
                append(ConditionWaitChecker().run(facts));

            if (options_.isEnabled(RuleId::WeakPublicationOrdering))
                append(PublicationOrderingChecker().run(facts));

            if (options_.isEnabled(RuleId::ForkAfterThreadCreation) ||
                options_.isEnabled(RuleId::UnreapedChildProcess) ||
                options_.isEnabled(RuleId::ThreadArgumentEscapesFrame) ||
                options_.isEnabled(RuleId::ThreadArgumentFreedEarly) ||
                options_.isEnabled(RuleId::UnsafeSignalHandler))
            {
                DiagnosticReport processReport = ProcessLifecycleChecker().run(facts);
                std::erase_if(processReport.diagnostics, [this](const Diagnostic& diagnostic)
                              { return !options_.isEnabled(diagnostic.ruleId); });
                append(processReport);
            }

            finalizeReport(moduleReport, facts, selection.accesses);

            for (Diagnostic& diagnostic : moduleReport.diagnostics)
            {
                if (reportedDiagnostics.insert(diagnosticKey(diagnostic)).second)
                    analysis.report.diagnostics.push_back(std::move(diagnostic));
            }

            analysis.report.functions.insert(analysis.report.functions.end(),
                                             moduleReport.functions.begin(),
                                             moduleReport.functions.end());
        }

        analysis.report.diagnosticsSummary = computeSummary(analysis.report.diagnostics);
        return analysis;
    }
} // namespace ctrace::concurrency::internal::analysis::cross_tu
