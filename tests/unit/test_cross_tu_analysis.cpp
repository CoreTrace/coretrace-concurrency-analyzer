// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"
#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

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
    using ctrace::concurrency::CompileRequest;
    using ctrace::concurrency::CompileResult;
    using ctrace::concurrency::Diagnostic;
    using ctrace::concurrency::DiagnosticReport;
    using ctrace::concurrency::InMemoryIRCompiler;
    using ctrace::concurrency::IRFormat;
    using ctrace::concurrency::ProjectAnalysisReport;
    using ctrace::concurrency::ProjectConcurrencyAnalyzer;
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

    /// Owns every compiled unit of a project so the modules outlive the analysis that reads them.
    class CompiledProject
    {
      public:
        [[nodiscard]] bool add(std::string_view relativePath)
        {
            CompileRequest request;
            request.inputFile = projectFixturePath(relativePath).string();
            request.format = IRFormat::BC;

            CompileResult result = InMemoryIRCompiler().compile(request, context_);
            if (!result.success || result.module == nullptr)
            {
                std::cerr << "[FAIL] project fixture compile failed for " << relativePath << "\n";
                return false;
            }

            // parseBC keeps a non-owning view over the bitcode buffer, so it must stay alive too.
            bitcodes_.push_back(std::move(result.llvmBitcode));
            modules_.push_back(std::move(result.module));
            return true;
        }

        [[nodiscard]] std::vector<const llvm::Module*> modules() const
        {
            std::vector<const llvm::Module*> pointers;
            pointers.reserve(modules_.size());
            for (const std::unique_ptr<llvm::Module>& module : modules_)
                pointers.push_back(module.get());

            return pointers;
        }

        [[nodiscard]] const llvm::Module& moduleAt(std::size_t index) const
        {
            return *modules_.at(index);
        }

      private:
        llvm::LLVMContext context_;
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
            ProjectConcurrencyAnalyzer().analyze(project.modules());

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

        const ProjectAnalysisReport first = ProjectConcurrencyAnalyzer().analyze(forward.modules());
        const ProjectAnalysisReport second =
            ProjectConcurrencyAnalyzer().analyze(reversed.modules());

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

        std::vector<const llvm::Module*> modules = project.modules();
        const ProjectAnalysisReport once = ProjectConcurrencyAnalyzer().analyze(modules);

        modules.push_back(&project.moduleAt(1));
        const ProjectAnalysisReport twice = ProjectConcurrencyAnalyzer().analyze(modules);

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
            ProjectConcurrencyAnalyzer().analyze(project.modules());

        return assertTrue(countRacesOn(analysis.report, "g_shared_state") > 0,
                          "a global defined in another unit must still be tracked");
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
            ProjectConcurrencyAnalyzer().analyze(project.modules());

        return assertTrue(countDeadlocks(analysis.report) > 0,
                          "an inversion through helpers defined elsewhere must be reported");
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
            ProjectConcurrencyAnalyzer().analyze(project.modules());

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
            const ProjectAnalysisReport asProject =
                ProjectConcurrencyAnalyzer().analyze({compiled.module.get()});

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

    /// An empty project is a valid input, not a crash.
    bool testEmptyProjectIsAnEmptyReport()
    {
        const ProjectAnalysisReport analysis = ProjectConcurrencyAnalyzer().analyze({});
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
    ok = testHandleJoinedInAnotherUnitIsNotOutstanding() && ok;
    ok = testCreatingUnitAloneStillReportsTheHandle() && ok;
    ok = testLockWrapperDefinedInAnotherUnitClosesTheCycle() && ok;
    ok = testWorkerUnitWithoutTheHelpersReportsNoCycle() && ok;
    ok = testProjectModeKeepsEverySingleUnitFinding() && ok;
    ok = testEmptyProjectIsAnEmptyReport() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] cross-TU analysis tests\n";
    return 0;
}
