// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"
#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/LLVMContext.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace
{
    using ctrace::concurrency::AnalysisOptions;
    using ctrace::concurrency::CompileRequest;
    using ctrace::concurrency::CompileResult;
    using ctrace::concurrency::ConfidenceLevel;
    using ctrace::concurrency::Diagnostic;
    using ctrace::concurrency::DiagnosticReport;
    using ctrace::concurrency::InMemoryIRCompiler;
    using ctrace::concurrency::IRFormat;
    using ctrace::concurrency::RuleId;
    using ctrace::concurrency::SingleTUConcurrencyAnalyzer;

    std::filesystem::path fixturePath(std::string_view relativePath)
    {
        return std::filesystem::path(CORETRACE_PROJECT_SOURCE_DIR) / relativePath;
    }

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    std::optional<DiagnosticReport> analyzeFixture(std::string_view relativePath,
                                                   AnalysisOptions options = {},
                                                   std::vector<std::string> extraCompileArgs = {})
    {
        llvm::LLVMContext context;
        InMemoryIRCompiler compiler;

        CompileRequest request;
        request.inputFile = fixturePath(relativePath).string();
        request.extraCompileArgs = std::move(extraCompileArgs);
        request.format = IRFormat::BC;

        CompileResult compileResult = compiler.compile(request, context);
        if (!compileResult.success || compileResult.module == nullptr)
        {
            std::cerr << "[FAIL] fixture compile failed for " << relativePath << "\n";
            return std::nullopt;
        }

        SingleTUConcurrencyAnalyzer analyzer(std::move(options));
        return analyzer.analyze(*compileResult.module);
    }

    std::optional<std::string> symbolOf(const Diagnostic& diagnostic)
    {
        const auto it = diagnostic.properties.find("symbol");
        if (it == diagnostic.properties.end())
            return std::nullopt;

        if (const auto* value = std::get_if<std::string>(&it->second))
            return *value;

        return std::nullopt;
    }

    bool locationReferencesFixture(const ctrace::concurrency::SourceLocation& location,
                                   std::string_view fixtureName)
    {
        return location.file.find(fixtureName) != std::string::npos;
    }

    std::optional<std::string> stringPropertyOf(const Diagnostic& diagnostic,
                                                std::string_view propertyName)
    {
        const auto it = diagnostic.properties.find(std::string(propertyName));
        if (it == diagnostic.properties.end())
            return std::nullopt;

        if (const auto* value = std::get_if<std::string>(&it->second))
            return *value;

        return std::nullopt;
    }

    std::optional<std::int64_t> intPropertyOf(const Diagnostic& diagnostic,
                                              std::string_view propertyName)
    {
        const auto it = diagnostic.properties.find(std::string(propertyName));
        if (it == diagnostic.properties.end())
            return std::nullopt;

        if (const auto* value = std::get_if<std::int64_t>(&it->second))
            return *value;

        return std::nullopt;
    }

    std::optional<std::vector<std::string>> stringVectorPropertyOf(const Diagnostic& diagnostic,
                                                                   std::string_view propertyName)
    {
        const auto it = diagnostic.properties.find(std::string(propertyName));
        if (it == diagnostic.properties.end())
            return std::nullopt;

        if (const auto* value = std::get_if<std::vector<std::string>>(&it->second))
            return *value;

        return std::nullopt;
    }

    bool stringVectorPropertyContains(const Diagnostic& diagnostic, std::string_view propertyName,
                                      std::string_view expected)
    {
        const std::optional<std::vector<std::string>> values =
            stringVectorPropertyOf(diagnostic, propertyName);
        if (!values.has_value())
            return false;

        return std::find(values->begin(), values->end(), expected) != values->end();
    }

    bool hasDiagnosticForSymbol(const DiagnosticReport& report, std::string_view symbol)
    {
        return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                           [symbol](const auto& diagnostic)
                           {
                               const std::optional<std::string> value = symbolOf(diagnostic);
                               return value.has_value() && *value == symbol;
                           });
    }

    const Diagnostic* findFirstDiagnosticForRule(const DiagnosticReport& report, RuleId ruleId)
    {
        const auto it = std::find_if(report.diagnostics.begin(), report.diagnostics.end(),
                                     [ruleId](const Diagnostic& diagnostic)
                                     { return diagnostic.ruleId == ruleId; });
        return it == report.diagnostics.end() ? nullptr : &*it;
    }

    std::size_t countDiagnosticsForRule(const DiagnosticReport& report, RuleId ruleId)
    {
        return static_cast<std::size_t>(std::count_if(
            report.diagnostics.begin(), report.diagnostics.end(),
            [ruleId](const Diagnostic& diagnostic) { return diagnostic.ruleId == ruleId; }));
    }

    bool testDataRaceBasicIsReported()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_basic.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(), "data_race_basic should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "shared_counter"),
                          "data_race_basic should report shared_counter") &&
               assertTrue(report->diagnostics.front().ruleId == RuleId::DataRaceGlobal,
                          "diagnostics should carry a stable rule id") &&
               assertTrue(report->diagnostics.front().location.line == 10,
                          "data_race_basic should report line 10") &&
               assertTrue(report->diagnostics.front().location.column == 23,
                          "data_race_basic should report column 23") &&
               assertTrue(!report->diagnostics.front().location.function.empty(),
                          "diagnostics should carry function names") &&
               assertTrue(report->diagnosticsSummary.error >= 1,
                          "data_race_basic should count an error diagnostic");
    }

    bool testDataRaceBasicReportsDirectAliasHighConfidence()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_basic.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic = findFirstDiagnosticForRule(*report, RuleId::DataRaceGlobal);
        return assertTrue(diagnostic != nullptr, "data_race_basic should report a race") &&
               assertTrue(diagnostic->confidence == ConfidenceLevel::High,
                          "direct global access should produce high confidence") &&
               assertTrue(stringPropertyOf(*diagnostic, "firstAliasProvenance") == "direct",
                          "first direct access should expose direct alias provenance") &&
               assertTrue(stringPropertyOf(*diagnostic, "secondAliasProvenance") == "direct",
                          "second direct access should expose direct alias provenance") &&
               assertTrue(stringVectorPropertyContains(*diagnostic, "variableAliasing", "direct"),
                          "direct race should expose direct variable aliasing");
    }

    bool testAtomicVsNonAtomicReportsSharedState()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_atomic_vs_non_atomic.cpp",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(),
                          "cpp_atomic_vs_non_atomic should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "state"),
                          "cpp_atomic_vs_non_atomic should report state");
    }

    bool testClassDataRaceReportsGlobalCounter()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_data_race_class.cpp",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(),
                          "cpp_data_race_class should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "global_counter"),
                          "cpp_data_race_class should report global_counter") &&
               assertTrue(report->diagnostics.front().location.function == "increment",
                          "cpp_data_race_class should point to increment") &&
               assertTrue(report->diagnostics.front().location.line == 13,
                          "cpp_data_race_class should report line 13");
    }

    bool testSharedObjectByRefReportsGlobalCounter()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_shared_object_by_ref.cpp",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(),
                          "cpp_shared_object_by_ref should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "global_counter"),
                          "cpp_shared_object_by_ref should report global_counter");
    }

    bool testMutexProtectedFixtureHasNoDiagnostics()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_mutex_protected.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "mutex-protected fixture should not report a race") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "mutex-protected fixture should not count error diagnostics");
    }

    bool testTwoGlobalFixtureOnlyReportsRacySymbol()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_split_symbols.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(),
                          "split-symbol fixture should report at least one race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "racy_counter"),
                          "split-symbol fixture should report racy_counter") &&
               assertTrue(!hasDiagnosticForSymbol(*report, "safe_counter"),
                          "split-symbol fixture should not report safe_counter");
    }

    bool testThreadLocalClassHasNoDiagnostics()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_thread_local_class.cpp",
                           AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "thread-local class fixture should not report missing join") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "thread-local class fixture should not count error diagnostics") &&
               assertTrue(report->diagnosticsSummary.warning == 0,
                          "thread-local class fixture should not count warning diagnostics");
    }

    bool testMoveSemanticsRaceUsesUserLocations()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_move_semantics_race.cpp",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        bool allSharedResource = true;
        bool allPrimaryLocationsInFixture = true;
        bool hasLoweredRelatedLocation = false;

        for (const auto& diagnostic : report->diagnostics)
        {
            const std::optional<std::string> symbol = symbolOf(diagnostic);
            if (!symbol.has_value() || *symbol != "shared_resource")
                allSharedResource = false;

            if (!locationReferencesFixture(diagnostic.location, "cpp_move_semantics_race.cpp"))
                allPrimaryLocationsInFixture = false;

            for (const auto& related : diagnostic.relatedLocations)
            {
                if (related.label.starts_with("Lowered ") &&
                    !locationReferencesFixture(related.location, "cpp_move_semantics_race.cpp"))
                {
                    hasLoweredRelatedLocation = true;
                }
            }
        }

        return assertTrue(!report->diagnostics.empty(),
                          "cpp_move_semantics_race should report races") &&
               assertTrue(allSharedResource,
                          "cpp_move_semantics_race should only report shared_resource") &&
               assertTrue(!hasDiagnosticForSymbol(*report, "_ZNSt3__14coutE"),
                          "cpp_move_semantics_race should not report std::cout") &&
               assertTrue(allPrimaryLocationsInFixture,
                          "cpp_move_semantics_race should use fixture locations as primaries") &&
               assertTrue(hasLoweredRelatedLocation,
                          "cpp_move_semantics_race should preserve lowered related locations");
    }

    bool testCallsiteLockProtectedHelperHasNoRace()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/data-race/data_race_callsite_lock_protected.c",
            AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "callsite-protected helper fixture should not report a race") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "callsite-protected helper fixture should not count error diagnostics");
    }

    bool testAliasGlobalPointerReportsLowConfidenceRace()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/"
                           "data_race_alias_global_pointer_reports_low_confidence.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic = findFirstDiagnosticForRule(*report, RuleId::DataRaceGlobal);
        return assertTrue(diagnostic != nullptr,
                          "alias global pointer fixture should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "shared_counter"),
                          "alias global pointer fixture should resolve shared_counter") &&
               assertTrue(diagnostic->confidence == ConfidenceLevel::Low,
                          "may-alias race should produce low confidence") &&
               assertTrue(stringPropertyOf(*diagnostic, "firstAliasProvenance") == "may_alias",
                          "first aliased access should expose may_alias provenance") &&
               assertTrue(stringPropertyOf(*diagnostic, "secondAliasProvenance") == "may_alias",
                          "second aliased access should expose may_alias provenance") &&
               assertTrue(
                   stringVectorPropertyContains(*diagnostic, "variableAliasing", "may_alias"),
                   "may-alias race should expose may_alias variable aliasing");
    }

    bool testAliasAmbiguousPointerHasNoFalsePositive()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/data-race/data_race_alias_ambiguous_pointer_no_fp.c",
            AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "ambiguous alias fixture should not report an arbitrary race") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "ambiguous alias fixture should not count error diagnostics");
    }

    bool testUnprotectedCallsiteHelperReportsRace()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/"
                           "data_race_callsite_unprotected_helper_reports_race.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(),
                          "unprotected callsite helper fixture should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "shared_counter"),
                          "unprotected callsite helper fixture should report shared_counter");
    }

    bool testPartialCallsiteLockReportsRace()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/data-race/data_race_partial_callsite_lock_reports_race.c",
            AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(!report->diagnostics.empty(),
                          "partial callsite lock fixture should report a race") &&
               assertTrue(hasDiagnosticForSymbol(*report, "shared_counter"),
                          "partial callsite lock fixture should report shared_counter");
    }

    bool testNestedCallsiteLockProtectedHelperHasNoRace()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/"
                           "data_race_nested_callsite_lock_protected_no_race.c",
                           AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "nested callsite-protected fixture should not report a race") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "nested callsite-protected fixture should not count error diagnostics");
    }

    bool testPThreadJoinAllHasNoMissingJoin()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/missing-join/pthread_join_all_no_missing_join.c",
            AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "pthread_join_all should not report missing join") &&
               assertTrue(report->diagnosticsSummary.warning == 0,
                          "pthread_join_all should not count warning diagnostics");
    }

    bool testPThreadDetachAllHasNoMissingJoin()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/missing-join/pthread_detach_all_no_missing_join.c",
            AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "pthread_detach_all should not report missing join") &&
               assertTrue(report->diagnosticsSummary.warning == 0,
                          "pthread_detach_all should not count warning diagnostics");
    }

    bool testPThreadHandlePointerJoinHasNoMissingJoin()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/missing-join/"
                           "pthread_create_join_through_handle_pointer_no_missing_join.c",
                           AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "pthread handle pointer fixture should not report missing join") &&
               assertTrue(report->diagnosticsSummary.warning == 0,
                          "pthread handle pointer fixture should not count warning diagnostics");
    }

    bool testStdThreadJoinHasNoMissingJoin()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/missing-join/cpp_std_thread_join_no_missing_join.cpp",
            AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "joined std::thread fixture should not report missing join") &&
               assertTrue(report->diagnosticsSummary.warning == 0,
                          "joined std::thread fixture should not count warning diagnostics");
    }

    bool testStdThreadMoveJoinHasNoMissingJoin()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/missing-join/cpp_std_thread_move_join_no_missing_join.cpp",
            AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "moved-and-joined std::thread fixture should not report missing join") &&
               assertTrue(
                   report->diagnosticsSummary.warning == 0,
                   "moved-and-joined std::thread fixture should not count warning diagnostics");
    }

    bool testMissingJoinBasicReportsOutstandingPThread()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/missing-join/missing_join_basic.c",
                           AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic = findFirstDiagnosticForRule(*report, RuleId::MissingJoin);
        return assertTrue(diagnostic != nullptr,
                          "missing_join_basic should report a missing pthread join") &&
               assertTrue(countDiagnosticsForRule(*report, RuleId::MissingJoin) == 1,
                          "missing_join_basic should emit a single missing-join diagnostic") &&
               assertTrue(diagnostic->location.function == "main",
                          "missing_join_basic should point to main") &&
               assertTrue(stringPropertyOf(*diagnostic, "handleKind") == "pthread",
                          "missing_join_basic should classify the handle as pthread") &&
               assertTrue(intPropertyOf(*diagnostic, "outstandingCount") == 1,
                          "missing_join_basic should report one outstanding thread");
    }

    bool testMissingJoinDetachMixReportsOnlyOutstandingThread()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/missing-join/missing_join_detach_mix.c",
                           AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic = findFirstDiagnosticForRule(*report, RuleId::MissingJoin);
        return assertTrue(diagnostic != nullptr,
                          "missing_join_detach_mix should report one outstanding thread") &&
               assertTrue(countDiagnosticsForRule(*report, RuleId::MissingJoin) == 1,
                          "missing_join_detach_mix should emit one missing-join diagnostic") &&
               assertTrue(intPropertyOf(*diagnostic, "createCount") == 1,
                          "missing_join_detach_mix should report one unresolved thread creation") &&
               assertTrue(
                   intPropertyOf(*diagnostic, "detachCount") == 0,
                   "missing_join_detach_mix diagnostic should track only the unresolved handle") &&
               assertTrue(intPropertyOf(*diagnostic, "outstandingCount") == 1,
                          "missing_join_detach_mix should report one outstanding handle");
    }

    bool testPThreadJoinMixReportsOnlyUnresolvedHandle()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/missing-join/"
                           "pthread_join_mix_reports_only_unresolved_handle.c",
                           AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic = findFirstDiagnosticForRule(*report, RuleId::MissingJoin);
        return assertTrue(diagnostic != nullptr,
                          "pthread join mix should report one unresolved handle") &&
               assertTrue(countDiagnosticsForRule(*report, RuleId::MissingJoin) == 1,
                          "pthread join mix should emit one missing-join diagnostic") &&
               assertTrue(stringPropertyOf(*diagnostic, "handleKind") == "pthread",
                          "pthread join mix should classify the handle as pthread") &&
               assertTrue(intPropertyOf(*diagnostic, "outstandingCount") == 1,
                          "pthread join mix should report one outstanding handle");
    }

    bool testStdThreadMissingJoinReportsJoinableHandle()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/missing-join/cpp_std_thread_missing_join.cpp",
            AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic = findFirstDiagnosticForRule(*report, RuleId::MissingJoin);
        return assertTrue(diagnostic != nullptr,
                          "cpp_std_thread_missing_join should report a std::thread leak") &&
               assertTrue(countDiagnosticsForRule(*report, RuleId::MissingJoin) == 1,
                          "cpp_std_thread_missing_join should emit one missing-join diagnostic") &&
               assertTrue(
                   stringPropertyOf(*diagnostic, "handleKind") == "std::thread",
                   "cpp_std_thread_missing_join should classify the handle as std::thread") &&
               assertTrue(intPropertyOf(*diagnostic, "outstandingCount") == 1,
                          "cpp_std_thread_missing_join should report one outstanding handle");
    }

    bool testDetachedStdThreadFixtureHasNoMissingJoin()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/missing-join/cpp_missing_join.cpp",
                           AnalysisOptions{.enabledRules = {RuleId::MissingJoin}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "cpp_missing_join should not report missing join after detach") &&
               assertTrue(report->diagnosticsSummary.warning == 0,
                          "cpp_missing_join should not count warning diagnostics");
    }

    bool testDeadlockBasicReportsLockOrderCycle()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/deadlock/deadlock_basic.c",
                           AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic =
            findFirstDiagnosticForRule(*report, RuleId::DeadlockLockOrder);
        const std::optional<std::string> firstLock =
            diagnostic == nullptr ? std::nullopt : stringPropertyOf(*diagnostic, "firstLock");
        const std::optional<std::string> secondLock =
            diagnostic == nullptr ? std::nullopt : stringPropertyOf(*diagnostic, "secondLock");

        return assertTrue(diagnostic != nullptr,
                          "deadlock_basic should report a lock-order inversion") &&
               assertTrue(firstLock.has_value() && secondLock.has_value() &&
                              *firstLock != *secondLock,
                          "deadlock_basic should report two distinct locks") &&
               assertTrue(!diagnostic->relatedLocations.empty(),
                          "deadlock_basic should provide a conflicting lock-order location");
    }

    bool testRecursiveDeadlockReportsReacquiredLock()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/deadlock/recursive_deadlock.c",
                           AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic =
            findFirstDiagnosticForRule(*report, RuleId::DeadlockLockOrder);
        return assertTrue(diagnostic != nullptr,
                          "recursive_deadlock should report a self-deadlock") &&
               assertTrue(diagnostic->location.function == "helper_function",
                          "recursive_deadlock should point to helper_function") &&
               assertTrue(stringPropertyOf(*diagnostic, "firstLock") ==
                              stringPropertyOf(*diagnostic, "secondLock"),
                          "recursive_deadlock should report reacquiring the same lock");
    }

    bool testConsistentLockOrderHasNoDeadlock()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/deadlock/deadlock_consistent_order_no_diagnostic.c",
            AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "consistent lock order should not report deadlock") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "consistent lock order should not count error diagnostics");
    }

    bool testOppositeLockOrderOutsideThreadsHasNoDeadlock()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/deadlock/"
                           "deadlock_opposite_order_not_concurrent_no_diagnostic.c",
                           AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "non-concurrent opposite lock order should not report deadlock") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "non-concurrent opposite lock order should not count error diagnostics");
    }

    bool testIndependentLocksHaveNoDeadlock()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/deadlock/deadlock_independent_locks_no_diagnostic.c",
            AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}});
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "independent lock pairs should not report deadlock") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "independent lock pairs should not count error diagnostics");
    }

    // -----------------------------------------------------------------------------------
    // Fixture expectation table
    //
    // One row per concurrency fixture, pinning the exact number of diagnostics each rule
    // produces. Counting rather than merely asserting presence catches both regression
    // directions with the same row: a false positive raises a count, a false negative lowers
    // one. Adding a case costs a line, which is what makes broad coverage affordable.
    // -----------------------------------------------------------------------------------

    constexpr std::string_view kCxx20Standard = "-std=c++20";

    struct FixtureExpectation
    {
        /// Path relative to the project source directory.
        std::string_view path;
        /// Why this fixture exists; printed when the row fails.
        std::string_view intent;
        std::size_t dataRace = 0;
        std::size_t missingJoin = 0;
        std::size_t deadlock = 0;
        /// Symbol the data-race diagnostics must name; empty when not asserted.
        std::string_view racingSymbol;
        bool requiresCxx20 = false;
        /// The fixture is kept to exercise the compiler error path and is never analyzed.
        bool expectCompileFailure = false;
    };

    // clang-format off
    const std::vector<FixtureExpectation>& fixtureExpectations()
    {
        static const std::vector<FixtureExpectation> expectations = {
            // --- data race: conflicts that must be reported -------------------------------
            {.path = "tests/fixtures/concurrency/data-race/data_race_basic.c",
             .intent = "two workers increment the same global",
             .dataRace = 1, .racingSymbol = "shared_counter"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_split_symbols.c",
             .intent = "only the unprotected global of the pair races",
             .dataRace = 1, .racingSymbol = "racy_counter"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_mixed_access.c",
             .intent = "writer and reader entries conflict on the shared flag and value",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_callsite_unprotected_helper_reports_race.c",
             .intent = "helper reached from an unprotected call site still races",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_partial_callsite_lock_reports_race.c",
             .intent = "one unprotected call site among several keeps the race",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_alias_global_pointer_reports_low_confidence.c",
             .intent = "alias-resolved global access is reported with low confidence",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/race_condition_check_then_use.c",
             .intent = "check-then-use on shared state, with an unjoined handle",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_data_race_class.cpp",
             .intent = "member function of a global object races",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_shared_object_by_ref.cpp",
             .intent = "object shared by reference races on its global counter",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_atomic_vs_non_atomic.cpp",
             .intent = "non-atomic field of a partially atomic struct races",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_move_semantics_race.cpp",
             .intent = "producer and consumer race on the moved-from resource",
             .dataRace = 2},

            // --- data race: regressions fixed, must stay silent ---------------------------
            {.path = "tests/fixtures/concurrency/data-race/data_race_mutex_protected.c",
             .intent = "a common mutex protects both accesses"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_callsite_lock_protected.c",
             .intent = "the lock held at the call site protects the helper"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_nested_callsite_lock_protected_no_race.c",
             .intent = "nested call sites all hold the lock"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_alias_ambiguous_pointer_no_fp.c",
             .intent = "an ambiguous alias is dropped rather than guessed"},
            {.path = "tests/fixtures/concurrency/data-race/cpp_thread_local_class.cpp",
             .intent = "each worker owns its instance"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_disjoint_array_elements_no_fp.c",
             .intent = "workers own distinct elements of a global array"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_disjoint_struct_fields_no_fp.c",
             .intent = "workers write distinct fields of a global struct"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_sequential_threads_no_fp.c",
             .intent = "a join separates the two spawns"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_exclusive_branch_spawns_no_fp.c",
             .intent = "mutually exclusive branches never run two instances at once"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_rwlock_protected_no_fp.c",
             .intent = "a reader/writer lock protects the value, and is not data itself"},
            {.path = "tests/fixtures/concurrency/data-race/cpp_lock_guard_protected_no_fp.cpp",
             .intent = "std::lock_guard is a recognized acquisition"},
            {.path = "tests/fixtures/concurrency/data-race/cpp_scoped_lock_protected_no_fp.cpp",
             .intent = "std::scoped_lock acquires both mutexes safely"},
            {.path = "tests/fixtures/concurrency/data-race/cpp_atomic_operations_no_fp.cpp",
             .intent = "atomic operations never race with each other"},
            {.path = "tests/fixtures/concurrency/data-race/cpp_condition_variable_no_fp.cpp",
             .intent = "a textbook wait/notify pair is synchronized"},

            // --- data race: regressions fixed, must be reported ---------------------------
            {.path = "tests/fixtures/concurrency/data-race/data_race_main_thread_vs_worker.c",
             .intent = "the initial thread writes between the spawn and the join",
             .dataRace = 1, .racingSymbol = "escaping_counter"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_mixed_atomic_and_plain.c",
             .intent = "an atomic RMW still races against a plain increment",
             .dataRace = 1, .racingSymbol = "mixed_counter"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_distinct_member_mutexes.c",
             .intent = "sibling mutexes are distinct locks, so the field is unprotected",
             .dataRace = 1, .racingSymbol = "state"},
            {.path = "tests/fixtures/concurrency/thread-escape/thread_escape_posix.c",
             .intent = "a helper called from main and from a worker races with itself",
             .dataRace = 1, .racingSymbol = "buffer_index"},

            // --- deadlock -----------------------------------------------------------------
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_basic.c",
             .intent = "two workers take the same pair of locks in opposite orders",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/recursive_deadlock.c",
             .intent = "a helper reacquires a lock its caller already holds",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/lock_order_violation.c",
             .intent = "a three-lock wait-for cycle",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_three_lock_cycle.c",
             .intent = "cycles longer than a pair are found",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_main_thread_vs_worker.c",
             .intent = "the initial thread participates in the lock order",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_consistent_order_no_diagnostic.c",
             .intent = "a single consistent order cannot deadlock"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_independent_locks_no_diagnostic.c",
             .intent = "independent lock pairs form no cycle"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_opposite_order_not_concurrent_no_diagnostic.c",
             .intent = "opposite orders outside any thread cannot interleave"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_sibling_member_mutexes_no_diagnostic.c",
             .intent = "two mutexes in one aggregate are distinct locks"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_pthread_recursive_mutex_no_diagnostic.c",
             .intent = "a recursive mutex may be reacquired by its owner"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gate_lock_serializes_no_diagnostic.c",
             .intent = "a common outer lock serializes the inner orders"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_sequential_threads_no_diagnostic.c",
             .intent = "the two lock orders belong to threads with disjoint lifetimes"},
            {.path = "tests/fixtures/concurrency/deadlock/cpp_recursive_mutex_no_diagnostic.cpp",
             .intent = "std::recursive_mutex is designed to be relocked"},

            // --- missing join --------------------------------------------------------------
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_basic.c",
             .intent = "a created handle is never joined",
             .dataRace = 1, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_detach_mix.c",
             .intent = "only the handle that is neither joined nor detached is reported",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_multiple.c",
             .intent = "loop-created handles joined once; array-element insensitivity is known",
             .dataRace = 1, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_mix_reports_only_unresolved_handle.c",
             .intent = "only the unresolved handle of the pair is reported",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/cpp_std_thread_missing_join.cpp",
             .intent = "a std::thread destroyed while joinable",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/cpp_missing_join.cpp",
             .intent = "the worker races while its handle is left joinable",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_loop_create_single_join_reports_missing_join.c",
             .intent = "a loop creates one handle per iteration, joined only once",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "cpp_conditional_join_reports_missing_join.cpp",
             .intent = "a join on only one branch leaves a path without one",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/pthread_join_all_no_missing_join.c",
             .intent = "every handle is joined"},
            {.path = "tests/fixtures/concurrency/missing-join/pthread_detach_all_no_missing_join.c",
             .intent = "detaching resolves a handle as well as joining"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_create_join_through_handle_pointer_no_missing_join.c",
             .intent = "the handle is joined through a local pointer copy"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "cpp_std_thread_join_no_missing_join.cpp",
             .intent = "the std::thread is joined"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "cpp_std_thread_move_join_no_missing_join.cpp",
             .intent = "a moved-to handle carries the obligation"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_branch_creates_single_join_no_missing_join.c",
             .intent = "exclusive creation branches share the join that follows"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_detached_attribute_no_missing_join.c",
             .intent = "PTHREAD_CREATE_DETACHED resolves the handle at creation"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "cpp_thread_vector_join_no_missing_join.cpp",
             .intent = "handles moved into a container are joined through it"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "cpp_user_type_named_thread_no_missing_join.cpp",
             .intent = "a user type whose mangling contains \"thread\" is not a std::thread"},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_jthread_auto_join_no_missing_join.cpp",
             .intent = "std::jthread joins in its destructor",
             .requiresCxx20 = true},

            // --- condition variables ------------------------------------------------------
            // The state around a condition variable is ordinary shared data: the existing rules
            // still apply to it. Only the wait protocol itself is unmodelled.
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "condition_variable_unprotected_predicate.c",
             .intent = "the predicate is published without the mutex the waiter holds",
             .dataRace = 2,
             .racingSymbol = "data_ready"},
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "condition_variable_predicate_loop_no_diagnostic.c",
             .intent = "rechecking the predicate in a loop is the correct protocol"},
            {.path = "tests/fixtures/concurrency/condition-variable/condition_variable_spurious.c",
             .intent = "checking the predicate with if instead of while: no rule models "
                       "spurious wakeups yet"},

            // --- rules that do not exist yet: pinned as silent -----------------------------
            {.path = "tests/fixtures/concurrency/memory-barrier/missing_memory_barrier.c",
             .intent = "no rule models memory ordering yet; the plain accesses still race",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/thread-escape/fork_thread_race.c",
             .intent = "no rule models fork() semantics yet; the global access still races",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_race_std_async.cpp",
             .intent = "std::async spawns no recognized thread entry yet"},

            // --- compiler error path -------------------------------------------------------
            {.path = "tests/fixtures/concurrency/data-race/cpp_double_checked_locking.cpp",
             .intent = "kept to exercise the compile failure path",
             .expectCompileFailure = true},
        };

        return expectations;
    }
    // clang-format on

    /// Analyzes a fixture once with every rule enabled, then compares each rule's diagnostic
    /// count. Running the rules separately would recompile the fixture three times, and the
    /// checkers are independent, so one pass gives the same counts.
    bool checkFixtureExpectation(const FixtureExpectation& expectation)
    {
        std::vector<std::string> compileArgs;
        if (expectation.requiresCxx20)
            compileArgs.emplace_back(kCxx20Standard);

        const AnalysisOptions options{.enabledRules = {RuleId::DataRaceGlobal, RuleId::MissingJoin,
                                                       RuleId::DeadlockLockOrder}};
        const std::optional<DiagnosticReport> report =
            analyzeFixture(expectation.path, options, std::move(compileArgs));
        if (!report.has_value())
            return false;

        struct RuleColumn
        {
            RuleId rule;
            std::size_t expected;
            std::string_view name;
        };

        const RuleColumn columns[] = {
            {RuleId::DataRaceGlobal, expectation.dataRace, "data-race"},
            {RuleId::MissingJoin, expectation.missingJoin, "missing-join"},
            {RuleId::DeadlockLockOrder, expectation.deadlock, "deadlock-lock-order"},
        };

        bool ok = true;
        for (const RuleColumn& column : columns)
        {
            const std::size_t actual = countDiagnosticsForRule(*report, column.rule);
            if (actual == column.expected)
                continue;

            std::cerr << "[FAIL] " << expectation.path << " (" << expectation.intent
                      << "): " << column.name << " expected " << column.expected
                      << " diagnostic(s), got " << actual << "\n";
            ok = false;
        }

        if (!expectation.racingSymbol.empty() &&
            !hasDiagnosticForSymbol(*report, expectation.racingSymbol))
        {
            std::cerr << "[FAIL] " << expectation.path << " (" << expectation.intent
                      << "): expected a race on '" << expectation.racingSymbol << "'\n";
            ok = false;
        }

        return ok;
    }

    bool testFixtureExpectationTable()
    {
        bool ok = true;
        for (const FixtureExpectation& expectation : fixtureExpectations())
        {
            if (expectation.expectCompileFailure)
                continue;

            ok = checkFixtureExpectation(expectation) && ok;
        }

        return ok;
    }

    /// Guards against fixtures that exist but are asserted nowhere. Two real defects — the
    /// three-lock cycle and the helper shared between main and a worker — sat in the tree
    /// undetected precisely because their fixtures were never referenced.
    bool testEveryConcurrencyFixtureIsCovered()
    {
        static constexpr std::string_view kFixtureRoots[] = {
            "tests/fixtures/concurrency",
            "tests/fixtures/concurrency-cxx20",
        };

        std::vector<std::string> expectedPaths;
        expectedPaths.reserve(fixtureExpectations().size());
        for (const FixtureExpectation& expectation : fixtureExpectations())
            expectedPaths.emplace_back(expectation.path);
        std::sort(expectedPaths.begin(), expectedPaths.end());

        const std::filesystem::path projectRoot(CORETRACE_PROJECT_SOURCE_DIR);
        bool ok = true;

        for (const std::string_view root : kFixtureRoots)
        {
            const std::filesystem::path rootPath = projectRoot / root;
            if (!assertTrue(std::filesystem::is_directory(rootPath),
                            "fixture root " + std::string(root) + " should exist"))
            {
                ok = false;
                continue;
            }

            for (const std::filesystem::directory_entry& entry :
                 std::filesystem::recursive_directory_iterator(rootPath))
            {
                if (!entry.is_regular_file())
                    continue;

                const std::string extension = entry.path().extension().string();
                if (extension != ".c" && extension != ".cpp")
                    continue;

                const std::string relativePath =
                    entry.path().lexically_relative(projectRoot).generic_string();
                if (std::binary_search(expectedPaths.begin(), expectedPaths.end(), relativePath))
                    continue;

                std::cerr << "[FAIL] fixture " << relativePath
                          << " is not listed in the expectation table; add a row describing what "
                             "it pins\n";
                ok = false;
            }
        }

        return ok;
    }
} // namespace

int main()
{
    bool ok = true;

    ok = testDataRaceBasicIsReported() && ok;
    ok = testDataRaceBasicReportsDirectAliasHighConfidence() && ok;
    ok = testAtomicVsNonAtomicReportsSharedState() && ok;
    ok = testClassDataRaceReportsGlobalCounter() && ok;
    ok = testSharedObjectByRefReportsGlobalCounter() && ok;
    ok = testMutexProtectedFixtureHasNoDiagnostics() && ok;
    ok = testTwoGlobalFixtureOnlyReportsRacySymbol() && ok;
    ok = testThreadLocalClassHasNoDiagnostics() && ok;
    ok = testMoveSemanticsRaceUsesUserLocations() && ok;
    ok = testCallsiteLockProtectedHelperHasNoRace() && ok;
    ok = testAliasGlobalPointerReportsLowConfidenceRace() && ok;
    ok = testAliasAmbiguousPointerHasNoFalsePositive() && ok;
    ok = testUnprotectedCallsiteHelperReportsRace() && ok;
    ok = testPartialCallsiteLockReportsRace() && ok;
    ok = testNestedCallsiteLockProtectedHelperHasNoRace() && ok;
    ok = testPThreadJoinAllHasNoMissingJoin() && ok;
    ok = testPThreadDetachAllHasNoMissingJoin() && ok;
    ok = testPThreadHandlePointerJoinHasNoMissingJoin() && ok;
    ok = testStdThreadJoinHasNoMissingJoin() && ok;
    ok = testStdThreadMoveJoinHasNoMissingJoin() && ok;
    ok = testMissingJoinBasicReportsOutstandingPThread() && ok;
    ok = testMissingJoinDetachMixReportsOnlyOutstandingThread() && ok;
    ok = testPThreadJoinMixReportsOnlyUnresolvedHandle() && ok;
    ok = testStdThreadMissingJoinReportsJoinableHandle() && ok;
    ok = testDetachedStdThreadFixtureHasNoMissingJoin() && ok;
    ok = testDeadlockBasicReportsLockOrderCycle() && ok;
    ok = testRecursiveDeadlockReportsReacquiredLock() && ok;
    ok = testConsistentLockOrderHasNoDeadlock() && ok;
    ok = testOppositeLockOrderOutsideThreadsHasNoDeadlock() && ok;
    ok = testIndependentLocksHaveNoDeadlock() && ok;
    ok = testFixtureExpectationTable() && ok;
    ok = testEveryConcurrencyFixtureIsCovered() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] concurrency analysis tests\n";
    return 0;
}
