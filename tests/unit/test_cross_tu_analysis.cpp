// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"
#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
    using ctrace::concurrency::AnalysisOptions;
    using ctrace::concurrency::CompileError;
    using ctrace::concurrency::CompileRequest;
    using ctrace::concurrency::CompileResult;
    using ctrace::concurrency::Diagnostic;
    using ctrace::concurrency::DiagnosticReport;
    using ctrace::concurrency::FunctionSummary;
    using ctrace::concurrency::InMemoryIRCompiler;
    using ctrace::concurrency::IRFormat;
    using ctrace::concurrency::LoadedUnit;
    using ctrace::concurrency::ProjectAnalysisReport;
    using ctrace::concurrency::ProjectConcurrencyAnalyzer;
    using ctrace::concurrency::ProjectUnitSource;
    using ctrace::concurrency::RuleId;
    using ctrace::concurrency::SingleTUConcurrencyAnalyzer;

    constexpr std::string_view kProjectRoot = "tests/fixtures/concurrency-project";

    std::filesystem::path projectFixturePath(std::string_view relativePath)
    {
        return std::filesystem::path(CORETRACE_PROJECT_SOURCE_DIR) / kProjectRoot / relativePath;
    }

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    /// Compiles the units of a project into the bitcode source the project analysis reads, and
    /// keeps the compiled modules alongside for the single-unit comparisons.
    class CompiledProject
    {
      public:
        [[nodiscard]] bool add(std::string_view relativePath,
                               std::vector<std::string> compileArgs = {})
        {
            CompileRequest request;
            request.inputFile = projectFixturePath(relativePath).string();
            request.extraCompileArgs = std::move(compileArgs);
            request.format = IRFormat::BC;

            CompileResult result = InMemoryIRCompiler().compile(request, context_);
            if (!result.success || result.module == nullptr)
            {
                std::cerr << "[FAIL] project fixture compile failed for " << relativePath << "\n";
                return false;
            }

            // parseBC keeps a non-owning view over the bitcode buffer, so the copy the module
            // reads stays alive here; the source gets its own copy to parse on demand.
            units_.add(request.inputFile, result.llvmBitcode);
            bitcodes_.push_back(std::move(result.llvmBitcode));
            modules_.push_back(std::move(result.module));
            return true;
        }

        [[nodiscard]] const ProjectUnitSource& units() const noexcept
        {
            return units_;
        }

        [[nodiscard]] const llvm::Module& moduleAt(std::size_t index) const
        {
            return *modules_.at(index);
        }

        [[nodiscard]] const std::string& bitcodeAt(std::size_t index) const
        {
            return bitcodes_.at(index);
        }

      private:
        llvm::LLVMContext context_;
        ProjectUnitSource units_;
        std::vector<std::string> bitcodes_;
        std::vector<std::unique_ptr<llvm::Module>> modules_;
    };

    std::optional<std::string> symbolOf(const Diagnostic& diagnostic)
    {
        const auto it = diagnostic.properties.find("symbol");
        if (it == diagnostic.properties.end())
            return std::nullopt;

        if (const auto* value = std::get_if<std::string>(&it->second))
            return *value;

        return std::nullopt;
    }

    std::size_t countRacesOn(const DiagnosticReport& report, std::string_view symbol)
    {
        std::size_t count = 0;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.ruleId != RuleId::DataRaceGlobal)
                continue;

            const std::optional<std::string> reported = symbolOf(diagnostic);
            if (reported.has_value() && *reported == symbol)
                ++count;
        }

        return count;
    }

    /// The acceptance case for the project mode: the spawn lives in `main.c`, the racing body in
    /// `worker.c`, and neither unit holds both halves of the proof.
    bool testSpawnInOneUnitRacesWithBodyInAnother()
    {
        CompiledProject project;
        if (!project.add("cross-tu-data-race/main.c") ||
            !project.add("cross-tu-data-race/worker.c"))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(countRacesOn(analysis.report, "shared_counter") > 0,
                          "cross-TU project should report the race on shared_counter") &&
               assertTrue(analysis.skippedIncompatibleModules.empty(),
                          "units of one project share an ABI and none should be skipped");
    }

    /// The same worker unit alone must stay silent. Without this, the test above would pass even
    /// if the cross-unit reasoning did nothing.
    bool testWorkerUnitAloneReportsNothing()
    {
        CompiledProject project;
        if (!project.add("cross-tu-data-race/worker.c"))
            return false;

        const DiagnosticReport report = SingleTUConcurrencyAnalyzer().analyze(project.moduleAt(0));

        return assertTrue(countRacesOn(report, "shared_counter") == 0,
                          "worker.c alone proves no concurrency and must report no race");
    }

    /// Order of the units is a property of the build system, not of the program, so it must not
    /// change what is reported.
    bool testUnitOrderDoesNotChangeTheReport()
    {
        CompiledProject forward;
        CompiledProject reversed;
        if (!forward.add("cross-tu-data-race/main.c") ||
            !forward.add("cross-tu-data-race/worker.c") ||
            !reversed.add("cross-tu-data-race/worker.c") ||
            !reversed.add("cross-tu-data-race/main.c"))
        {
            return false;
        }

        const ProjectAnalysisReport first = ProjectConcurrencyAnalyzer().analyze(forward.units());
        const ProjectAnalysisReport second = ProjectConcurrencyAnalyzer().analyze(reversed.units());

        return assertTrue(countRacesOn(first.report, "shared_counter") ==
                              countRacesOn(second.report, "shared_counter"),
                          "reversing the unit order must not change the reported races");
    }

    /// A project is not a heap of files: passing the same unit twice describes one program, and
    /// the same conflict must not be reported twice.
    bool testRepeatedUnitIsNotReportedTwice()
    {
        CompiledProject project;
        if (!project.add("cross-tu-data-race/main.c") ||
            !project.add("cross-tu-data-race/worker.c"))
        {
            return false;
        }

        const ProjectAnalysisReport once = ProjectConcurrencyAnalyzer().analyze(project.units());

        ProjectUnitSource repeated = project.units();
        repeated.add(repeated.identifier(1), project.bitcodeAt(1));
        const ProjectAnalysisReport twice = ProjectConcurrencyAnalyzer().analyze(repeated);

        return assertTrue(countRacesOn(once.report, "shared_counter") ==
                              countRacesOn(twice.report, "shared_counter"),
                          "a unit listed twice must not duplicate its diagnostics");
    }

    /// The second half of the boundary: the conflict is entirely inside one unit, but the object
    /// it is about is defined in another. Without the whole-program view, `extern int` and an
    /// unresolved symbol are indistinguishable.
    bool testExternGlobalDefinedInAnotherUnitIsTracked()
    {
        CompiledProject project;
        if (!project.add("cross-tu-extern-global/app.c") ||
            !project.add("cross-tu-extern-global/state.c"))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(countRacesOn(analysis.report, "g_shared_state") > 0,
                          "a global defined in another unit must still be tracked");
    }

    /// A second pass is owed to a unit only when the program-wide view can change a fact a
    /// selected rule reads. The mutable global defined in state.c changes what app.c's accesses
    /// mean, which matters to a rule reading accesses and to no other (#52).
    bool testExternGlobalReanalysisFollowsTheSelection()
    {
        CompiledProject project;
        if (!project.add("cross-tu-extern-global/app.c") ||
            !project.add("cross-tu-extern-global/state.c"))
        {
            return false;
        }

        const ProjectAnalysisReport races =
            ProjectConcurrencyAnalyzer(AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}})
                .analyze(project.units());
        const ProjectAnalysisReport waits =
            ProjectConcurrencyAnalyzer(
                AnalysisOptions{.enabledRules = {RuleId::ConditionWaitWithoutPredicate}})
                .analyze(project.units());

        return assertTrue(races.reanalyzedUnitCount == 1,
                          "a rule reading accesses reanalyses the unit that declares the global") &&
               assertTrue(countRacesOn(races.report, "g_shared_state") > 0,
                          "and reports the race the second pass makes visible") &&
               assertTrue(waits.reanalyzedUnitCount == 0,
                          "a rule reading no access reanalyses nothing for that global");
    }

    /// A vtable and type information declared here and defined in another unit are constant
    /// data no thread writes. The program-wide view changes nothing about this unit, so it
    /// must not be analysed a second time (#52).
    bool testConstantDeclarationsTriggerNoSecondPass()
    {
        CompiledProject project;
        const std::vector<std::string> args = {"-std=c++20"};
        if (!project.add("cross-tu-vtable-declaration/widget.cpp", args) ||
            !project.add("cross-tu-vtable-declaration/main.cpp", args))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());
        return assertTrue(analysis.complete(), "both units are analysed") &&
               assertTrue(analysis.reanalyzedUnitCount == 0,
                          "declaring a vtable and type information defined elsewhere triggers no "
                          "second pass");
    }

    /// The same application unit alone must stay silent, or the test above would prove nothing.
    bool testUnresolvedExternIsNotTracked()
    {
        CompiledProject project;
        if (!project.add("cross-tu-extern-global/app.c"))
            return false;

        const DiagnosticReport report = SingleTUConcurrencyAnalyzer().analyze(project.moduleAt(0));

        return assertTrue(countRacesOn(report, "g_shared_state") == 0,
                          "an extern with no visible definition must not be reported");
    }

    std::size_t countDeadlocks(const DiagnosticReport& report)
    {
        std::size_t count = 0;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.ruleId == RuleId::DeadlockLockOrder)
                ++count;
        }

        return count;
    }

    /// The third thing that crosses the boundary: what a helper does to the lock it is handed.
    /// `workers.c` holds the inversion and never names a lock primitive; `sync.c` holds the
    /// helpers and no ordering of its own.
    bool testLockWrapperDefinedInAnotherUnitClosesTheCycle()
    {
        CompiledProject project;
        if (!project.add("cross-tu-lock-wrapper/workers.c") ||
            !project.add("cross-tu-lock-wrapper/sync.c"))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(countDeadlocks(analysis.report) > 0,
                          "an inversion through helpers defined elsewhere must be reported");
    }

    /// The same for a helper defined in another unit: what it does to the lock it is handed
    /// changes the lock state workers.c is judged under, which matters to a rule reading lock
    /// state and to no other (#52).
    bool testLockSummaryReanalysisFollowsTheSelection()
    {
        CompiledProject project;
        if (!project.add("cross-tu-lock-wrapper/workers.c") ||
            !project.add("cross-tu-lock-wrapper/sync.c"))
        {
            return false;
        }

        const ProjectAnalysisReport cycles =
            ProjectConcurrencyAnalyzer(AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}})
                .analyze(project.units());
        const ProjectAnalysisReport waits =
            ProjectConcurrencyAnalyzer(
                AnalysisOptions{.enabledRules = {RuleId::ConditionWaitWithoutPredicate}})
                .analyze(project.units());

        return assertTrue(cycles.reanalyzedUnitCount == 1,
                          "a rule reading lock state reanalyses the unit calling the helpers") &&
               assertTrue(countDeadlocks(cycles.report) > 0,
                          "and reports the cycle the second pass makes visible") &&
               assertTrue(waits.reanalyzedUnitCount == 0,
                          "a rule reading no lock state reanalyses nothing for those helpers");
    }

    /// The worker unit alone cannot see what the helpers do, and must not guess.
    bool testWorkerUnitWithoutTheHelpersReportsNoCycle()
    {
        CompiledProject project;
        if (!project.add("cross-tu-lock-wrapper/workers.c"))
            return false;

        const DiagnosticReport report = SingleTUConcurrencyAnalyzer().analyze(project.moduleAt(0));

        return assertTrue(countDeadlocks(report) == 0,
                          "opaque helpers must not produce a lock-order finding");
    }

    std::size_t countMissingJoins(const DiagnosticReport& report)
    {
        std::size_t count = 0;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.ruleId == RuleId::MissingJoin)
                ++count;
        }

        return count;
    }

    /// A thread started in one unit and joined in another is not leaked. Only the program sees
    /// both halves, so this is a finding the project mode must remove rather than add.
    bool testHandleJoinedInAnotherUnitIsNotOutstanding()
    {
        CompiledProject project;
        if (!project.add("cross-tu-handle-lifecycle/start.c") ||
            !project.add("cross-tu-handle-lifecycle/stop.c") ||
            !project.add("cross-tu-handle-lifecycle/main.c"))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(countMissingJoins(analysis.report) == 0,
                          "a handle joined in another unit must not be reported as outstanding");
    }

    /// The creating unit alone must still report it: it genuinely cannot know.
    bool testCreatingUnitAloneStillReportsTheHandle()
    {
        CompiledProject project;
        if (!project.add("cross-tu-handle-lifecycle/start.c"))
            return false;

        const DiagnosticReport report = SingleTUConcurrencyAnalyzer().analyze(project.moduleAt(0));

        return assertTrue(countMissingJoins(report) == 1,
                          "the creating unit alone has no evidence the handle is ever joined");
    }

    /// Existing single-unit fixtures, analysed as one-unit projects. The project mode adds
    /// knowledge; it must never subtract any. This is what catches a cross-unit change quietly
    /// losing a finding the single-unit path still makes.
    bool testProjectModeKeepsEverySingleUnitFinding()
    {
        constexpr std::string_view kFixtures[] = {
            "concurrency/data-race/data_race_basic.c",
            "concurrency/data-race/data_race_mutex_protected.c",
            "concurrency/deadlock/deadlock_basic.c",
            "concurrency/deadlock/deadlock_consistent_order_no_diagnostic.c",
            "concurrency/missing-join/missing_join_basic.c",
            "concurrency/missing-join/pthread_join_all_no_missing_join.c",
        };

        bool ok = true;
        for (const std::string_view fixture : kFixtures)
        {
            llvm::LLVMContext context;
            CompileRequest request;
            request.inputFile =
                (std::filesystem::path(CORETRACE_PROJECT_SOURCE_DIR) / "tests/fixtures" / fixture)
                    .string();
            request.format = IRFormat::BC;

            CompileResult compiled = InMemoryIRCompiler().compile(request, context);
            if (!assertTrue(compiled.success && compiled.module != nullptr,
                            std::string("fixture compiles: ") + std::string(fixture)))
            {
                ok = false;
                continue;
            }

            const DiagnosticReport alone = SingleTUConcurrencyAnalyzer().analyze(*compiled.module);
            ProjectUnitSource single;
            single.add(request.inputFile, compiled.llvmBitcode);
            const ProjectAnalysisReport asProject = ProjectConcurrencyAnalyzer().analyze(single);

            ok = assertTrue(asProject.report.diagnostics.size() >= alone.diagnostics.size(),
                            std::string("project mode keeps every finding of ") +
                                std::string(fixture)) &&
                 ok;

            // A fixture with no diagnostic alone must stay clean: that is where a new false
            // positive would show up.
            if (alone.diagnostics.empty())
            {
                ok = assertTrue(asProject.report.diagnostics.empty(),
                                std::string("project mode adds no finding to ") +
                                    std::string(fixture)) &&
                     ok;
            }
        }

        return ok;
    }

    std::size_t countRule(const DiagnosticReport& report, RuleId rule)
    {
        std::size_t count = 0;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.ruleId == rule)
                ++count;
        }

        return count;
    }

    std::vector<std::string> idiomaticServiceArgs()
    {
        return {"-std=c++20", "-I" + projectFixturePath("idiomatic-cxx-service").string()};
    }

    /// A service written the way C++ services are written: synchronisation behind hand-rolled
    /// wrappers, shared state held as members, a thread started from a pointer-to-member, and a
    /// fork whose children keep running the same program.
    ///
    /// The fork is judged against the whole program: the unit that forks starts no thread of its
    /// own, and only the unit next to it does.
    bool testIdiomaticServiceReportsWhatCrossesTheProgram()
    {
        CompiledProject project;
        const std::vector<std::string> args = idiomaticServiceArgs();
        if (!project.add("idiomatic-cxx-service/worker.cpp", args) ||
            !project.add("idiomatic-cxx-service/supervisor.cpp", args) ||
            !project.add("idiomatic-cxx-service/main.cpp", args))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(countRule(analysis.report, RuleId::ForkAfterThreadCreation) == 1,
                          "the fork is unsafe because another unit starts a thread") &&
               assertTrue(countRule(analysis.report, RuleId::UnreapedChildProcess) == 1,
                          "no unit of the program collects the children");
    }

    /// The forking unit alone starts no thread, so on its own it has no reason to object.
    /// Without this, the test above would pass even if the fork rule ignored the program.
    bool testForkingUnitAloneDoesNotSeeTheThreads()
    {
        CompiledProject project;
        if (!project.add("idiomatic-cxx-service/supervisor.cpp", idiomaticServiceArgs()))
            return false;

        const DiagnosticReport report = SingleTUConcurrencyAnalyzer().analyze(project.moduleAt(0));

        return assertTrue(countRule(report, RuleId::ForkAfterThreadCreation) == 0,
                          "a unit that starts no thread cannot call its own fork unsafe") &&
               assertTrue(countRule(report, RuleId::UnreapedChildProcess) == 1,
                          "the unreaped child is visible from the forking unit alone");
    }

    /// A child forked in one unit and reaped by a helper in another is collected. Reaping is
    /// judged against the whole program, like the threads a fork is judged against (#89).
    bool testChildReapedInAnotherUnitIsCollected()
    {
        CompiledProject project;
        if (!project.add("cross-tu-reaped-elsewhere/main.c") ||
            !project.add("cross-tu-reaped-elsewhere/reaper.c"))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(countRule(analysis.report, RuleId::UnreapedChildProcess) == 0,
                          "a child reaped by a helper in another unit is not left a zombie");
    }

    /// The forking unit alone must still report the child: it genuinely cannot know. Without
    /// this, the test above would pass even if the rule never looked at the fork.
    bool testForkingUnitAloneStillReportsTheChild()
    {
        CompiledProject project;
        if (!project.add("cross-tu-reaped-elsewhere/main.c"))
            return false;

        const DiagnosticReport report = SingleTUConcurrencyAnalyzer().analyze(project.moduleAt(0));

        return assertTrue(countRule(report, RuleId::UnreapedChildProcess) == 1,
                          "the forking unit alone has no evidence the child is ever collected");
    }

    /// The reaping elsewhere changes what the forking unit concludes for unreaped-child, and
    /// for no other rule: fork-after-thread asks about threads, not about waits (#89).
    bool testReapingReanalysisFollowsTheSelection()
    {
        CompiledProject project;
        if (!project.add("cross-tu-reaped-elsewhere/main.c") ||
            !project.add("cross-tu-reaped-elsewhere/reaper.c"))
        {
            return false;
        }

        const ProjectAnalysisReport reaping =
            ProjectConcurrencyAnalyzer(
                AnalysisOptions{.enabledRules = {RuleId::UnreapedChildProcess}})
                .analyze(project.units());
        const ProjectAnalysisReport forking =
            ProjectConcurrencyAnalyzer(
                AnalysisOptions{.enabledRules = {RuleId::ForkAfterThreadCreation}})
                .analyze(project.units());

        return assertTrue(reaping.reanalyzedUnitCount == 1,
                          "unreaped-child reanalyses the unit that forks without reaping") &&
               assertTrue(countRule(reaping.report, RuleId::UnreapedChildProcess) == 0,
                          "and the second pass sees the child collected") &&
               assertTrue(forking.reanalyzedUnitCount == 0,
                          "fork-after-thread reanalyses nothing for a wait in another unit");
    }

    /// Pins the frontier rather than a finding: two real defects in this fixture are invisible,
    /// and both need work that does not exist yet. When either becomes visible this test fails,
    /// which is the point — it is how the analyzer announces that it grew.
    bool testIdiomaticServiceKeepsItsKnownBlindSpots()
    {
        CompiledProject project;
        const std::vector<std::string> args = idiomaticServiceArgs();
        if (!project.add("idiomatic-cxx-service/worker.cpp", args) ||
            !project.add("idiomatic-cxx-service/supervisor.cpp", args) ||
            !project.add("idiomatic-cxx-service/main.cpp", args))
        {
            return false;
        }

        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        // `Worker::stop` writes `_running` while `Worker::loop` reads it, neither under a lock.
        // Seeing it needs an identity for a field reached through `this`, and a thread entry
        // resolved through the pointer-to-member `std::thread` was handed.
        const bool sharedMemberRaceInvisible =
            countRule(analysis.report, RuleId::DataRaceGlobal) == 0;

        // `Worker::loop` wraps the bare wait in `while (_running)`, so the rule considers the
        // recheck done. That the loop tests something unrelated to the condition waited on is
        // beyond what it models.
        const bool loopIsTakenAtFaceValue =
            countRule(analysis.report, RuleId::ConditionWaitWithoutPredicate) == 0;

        return assertTrue(sharedMemberRaceInvisible,
                          "a race on a member reached through this is still invisible") &&
               assertTrue(loopIsTakenAtFaceValue,
                          "a loop around a bare wait is still taken at face value");
    }

    /// Everything a diagnostic says, in a form two reports can be compared by. With a rule,
    /// only that rule's diagnostics, so a narrow run can be compared against a full one.
    /// What a diagnostic reports, without its id: the id numbers the diagnostics of one report,
    /// so it moves when another rule's findings are added or removed.
    std::string describeFinding(const Diagnostic& diagnostic)
    {
        return diagnostic.location.file + ':' + std::to_string(diagnostic.location.line) + ':' +
               std::to_string(diagnostic.location.column) + '|' + diagnostic.message + '|' +
               symbolOf(diagnostic).value_or("");
    }

    std::vector<std::string> diagnosticLines(const DiagnosticReport& report,
                                             std::optional<RuleId> onlyRule = std::nullopt)
    {
        std::vector<std::string> lines;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (onlyRule.has_value() && diagnostic.ruleId != *onlyRule)
                continue;

            lines.push_back(diagnostic.id + '|' + describeFinding(diagnostic));
        }
        std::sort(lines.begin(), lines.end());
        return lines;
    }

    /// A rule's findings in a report, comparable between reports made with different rules.
    std::vector<std::string> ruleFindings(const DiagnosticReport& report, RuleId rule)
    {
        std::vector<std::string> findings;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.ruleId == rule)
                findings.push_back(describeFinding(diagnostic));
        }
        std::sort(findings.begin(), findings.end());
        return findings;
    }

    /// The functions a report summarises, comparable between reports.
    std::vector<std::string> summarisedFunctions(const DiagnosticReport& report)
    {
        std::vector<std::string> functions;
        for (const FunctionSummary& function : report.functions)
            functions.push_back(function.file + '|' + function.name);
        std::sort(functions.begin(), functions.end());
        return functions;
    }

    struct ProjectFixture
    {
        std::vector<std::string_view> units;
        std::vector<std::string> compileArgs;
    };

    /// Every project fixture, each exercising what one kind of cross-unit fact changes.
    std::vector<ProjectFixture> everyProjectFixture()
    {
        const std::vector<std::string> cxx = {"-std=c++20"};
        return {
            {.units = {"cross-tu-data-race/main.c", "cross-tu-data-race/worker.c"}},
            {.units = {"cross-tu-extern-global/app.c", "cross-tu-extern-global/state.c"}},
            {.units = {"cross-tu-handle-lifecycle/start.c", "cross-tu-handle-lifecycle/stop.c",
                       "cross-tu-handle-lifecycle/main.c"}},
            {.units = {"cross-tu-lock-wrapper/workers.c", "cross-tu-lock-wrapper/sync.c"}},
            {.units = {"cross-tu-reaped-elsewhere/main.c", "cross-tu-reaped-elsewhere/reaper.c"}},
            {.units = {"cross-tu-vtable-declaration/widget.cpp",
                       "cross-tu-vtable-declaration/main.cpp"},
             .compileArgs = cxx},
            {.units = {"idiomatic-cxx-service/worker.cpp", "idiomatic-cxx-service/supervisor.cpp",
                       "idiomatic-cxx-service/main.cpp"},
             .compileArgs = idiomaticServiceArgs()},
        };
    }

    bool compileFixture(const ProjectFixture& fixture, CompiledProject& project)
    {
        for (const std::string_view unit : fixture.units)
        {
            if (!project.add(unit, fixture.compileArgs))
                return false;
        }
        return true;
    }

    /// The rules no fact from another unit can change: their project conclusions are the
    /// single-unit ones, so a project analysis owes them no program-wide work (#52).
    constexpr RuleId kUnitLocalRules[] = {
        RuleId::ConditionWaitWithoutPredicate, RuleId::ThreadArgumentEscapesFrame,
        RuleId::UnsafeSignalHandler,           RuleId::ThreadArgumentFreedEarly,
        RuleId::ThreadLocalOutlivesThread,
    };

    /// The rules that read which thread entries reach each function; every other rule leaves
    /// the function summaries to what it computed, as a single-unit run does.
    constexpr RuleId kEntryConcurrencyRules[] = {
        RuleId::DataRaceGlobal,
        RuleId::DeadlockLockOrder,
        RuleId::WeakPublicationOrdering,
    };

    /// Four units with findings that cross the program: enough to exercise a bound below the
    /// unit count, and a mix of rules.
    bool addFourUnitProject(CompiledProject& project)
    {
        return project.add("cross-tu-data-race/main.c") &&
               project.add("cross-tu-data-race/worker.c") &&
               project.add("cross-tu-lock-wrapper/sync.c") &&
               project.add("cross-tu-lock-wrapper/workers.c");
    }

    /// A unit that does not parse is named, its error kept, and the rest of the project is
    /// still analysed: the report says what it covers instead of failing as a whole.
    bool testUnreadableUnitIsReportedAndTheRestAnalysed()
    {
        CompiledProject project;
        if (!project.add("cross-tu-data-race/main.c") ||
            !project.add("cross-tu-data-race/worker.c"))
        {
            return false;
        }

        ProjectUnitSource units = project.units();
        const std::string& worker = project.bitcodeAt(1);
        units.add("truncated-worker.c", worker.substr(0, worker.size() / 2));
        units.add("not-bitcode.c", "this is not bitcode");

        const ProjectAnalysisReport analysis = ProjectConcurrencyAnalyzer().analyze(units);

        return assertTrue(analysis.failedUnits.size() == 2, "both unreadable units are reported") &&
               assertTrue(analysis.failedUnits.size() == 2 &&
                              analysis.failedUnits[0].identifier == "truncated-worker.c" &&
                              analysis.failedUnits[1].identifier == "not-bitcode.c",
                          "failed units are named by identifier, in unit order") &&
               assertTrue(analysis.failedUnits.size() == 2 &&
                              analysis.failedUnits[0].error.hasError() &&
                              analysis.failedUnits[1].error.hasError(),
                          "each failed unit carries the load error") &&
               assertTrue(!analysis.complete(), "a report missing a unit is not complete") &&
               assertTrue(countRacesOn(analysis.report, "shared_counter") > 0,
                          "the readable units are still analysed together");
    }

    /// Loading the same bytes twice yields the same program: same source file, same functions,
    /// same identities for local symbols. This is what lets the second pass trust the first.
    bool testReloadingAUnitYieldsTheSameProgram()
    {
        CompiledProject project;
        if (!project.add("cross-tu-lock-wrapper/sync.c"))
            return false;

        const auto identities = [](const LoadedUnit& unit)
        {
            std::vector<std::string> names;
            for (const llvm::Function& function : *unit.module)
                names.push_back(function.getGlobalIdentifier());
            std::sort(names.begin(), names.end());
            return names;
        };

        CompileError firstError;
        CompileError secondError;
        const LoadedUnit first = project.units().load(0, firstError);
        const LoadedUnit second = project.units().load(0, secondError);
        if (!assertTrue(first.ok() && second.ok(), "the unit loads twice"))
            return false;

        const ProjectAnalysisReport once = ProjectConcurrencyAnalyzer().analyze(project.units());
        const ProjectAnalysisReport again = ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(first.module->getSourceFileName() == second.module->getSourceFileName(),
                          "the source file name survives a reload") &&
               assertTrue(first.module->getModuleIdentifier() == project.units().identifier(0),
                          "the unit identifier becomes the module identifier") &&
               assertTrue(identities(first) == identities(second),
                          "every function keeps its program identity across a reload") &&
               assertTrue(first.context.get() != second.context.get(),
                          "each load owns a context of its own") &&
               assertTrue(diagnosticLines(once.report) == diagnosticLines(again.report),
                          "analysing the same source twice yields the same report");
    }

    /// The bound is a ceiling on units in memory, observed rather than promised, and it does
    /// not change what the analysis finds.
    bool testLiveUnitBoundIsRespected()
    {
        CompiledProject project;
        if (!addFourUnitProject(project))
            return false;

        AnalysisOptions one;
        one.maxLiveUnits = 1;
        AnalysisOptions two;
        two.maxLiveUnits = 2;

        const ProjectAnalysisReport serial =
            ProjectConcurrencyAnalyzer(one).analyze(project.units());
        const ProjectAnalysisReport paired =
            ProjectConcurrencyAnalyzer(two).analyze(project.units());
        const ProjectAnalysisReport unbounded =
            ProjectConcurrencyAnalyzer().analyze(project.units());

        return assertTrue(serial.peakLiveUnits == 1, "a bound of one keeps one unit live") &&
               assertTrue(paired.peakLiveUnits >= 1 && paired.peakLiveUnits <= 2,
                          "a bound of two keeps at most two units live") &&
               assertTrue(unbounded.peakLiveUnits >= 1 && unbounded.peakLiveUnits <= 4,
                          "the default never exceeds the unit count") &&
               assertTrue(!serial.report.diagnostics.empty(), "the bounded run still reports") &&
               assertTrue(diagnosticLines(serial.report) == diagnosticLines(unbounded.report),
                          "the bound does not change the diagnostics");
    }

    /// A unit compiled for another target is set aside by name, whichever order it arrives
    /// in, and the rest of the project is analysed as before.
    bool testUnitWithAnotherAbiIsSkippedByName()
    {
        CompiledProject project;
        if (!project.add("cross-tu-data-race/main.c") ||
            !project.add("cross-tu-data-race/worker.c"))
        {
            return false;
        }

        // The same worker, re-targeted: valid bitcode whose ABI key differs from the rest.
        std::string foreignBitcode;
        {
            CompileError error;
            const LoadedUnit worker = project.units().load(1, error);
            if (!assertTrue(worker.ok(), "the worker loads"))
                return false;
            worker.module->setTargetTriple("mips64-unknown-linux-gnuabi64");
            llvm::raw_string_ostream stream(foreignBitcode);
            llvm::WriteBitcodeToFile(*worker.module, stream);
        }

        ProjectUnitSource last = project.units();
        last.add("foreign-worker.c", foreignBitcode);
        ProjectUnitSource first;
        first.add("foreign-worker.c", foreignBitcode);
        first.add(project.units().identifier(0), project.bitcodeAt(0));
        first.add(project.units().identifier(1), project.bitcodeAt(1));

        const ProjectAnalysisReport lastReport = ProjectConcurrencyAnalyzer().analyze(last);
        const ProjectAnalysisReport firstReport = ProjectConcurrencyAnalyzer().analyze(first);

        return assertTrue(lastReport.skippedIncompatibleModules ==
                              std::vector<std::string>{"foreign-worker.c"},
                          "the foreign unit is skipped and named") &&
               assertTrue(firstReport.skippedIncompatibleModules ==
                              lastReport.skippedIncompatibleModules,
                          "the choice does not depend on unit order") &&
               assertTrue(lastReport.complete() && firstReport.complete(),
                          "a skipped unit is not a failed one") &&
               assertTrue(countRacesOn(lastReport.report, "shared_counter") > 0 &&
                              diagnosticLines(lastReport.report) ==
                                  diagnosticLines(firstReport.report),
                          "the compatible units are analysed together either way");
    }

    /// How many units are analysed at once is a resource choice, not an analysis one.
    bool testConcurrencyDoesNotChangeTheReport()
    {
        CompiledProject project;
        if (!addFourUnitProject(project))
            return false;

        AnalysisOptions one;
        one.maxLiveUnits = 1;
        AnalysisOptions four;
        four.maxLiveUnits = 4;

        const ProjectAnalysisReport serial =
            ProjectConcurrencyAnalyzer(one).analyze(project.units());
        const ProjectAnalysisReport parallel =
            ProjectConcurrencyAnalyzer(four).analyze(project.units());

        return assertTrue(diagnosticLines(serial.report) == diagnosticLines(parallel.report),
                          "one worker and four workers report the same diagnostics") &&
               assertTrue(serial.report.functions.size() == parallel.report.functions.size(),
                          "and the same function summaries") &&
               assertTrue(serial.reanalyzedUnitCount == parallel.reanalyzedUnitCount,
                          "and re-analyse the same units");
    }

    /// Beyond the two rules above: every rule, on every project fixture, reports alone exactly
    /// what it reports among all the rules, whatever facts a narrow run leaves out (#52).
    bool everyRuleAloneMatchesTheFullRun()
    {
        bool ok = true;
        for (const ProjectFixture& fixture : everyProjectFixture())
        {
            CompiledProject project;
            if (!compileFixture(fixture, project))
                return false;

            const ProjectAnalysisReport full =
                ProjectConcurrencyAnalyzer().analyze(project.units());
            for (const RuleId rule : AnalysisOptions::allAvailable().enabledRules)
            {
                const ProjectAnalysisReport narrow =
                    ProjectConcurrencyAnalyzer(AnalysisOptions{.enabledRules = {rule}})
                        .analyze(project.units());
                const std::vector<std::string> findings = ruleFindings(narrow.report, rule);
                if (narrow.complete() && findings == ruleFindings(full.report, rule) &&
                    findings.size() == narrow.report.diagnostics.size())
                {
                    continue;
                }

                std::cerr << "[FAIL] " << fixture.units.front() << ": " << toString(rule)
                          << " alone does not report what the full run reports for it\n";
                ok = false;
            }
        }
        return ok;
    }

    /// A narrow `--rules` selection must reach the same conclusions about the program as a
    /// full run does. What crosses a unit boundary is not a rule's private business: the
    /// spawn in one unit and the worker body in another, or the handle created here and
    /// joined there, are established by the whole-program passes, which run whatever the
    /// rules select. Asserted through the conclusions, so that how the facts are arranged
    /// underneath stays free to change.
    bool testNarrowRuleSelectionKeepsCrossUnitConclusions()
    {
        CompiledProject race;
        CompiledProject lifecycle;
        if (!race.add("cross-tu-data-race/main.c") || !race.add("cross-tu-data-race/worker.c") ||
            !lifecycle.add("cross-tu-handle-lifecycle/start.c") ||
            !lifecycle.add("cross-tu-handle-lifecycle/stop.c") ||
            !lifecycle.add("cross-tu-handle-lifecycle/main.c"))
        {
            return false;
        }

        const AnalysisOptions raceOnly{.enabledRules = {RuleId::DataRaceGlobal}};
        const AnalysisOptions joinOnly{.enabledRules = {RuleId::MissingJoin}};

        // The spawn is in main.c and the racing body in worker.c: neither unit holds both
        // halves, so this race exists only for a reader of the whole program.
        const ProjectAnalysisReport raceNarrow =
            ProjectConcurrencyAnalyzer(raceOnly).analyze(race.units());
        const ProjectAnalysisReport raceFull = ProjectConcurrencyAnalyzer().analyze(race.units());

        // The handle is created in start.c and joined in stop.c: only the program resolves it,
        // and a unit analysis that lost that would report a join that is right there.
        const ProjectAnalysisReport joinNarrow =
            ProjectConcurrencyAnalyzer(joinOnly).analyze(lifecycle.units());
        const ProjectAnalysisReport joinFull =
            ProjectConcurrencyAnalyzer().analyze(lifecycle.units());

        return assertTrue(countRacesOn(raceNarrow.report, "shared_counter") ==
                              countRacesOn(raceFull.report, "shared_counter"),
                          "a data-race-only run reports the cross-unit race the full run does") &&
               assertTrue(countRacesOn(raceNarrow.report, "shared_counter") > 0,
                          "and that race is actually reported") &&
               assertTrue(diagnosticLines(raceNarrow.report) ==
                              diagnosticLines(raceFull.report, RuleId::DataRaceGlobal),
                          "with the same diagnostics, not merely the same count") &&
               assertTrue(countMissingJoins(joinNarrow.report) == 0,
                          "a missing-join-only run still sees the join in the other unit") &&
               assertTrue(countMissingJoins(joinFull.report) == 0, "as the full run does") &&
               assertTrue(joinNarrow.complete() && raceNarrow.complete(),
                          "a narrow selection analyses every unit of the project") &&
               everyRuleAloneMatchesTheFullRun();
    }

    /// A rule no fact from another unit can change owes the program nothing: on any project, no
    /// unit is analysed a second time for it (#52).
    bool testUnitLocalRulesReanalyseNothing()
    {
        bool ok = true;
        for (const ProjectFixture& fixture : everyProjectFixture())
        {
            CompiledProject project;
            if (!compileFixture(fixture, project))
                return false;

            for (const RuleId rule : kUnitLocalRules)
            {
                const ProjectAnalysisReport analysis =
                    ProjectConcurrencyAnalyzer(AnalysisOptions{.enabledRules = {rule}})
                        .analyze(project.units());
                if (analysis.reanalyzedUnitCount == 0)
                    continue;

                std::cerr << "[FAIL] " << fixture.units.front() << ": " << toString(rule)
                          << " reanalysed " << analysis.reanalyzedUnitCount << " unit(s)\n";
                ok = false;
            }
        }
        return ok;
    }

    /// A narrow project run summarises the functions its rules computed something about, as a
    /// single-unit run does (#52). A rule reading no entry concurrency lists no function merely
    /// because a thread reaches it: only functions carrying one of its diagnostics. For the
    /// rules no other unit can inform, those are the single-unit diagnostics, so the summaries
    /// are the single-unit ones. missing-join, fork-after-thread and unreaped-child are left out
    /// of that last comparison on purpose: another unit legitimately changes what they report.
    bool testNarrowFunctionsMatchWhatTheRulesComputed()
    {
        const auto readsEntryConcurrency = [](RuleId rule)
        {
            return std::find(std::begin(kEntryConcurrencyRules), std::end(kEntryConcurrencyRules),
                             rule) != std::end(kEntryConcurrencyRules);
        };
        const auto isUnitLocal = [](RuleId rule)
        {
            return std::find(std::begin(kUnitLocalRules), std::end(kUnitLocalRules), rule) !=
                   std::end(kUnitLocalRules);
        };

        bool ok = true;
        for (const ProjectFixture& fixture : everyProjectFixture())
        {
            CompiledProject project;
            if (!compileFixture(fixture, project))
                return false;

            for (const RuleId rule : AnalysisOptions::allAvailable().enabledRules)
            {
                if (readsEntryConcurrency(rule))
                    continue;

                const AnalysisOptions options{.enabledRules = {rule}};
                const DiagnosticReport report =
                    ProjectConcurrencyAnalyzer(options).analyze(project.units()).report;

                bool onlyDiagnosed = true;
                for (const FunctionSummary& function : report.functions)
                {
                    onlyDiagnosed = onlyDiagnosed && function.hasDiagnostics &&
                                    !function.threadReachable && function.threadEntries.empty();
                }
                if (!onlyDiagnosed)
                {
                    std::cerr << "[FAIL] " << fixture.units.front() << ": " << toString(rule)
                              << " summarises functions it computed nothing about\n";
                    ok = false;
                }

                if (!isUnitLocal(rule))
                    continue;

                std::vector<std::string> singleUnit;
                for (std::size_t index = 0; index < fixture.units.size(); ++index)
                {
                    const std::vector<std::string> unitFunctions = summarisedFunctions(
                        SingleTUConcurrencyAnalyzer(options).analyze(project.moduleAt(index)));
                    singleUnit.insert(singleUnit.end(), unitFunctions.begin(), unitFunctions.end());
                }
                std::sort(singleUnit.begin(), singleUnit.end());
                if (summarisedFunctions(report) != singleUnit)
                {
                    std::cerr << "[FAIL] " << fixture.units.front() << ": " << toString(rule)
                              << " summarises other functions than its single-unit runs\n";
                    ok = false;
                }
            }
        }
        return ok;
    }

    /// An empty project is a valid input, not a crash.
    bool testEmptyProjectIsAnEmptyReport()
    {
        const ProjectAnalysisReport analysis =
            ProjectConcurrencyAnalyzer().analyze(ProjectUnitSource{});
        return assertTrue(analysis.report.diagnostics.empty(),
                          "an empty project reports nothing") &&
               assertTrue(analysis.skippedIncompatibleModules.empty(),
                          "an empty project skips nothing");
    }
} // namespace

int main()
{
    bool ok = true;
    ok = testSpawnInOneUnitRacesWithBodyInAnother() && ok;
    ok = testWorkerUnitAloneReportsNothing() && ok;
    ok = testUnitOrderDoesNotChangeTheReport() && ok;
    ok = testRepeatedUnitIsNotReportedTwice() && ok;
    ok = testExternGlobalDefinedInAnotherUnitIsTracked() && ok;
    ok = testUnresolvedExternIsNotTracked() && ok;
    ok = testIdiomaticServiceReportsWhatCrossesTheProgram() && ok;
    ok = testForkingUnitAloneDoesNotSeeTheThreads() && ok;
    ok = testChildReapedInAnotherUnitIsCollected() && ok;
    ok = testForkingUnitAloneStillReportsTheChild() && ok;
    ok = testReapingReanalysisFollowsTheSelection() && ok;
    ok = testIdiomaticServiceKeepsItsKnownBlindSpots() && ok;
    ok = testHandleJoinedInAnotherUnitIsNotOutstanding() && ok;
    ok = testCreatingUnitAloneStillReportsTheHandle() && ok;
    ok = testLockWrapperDefinedInAnotherUnitClosesTheCycle() && ok;
    ok = testWorkerUnitWithoutTheHelpersReportsNoCycle() && ok;
    ok = testProjectModeKeepsEverySingleUnitFinding() && ok;
    ok = testNarrowRuleSelectionKeepsCrossUnitConclusions() && ok;
    ok = testExternGlobalReanalysisFollowsTheSelection() && ok;
    ok = testLockSummaryReanalysisFollowsTheSelection() && ok;
    ok = testUnitLocalRulesReanalyseNothing() && ok;
    ok = testNarrowFunctionsMatchWhatTheRulesComputed() && ok;
    ok = testConstantDeclarationsTriggerNoSecondPass() && ok;
    ok = testEmptyProjectIsAnEmptyReport() && ok;
    ok = testUnreadableUnitIsReportedAndTheRestAnalysed() && ok;
    ok = testReloadingAUnitYieldsTheSameProgram() && ok;
    ok = testLiveUnitBoundIsRespected() && ok;
    ok = testUnitWithAnotherAbiIsSkippedByName() && ok;
    ok = testConcurrencyDoesNotChangeTheReport() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] cross-TU analysis tests\n";
    return 0;
}
