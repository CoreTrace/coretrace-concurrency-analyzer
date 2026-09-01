// SPDX-License-Identifier: Apache-2.0
#include "inter_tu_coordinator.hpp"

#include "internal/analysis/condition_wait_checker.hpp"
#include "internal/analysis/data_race_checker.hpp"
#include "internal/analysis/facts.hpp"
#include "internal/analysis/ir_utils.hpp"
#include "internal/analysis/lock_order_analyzer.hpp"
#include "internal/analysis/missing_join_detector.hpp"
#include "internal/analysis/report_builder.hpp"
#include "internal/analysis/tu_facts_builder.hpp"
#include "program_symbol_index.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ThreadPool.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace ctrace::concurrency::internal::analysis::cross_tu
{
    namespace
    {
        /// Identity of the target ABI. Two modules may only exchange byte offsets and lock
        /// identities when both were compiled against the same one.
        std::string abiKey(const llvm::Module& module)
        {
            return module.getTargetTriple() + '|' + module.getDataLayoutStr();
        }

        /// Stable identity of a diagnostic, used to drop the copies produced when several units
        /// include the same header and analyse the same inline body.
        std::string diagnosticKey(const Diagnostic& diagnostic)
        {
            return diagnostic.id + '|' + diagnostic.location.file + ':' +
                   std::to_string(diagnostic.location.line) + ':' +
                   std::to_string(diagnostic.location.column) + '|' + diagnostic.message;
        }

        /// True when the program-wide view contradicts what this unit concluded alone. Only these
        /// units are worth analysing a second time.
        bool crossTUChangesFacts(const llvm::Module& module, const TUFacts& facts,
                                 const ProgramSymbolIndex& index)
        {
            // An `extern` this unit dropped as unresolved turns out to name real storage.
            for (const llvm::GlobalVariable& global : module.globals())
            {
                if (global.isDeclaration() && index.isDefinedSomewhere(global))
                    return true;
            }

            // A handle this unit creates is joined by another.
            for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
            {
                if (fact.action != ThreadLifecycleAction::Create)
                    continue;

                const llvm::GlobalVariable* handle =
                    globalOfStorageGroupId(module, fact.handleGroupId);
                if (handle != nullptr && index.resolvesHandleElsewhere(*handle))
                    return true;
            }

            // A helper this unit only sees declared turns out to take a lock for its caller.
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration() && index.lockSummaryFor(function) != nullptr)
                    return true;
            }

            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration() || !index.isThreadEntry(function))
                    continue;

                const auto it = facts.entryConcurrency.find(functionId(function));
                if (it == facts.entryConcurrency.end())
                    return true;

                if (index.spawnCount(function) > it->second.staticSpawnCount ||
                    (index.spawnedInLoop(function) && !it->second.hasSpawnInLoop))
                {
                    return true;
                }
            }

            return false;
        }
    } // namespace

    InterTUCoordinator::InterTUCoordinator(AnalysisOptions options) : options_(std::move(options))
    {
    }

    ProjectAnalysis
    InterTUCoordinator::analyze(const std::vector<const llvm::Module*>& modules) const
    {
        ProjectAnalysis analysis;

        std::vector<const llvm::Module*> present;
        for (const llvm::Module* module : modules)
        {
            if (module != nullptr)
                present.push_back(module);
        }

        if (present.empty())
            return analysis;

        // Keep the largest ABI group: a project normally has one, and a stray module compiled for
        // another target is a configuration mistake worth naming rather than silently mixing in.
        std::map<std::string, std::size_t> moduleCountByAbi;
        for (const llvm::Module* module : present)
            ++moduleCountByAbi[abiKey(*module)];

        const auto dominantAbi = std::max_element(moduleCountByAbi.begin(), moduleCountByAbi.end(),
                                                  [](const auto& lhs, const auto& rhs)
                                                  { return lhs.second < rhs.second; });

        std::vector<const llvm::Module*> compatible;
        for (const llvm::Module* module : present)
        {
            if (abiKey(*module) == dominantAbi->first)
                compatible.push_back(module);
            else
                analysis.skippedIncompatibleModules.push_back(module->getModuleIdentifier());
        }

        // Pass A: every unit on its own, but already in project mode. A spawn naming a worker
        // defined elsewhere has to be recorded here, or the index built from these facts would
        // never learn that the worker is a thread entry at all. The index is empty, so nothing
        // is seeded back yet.
        const TUFactsBuilder factsBuilder;
        const ProgramSymbolIndex nothingKnownYet;
        std::vector<TUFacts> factsByModule(compatible.size());
        {
            llvm::DefaultThreadPool pool;
            for (std::size_t index = 0; index < compatible.size(); ++index)
            {
                pool.async(
                    [&, index]
                    {
                        factsByModule[index] =
                            factsBuilder.build(*compatible[index], &nothingKnownYet);
                    });
            }
            pool.wait();
        }

        ProgramSymbolIndex programIndex;
        for (std::size_t index = 0; index < compatible.size(); ++index)
            programIndex.addModule(*compatible[index], factsByModule[index]);

        // Pass B: only the units whose conclusions the program-wide view changes.
        {
            llvm::DefaultThreadPool pool;
            for (std::size_t index = 0; index < compatible.size(); ++index)
            {
                if (!crossTUChangesFacts(*compatible[index], factsByModule[index], programIndex))
                    continue;

                ++analysis.reanalyzedUnitCount;

                pool.async(
                    [&, index]
                    {
                        factsByModule[index] =
                            factsBuilder.build(*compatible[index], &programIndex);
                    });
            }
            pool.wait();
        }

        std::set<std::string> reportedDiagnostics;
        for (std::size_t index = 0; index < compatible.size(); ++index)
        {
            const llvm::Module& module = *compatible[index];
            const TUFacts& facts = factsByModule[index];

            DiagnosticReport moduleReport;
            const auto append = [&moduleReport](const DiagnosticReport& partial)
            {
                moduleReport.diagnostics.insert(moduleReport.diagnostics.end(),
                                                partial.diagnostics.begin(),
                                                partial.diagnostics.end());
            };

            if (options_.isEnabled(RuleId::DataRaceGlobal))
                append(DataRaceChecker().run(module, facts));

            if (options_.isEnabled(RuleId::DeadlockLockOrder))
                append(LockOrderAnalyzer().run(facts));

            if (options_.isEnabled(RuleId::MissingJoin))
                append(MissingJoinDetector().run(facts));

            if (options_.isEnabled(RuleId::ConditionWaitWithoutPredicate))
                append(ConditionWaitChecker().run(facts));

            finalizeReport(moduleReport, facts);

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
