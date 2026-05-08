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
                                                   AnalysisOptions options = {})
    {
        llvm::LLVMContext context;
        InMemoryIRCompiler compiler;

        CompileRequest request;
        request.inputFile = fixturePath(relativePath).string();
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

    if (!ok)
        return 1;

    std::cout << "[PASS] concurrency analysis tests\n";
    return 0;
}
