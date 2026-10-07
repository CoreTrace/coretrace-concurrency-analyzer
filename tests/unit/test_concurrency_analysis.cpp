// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"
#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
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
    using ctrace::concurrency::FunctionSummary;
    using ctrace::concurrency::InMemoryIRCompiler;
    using ctrace::concurrency::IRFormat;
    using ctrace::concurrency::RelatedLocation;
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

    /// Every diagnostic of `ruleId` for `fixture` must read exactly `expectedMessage`. Used to
    /// pin the wording `describeConflictSubject` chooses (#111): an object handed to a thread
    /// must never be described as if it were a global, and a real global must keep its own name.
    bool checkEveryDiagnosticMessage(std::string_view fixture, RuleId ruleId,
                                     std::string_view expectedMessage,
                                     std::vector<std::string> extraCompileArgs = {})
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            fixture, AnalysisOptions{.enabledRules = {ruleId}}, std::move(extraCompileArgs));
        if (!assertTrue(report.has_value() && !report->diagnostics.empty(),
                        std::string(fixture) + " should report a diagnostic"))
        {
            return false;
        }

        bool ok = true;
        for (const Diagnostic& diagnostic : report->diagnostics)
        {
            ok = assertTrue(diagnostic.message == expectedMessage,
                            std::string(fixture) + ": expected message \"" +
                                std::string(expectedMessage) + "\", got \"" + diagnostic.message +
                                "\"") &&
                 ok;
        }
        return ok;
    }

    std::string describeLocation(const ctrace::concurrency::SourceLocation& location)
    {
        return location.file + ':' + std::to_string(location.line) + ':' +
               std::to_string(location.column) + '-' + std::to_string(location.endLine) + ':' +
               std::to_string(location.endColumn) + '@' + location.function;
    }

    std::string describeProperty(const ctrace::concurrency::DiagnosticPropertyValue& value)
    {
        if (const auto* flag = std::get_if<bool>(&value))
            return *flag ? "true" : "false";
        if (const auto* number = std::get_if<std::int64_t>(&value))
            return std::to_string(*number);
        if (const auto* text = std::get_if<std::string>(&value))
            return *text;

        std::string joined;
        for (const std::string& item : std::get<std::vector<std::string>>(value))
            joined += item + '\x1f';
        return joined;
    }

    /// Everything a diagnostic says, except the `id`. Ids are positions in the emitted list
    /// (`diag-1`, `diag-2`, ...), so they differ whenever the list differs and say nothing
    /// about the finding itself. Every other field is part of what the tool reports, down to
    /// the confidence, the taxonomies, the related locations, the notes and the properties.
    std::string describeDiagnostic(const Diagnostic& diagnostic)
    {
        std::string description;
        description += std::to_string(static_cast<int>(diagnostic.severity)) + '|';
        description += std::to_string(static_cast<int>(diagnostic.ruleId)) + '|';
        description += diagnostic.confidence.has_value()
                           ? std::to_string(static_cast<int>(*diagnostic.confidence))
                           : "-";
        description += '|' + describeLocation(diagnostic.location);
        description += '|' + diagnostic.message;
        for (const ctrace::concurrency::TaxonomyRef& taxonomy : diagnostic.taxonomies)
            description += "|tax:" + taxonomy.scheme + '/' + taxonomy.id + '/' + taxonomy.title;
        for (const ctrace::concurrency::RelatedLocation& related : diagnostic.relatedLocations)
            description += "|rel:" + related.label + '@' + describeLocation(related.location);
        for (const ctrace::concurrency::DiagnosticNote& note : diagnostic.notes)
            description += "|note:" + note.text;
        for (const auto& [key, value] : diagnostic.properties)
            description += "|prop:" + key + '=' + describeProperty(value);
        return description;
    }

    /// The diagnostics of one rule, fully described and in a canonical order. The order of the
    /// emitted list is not part of what the tool promises — `finalizeReport` sorts it, and the
    /// sort key is not a total order — so it is normalised away here; nothing else is.
    std::vector<std::string> describedDiagnosticsForRule(const DiagnosticReport& report,
                                                         RuleId ruleId)
    {
        std::vector<std::string> described;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.ruleId == ruleId)
                described.push_back(describeDiagnostic(diagnostic));
        }
        std::sort(described.begin(), described.end());
        return described;
    }

    /// Holds a compiled fixture so several rule selections can be analysed without compiling
    /// it again; the context has to outlive the module it owns.
    struct CompiledFixture
    {
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;
    };

    std::optional<CompiledFixture> compileFixture(std::string_view relativePath,
                                                  std::vector<std::string> extraCompileArgs = {})
    {
        CompiledFixture compiled;
        compiled.context = std::make_unique<llvm::LLVMContext>();

        CompileRequest request;
        request.inputFile = fixturePath(relativePath).string();
        request.extraCompileArgs = std::move(extraCompileArgs);
        request.format = IRFormat::BC;

        CompileResult result = InMemoryIRCompiler().compile(request, *compiled.context);
        if (!result.success || result.module == nullptr)
        {
            std::cerr << "[FAIL] fixture compile failed for " << relativePath << "\n";
            return std::nullopt;
        }

        compiled.module = std::move(result.module);
        return compiled;
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

    bool testHelperOrderReportedWhereTaken()
    {
        const std::optional<DiagnosticReport> report = analyzeFixture(
            "tests/fixtures/concurrency/deadlock/deadlock_helper_order_kept_where_taken.c",
            AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}});
        if (!report.has_value())
            return false;

        const Diagnostic* diagnostic =
            findFirstDiagnosticForRule(*report, RuleId::DeadlockLockOrder);
        bool pointsToHelper = diagnostic != nullptr && diagnostic->location.function == "forward";
        if (diagnostic != nullptr)
        {
            for (const ctrace::concurrency::RelatedLocation& related : diagnostic->relatedLocations)
                pointsToHelper = pointsToHelper || related.location.function == "forward";
        }
        return assertTrue(pointsToHelper,
                          "an order its calls add no lock to should point to forward, where it is "
                          "taken (#136)");
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

    /// What two runs report, in order: identical for the same program.
    std::vector<std::string> describeDiagnostics(const DiagnosticReport& report)
    {
        std::vector<std::string> described;
        for (const Diagnostic& diagnostic : report.diagnostics)
        {
            std::string text = diagnostic.message + " @" +
                               std::to_string(diagnostic.location.line) + ":" +
                               std::to_string(diagnostic.location.column);
            for (const RelatedLocation& related : diagnostic.relatedLocations)
                text += " | " + related.label + " @" + std::to_string(related.location.line);
            described.push_back(std::move(text));
        }
        return described;
    }

    bool testLockOrderSearchLimitIsANoticeNotAFinding()
    {
        constexpr std::string_view fixture =
            "tests/fixtures/concurrency/deadlock/deadlock_cycle_search_reaches_exploration_limit.c";
        const AnalysisOptions options{.enabledRules = {RuleId::DeadlockLockOrder}};
        const std::optional<DiagnosticReport> first = analyzeFixture(fixture, options);
        const std::optional<DiagnosticReport> second = analyzeFixture(fixture, options);
        if (!first.has_value() || !second.has_value())
            return false;

        // The walks from two locks stop at the bound; the report says so once.
        const bool oneNotice =
            first->notices.size() == 1 &&
            first->notices.front().id == "cycle-search-limit-reached" &&
            first->notices.front().ruleId == RuleId::DeadlockLockOrder &&
            first->notices.front().message.starts_with("lock-order cycle search incomplete");

        return assertTrue(oneNotice, "a search stopped at its bound should give one notice") &&
               assertTrue(first->diagnostics.size() == 4 && first->diagnosticsSummary.error == 4 &&
                              first->diagnosticsSummary.warning == 0 &&
                              first->diagnosticsSummary.info == 0,
                          "the notice should count as no diagnostic") &&
               assertTrue(describeDiagnostics(*first) == describeDiagnostics(*second) &&
                              first->notices == second->notices,
                          "a search stopped at its bound should report the same twice");
    }

    bool testCompleteLockOrderSearchGivesNoNotice()
    {
        const AnalysisOptions options{.enabledRules = {RuleId::DeadlockLockOrder}};
        const std::optional<DiagnosticReport> cycle = analyzeFixture(
            "tests/fixtures/concurrency/deadlock/deadlock_three_lock_cycle.c", options);
        // Dense orders the search prunes: without either pruning, it would stop at its bound.
        const std::optional<DiagnosticReport> pruned = analyzeFixture(
            "tests/fixtures/concurrency/deadlock/deadlock_cycle_search_finishes_by_pruning.c",
            options);
        if (!cycle.has_value() || !pruned.has_value())
            return false;

        return assertTrue(cycle->diagnostics.size() == 1 && cycle->notices.empty(),
                          "a search that judged every cycle should give no notice") &&
               assertTrue(pruned->diagnostics.size() == 1 && pruned->notices.empty(),
                          "a search its prunings keep within bound should give no notice");
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

    /// A false missing join an open issue tracks, by where it is reported.
    struct TrackedMissingJoin
    {
        std::string_view issue;
        std::string_view function;
        unsigned line = 0;
        unsigned column = 0;
    };

    struct FixtureExpectation
    {
        /// Path relative to the project source directory.
        std::string_view path;
        /// Why this fixture exists; printed when the row fails.
        std::string_view intent;
        std::size_t dataRace = 0;
        std::size_t missingJoin = 0;
        std::size_t deadlock = 0;
        std::size_t conditionWait = 0;
        std::size_t forkAfterThread = 0;
        std::size_t unreapedChild = 0;
        std::size_t threadArgumentEscape = 0;
        std::size_t unsafeSignalHandler = 0;
        std::size_t weakPublication = 0;
        std::size_t threadArgumentFreed = 0;
        std::size_t threadLocalEscape = 0;
        /// Symbol the data-race diagnostics must name; empty when not asserted.
        std::string_view racingSymbol;
        bool requiresCxx20 = false;
        /// The fixture is kept to exercise the compiler error path and is never analyzed.
        bool expectCompileFailure = false;
        /// Counts are lower bounds rather than exact values. Reserved for fixtures whose
        /// diagnostics depend on how a standard library lowers its own templates: libc++ and
        /// libstdc++ expose a different number of intermediate accesses for the same source, and
        /// pinning one of them would fail the other. What must not regress is that the race is
        /// still found at all.
        bool countsAreMinimums = false;
        /// False missing joins open issues track, each excusing one diagnostic reported where it
        /// says. The missing-join count still covers every other one, and an entry no longer
        /// reported fails the row: the issue may be fixed, and the entry must go.
        std::vector<TrackedMissingJoin> trackedMissingJoins;
    };

    // clang-format off
    const std::vector<FixtureExpectation>& fixtureExpectations()
    {
        static const std::vector<FixtureExpectation> expectations = {
            // --- data race: conflicts that must be reported -------------------------------
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_creator_and_thread_share_object.c",
             .intent = "the thread that hands the object over keeps using it",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_object_owns_its_thread_race.cpp",
             .intent = "a constructor that starts a thread leaves it running for its caller",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_joins_its_thread_in_destructor_no_fp.cpp",
             .intent = "the destructor's call to the base one reaches the thread member only "
                       "(#99)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_starts_thread_before_initializing_value_race.cpp",
             .intent = "a destructor joining the thread member declared first reaches that member "
                       "only; the initializer after it races (#110)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_assigns_its_thread_in_constructor_no_fp.cpp",
             .intent = "a thread assigned in the constructor body races with nothing the owner "
                       "touches (#99)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_destructor_writes_before_join_race.cpp",
             .intent = "a destructor writing the thread's field before the join still races (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_destructor_helper_reads_before_join_race.cpp",
             .intent = "a destructor reading the thread's field through a helper still races (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_member_joined_while_detached_race.cpp",
             .intent = "joining and detaching one thread member through methods race (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_owner_writes_local_object_race.cpp",
             .intent = "the owner of a local object running a member thread races with it (#103)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_owner_writes_global_object_race.cpp",
             .intent = "the owner of a global object running a member thread races with it (#103)",
             .dataRace = 1, .racingSymbol = "counter", .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_joined_in_method_then_read_no_fp.cpp",
             .intent = "a method's join ends the thread the constructor started (#99)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_detached_in_method_then_read_race.cpp",
             .intent = "detaching the thread in a method ends nothing (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_joined_on_one_branch_then_read_race.cpp",
             .intent = "a join on one branch leaves the thread running on the other (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_joins_other_member_then_read_race.cpp",
             .intent = "joining one thread member leaves the other running (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_joined_through_helper_then_read_no_fp.cpp",
             .intent = "a join two calls down still names the constructor's thread (#99)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_object_join_throws_then_read_race.cpp",
             .intent = "a join that throws has not waited for the thread (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_join_caught_in_method_then_read_race.cpp",
             .intent = "a method swallowing its join's exception has not waited for the thread "
                       "(#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_detached_then_given_another_thread_race.cpp",
             .intent = "a thread moved into the member is the one its join ends, not the "
                       "detached one (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_object_join_caught_in_helper_then_read_race.cpp",
             .intent = "a helper swallowing the joining method's exception has not waited for "
                       "the thread (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_two_objects_one_joined_then_read_race.cpp",
             .intent = "joining one object's thread leaves another object's running (#99)",
             .dataRace = 2, .racingSymbol = "_ZL4hits", .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_two_objects_both_joined_then_read_no_fp.cpp",
             .intent = "each object's join ends its own thread (#99): main's read races with "
                       "neither, but the two threads write hits together (#126)",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_service_started_and_stopped_then_read_no_fp.cpp",
             .intent = "a thread moved into a member is joined through that member (#99)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_service_read_before_stop_race.cpp",
             .intent = "a read before the joining method still races with the thread (#99)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_thread_safe_global_object_no_fp.cpp",
             .intent = "a method guarding its member with the object's mutex is guarded at every "
                       "call (#103)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_helper_joins_before_returning_no_fp.cpp",
             .intent = "a helper that waits for its thread hands nothing back",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_shared_object_member_method_race.cpp",
             .intent = "a method called on the object a thread holds reaches the same bytes",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/data_race_shared_heap_object.c",
             .intent = "heap state has no name, but both threads were handed the same pointer",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_basic.c",
             .intent = "two workers increment the same global",
             .dataRace = 1, .racingSymbol = "shared_counter"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_conditional_lock_wrapper.c",
             .intent = "a helper that locks on one branch only cannot protect its caller",
             .dataRace = 1, .racingSymbol = "shared_total"},
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
             .intent = "check-then-use on shared state; one join loop joins both spawn ranges "
                       "(#151)",
             .dataRace = 2},
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
             .dataRace = 1,
             .countsAreMinimums = true},

            // --- data race: regressions fixed, must stay silent ---------------------------
            {.path = "tests/fixtures/concurrency/data-race/atomic_wrapper_pure_helper_no_fp.c",
             .intent = "a helper touching no shared memory leaves an atomic wrapper atomic"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_mutex_protected.c",
             .intent = "a common mutex protects both accesses"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_object_member_mutex_no_fp.c",
             .intent = "the mutex guarding a shared object lives inside it and still counts"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_private_heap_object_no_fp.c",
             .intent = "the same body on two allocations of its own shares nothing"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_lock_wrapper_protected_no_fp.c",
             .intent = "a lock taken through a helper still protects the access"},
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
            {.path = "tests/fixtures/concurrency/data-race/data_race_field_through_local_pointer.c",
             .intent = "a field written through a local pointer keeps its offset (#102)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_field_through_local_pointer_sibling_no_fp.c",
             .intent = "a field written through a local pointer is not read as its neighbour (#102)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_field_through_two_local_pointers.c",
             .intent = "offsets taken before each of two local copies add up (#102)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_writes_sibling_field_no_fp.c",
             .intent = "a helper's write to the field it is handed stays at that field's offset (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_writes_same_field.c",
             .intent = "a helper handed the field the worker writes still races (#99)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_opaque_call_on_sibling_field_no_fp.c",
             .intent = "a call without a body reaches only the field it is handed, not the whole global (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_opaque_call_on_same_field.c",
             .intent = "a call without a body handed the field the worker writes still races (#99)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_opaque_call_from_array_element.c",
             .intent = "an element pointer handed on reaches later elements of its array (#99)",
             .dataRace = 1, .racingSymbol = "values"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_opaque_call_reaches_earlier_element.c",
             .intent = "an element pointer handed on reaches earlier elements of its array (#99)",
             .dataRace = 1, .racingSymbol = "values"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_clears_member_array_tail.c",
             .intent = "a helper clearing from inside an array member reaches the rest of it (#99)",
             .dataRace = 1, .racingSymbol = "buffer"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_array_element_sibling_field_no_fp.c",
             .intent = "an element pointer reaches its array, not the field next to it (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_opaque_call_from_array_element.c",
             .intent = "a call without a body handed an element pointer reaches its array (#99)",
             .dataRace = 1, .racingSymbol = "values"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_writes_array_element_no_fp.c",
             .intent = "a helper's sized write through an element pointer stays at it (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_writes_array_element.c",
             .intent = "a helper handed the element the worker writes still races, once (#99)",
             .dataRace = 1, .racingSymbol = "values"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_only_reads_no_fp.c",
             .intent = "a helper that only reads is a read at its call site (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_writes_through_stored_pointer.c",
             .intent = "a helper writing through a pointer the global holds reaches the global "
                       "(#99)",
             .dataRace = 1, .racingSymbol = "list"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_writes_through_chosen_field.c",
             .intent = "a helper writing through a field chosen at run time may write either (#99)",
             .dataRace = 1, .racingSymbol = "pair"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_under_global_lock_no_fp.c",
             .intent = "a helper's guarded update is guarded at its call sites too (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_reads_through_stored_pointer_no_fp.c",
             .intent = "a helper reading through a pointer the global holds only reads it (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_hands_on_stored_pointer.c",
             .intent = "a helper handing on a pointer the global holds reaches the global (#99)",
             .dataRace = 1, .racingSymbol = "list"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_advancing_pointer_under_locks_no_fp.c",
             .intent = "hand-over-hand locking by a recursion advancing its pointer terminates and "
                       "reports nothing (#117)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_advancing_pointer_race.c",
             .intent = "a recursion advancing its pointer terminates; its writes race at the first "
                       "element and at any element it reaches (#117)",
             .dataRace = 2, .racingSymbol = "counters"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_mutual_recursion_advancing_pointer_race.c",
             .intent = "a cycle of two functions advancing the pointer terminates and still races "
                       "(#117)",
             .dataRace = 2, .racingSymbol = "counters"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_same_pointer_other_field_no_fp.c",
             .intent = "a recursion handing on its pointer unchanged keeps each field apart "
                       "(#117)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_distinct_elements_no_fp.c",
             .intent = "a helper's store at the index each call passes: distinct elements (#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_distinct_elements_increment_no_fp.c",
             .intent = "a helper's increment at the index each call passes: distinct elements "
                       "(#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_same_element.c",
             .intent = "two calls passing the same index race on that element (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_unknown_at_calls.c",
             .intent = "indices unknown at both calls may name one element (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_known_against_unknown.c",
             .intent = "an unknown index may name the element a known one does (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_two_calls_below_main_no_fp.c",
             .intent = "a constant index passed two calls below main still places the access "
                       "(#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_forwarded_no_fp.c",
             .intent = "an index handed on unchanged by another helper still places the access "
                       "(#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_expression_same_element.c",
             .intent = "an index computed from the parameter is not the parameter (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_two_index_parameters_same_element.c",
             .intent = "two variable indices leave the element unknown (#159)",
             .dataRace = 1, .racingSymbol = "grid"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameter_negative_through_pointer.c",
             .intent = "a negative index is not read as a large unsigned one (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_pointer_and_index_parameters_no_fp.c",
             .intent = "a helper's access through its pointer at its index: distinct elements "
                       "(#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_pointer_and_index_parameters_same_element.c",
             .intent = "the pointer and the index together name one element (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_pointer_and_forwarded_index_no_fp.c",
             .intent = "an index handed on to a helper indexing a pointer still places it (#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_from_unknown_element.c",
             .intent = "a known index from an unknown element stays unknown (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_parameters_forwarded_twice.c",
             .intent = "two indices handed on from two parameters stay apart (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_opaque_call_at_index_parameter_reaches_earlier_element.c",
             .intent = "a call handed an element may reach any element, whatever the index "
                       "(#159, #99)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_memset_from_index_parameter_no_fp.c",
             .intent = "a memset from the element an index picks runs forward only (#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_index_too_large_stays_unknown.c",
             .intent = "an index whose offset overflows stays unknown (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_called_with_integers_by_three_contexts.c",
             .intent = "a call handing only integers leaves the callee's other accesses where "
                       "they were (#159)",
             .dataRace = 1, .racingSymbol = "total"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_main_indexes_with_its_argument.c",
             .intent = "an index no call passes stays unknown in the function (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_index_parameter_unchanged_no_fp.c",
             .intent = "an index a recursion hands on unchanged terminates and keeps its element "
                       "(#159)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_index_parameter_advancing.c",
             .intent = "an index a recursion moves terminates and stays unknown (#159, #117)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_index_advancing_from_main_and_thread.c",
             .intent = "two recursions moving their index up race once (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_pointer_advancing_from_main_and_thread.c",
             .intent = "two recursions moving their pointer race once (#159, #118)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_index_descending_from_main_and_thread.c",
             .intent = "two recursions moving their index down race once (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_mutual_recursion_index_from_main_and_thread.c",
             .intent = "two mutual recursions moving their index race once (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_constant_index_from_main_and_thread.c",
             .intent = "a recursive call passing a constant is widened too: one race (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_index_through_wrapper.c",
             .intent = "a recursion's widened accesses follow the index through a wrapper (#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_mutual_recursion_index_in_other_position.c",
             .intent = "around a cycle the index is followed from its parameter, not its position "
                       "(#159)",
             .dataRace = 1, .racingSymbol = "slots"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_recursion_index_from_pointer_caller.c",
             .intent = "a recursion's accesses never wait on a pointer its calls cannot name "
                       "(#159)",
             .dataRace = 1, .racingSymbol = "counts"},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_member_indexes_member_array_no_fp.cpp",
             .intent = "a method's access at its index in its object: distinct elements (#159)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_member_indexes_member_array_same_element.cpp",
             .intent = "a method's access at the same index in one object races (#159)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_thread_started_on_index_function.cpp",
             .intent = "a thread started on the function passes no index a call shows (#159)",
             .dataRace = 1, .racingSymbol = "_ZL5slots", .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_joined_in_other_function_no_fp.c",
             .intent = "a join in another function ends the thread through the same field (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_detached_in_other_function_race.c",
             .intent = "a detach in another function ends nothing (#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_read_after_failed_join_race.c",
             .intent = "on the branch a join failed the thread may still run (#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_read_after_successful_join_no_fp.c",
             .intent = "on the branch a join succeeded the thread has finished (#99)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_joined_on_one_branch_in_other_function_race.c",
             .intent = "a function joining on one branch only leaves the thread running (#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_join_failure_ignored_in_other_function_race.c",
             .intent = "a function returning past a failed join leaves the thread running (#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_stored_join_checked_no_fp.c",
             .intent = "a join result kept in a local and tested proves success past the test "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_stored_join_failed_race.c",
             .intent = "on the branch where a kept join result reports a failure the thread may "
                       "still run (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_stored_join_unread_no_fp.c",
             .intent = "a join result kept in a local never read is ignored, like an unused one "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_stored_join_printed_race.c",
             .intent = "a kept join result read but never tested proves nothing (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_stored_join_reported_no_fp.c",
             .intent = "printing a tested join result does not undo what the test proves (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_stored_join_overwritten_race.c",
             .intent = "a local written again no longer holds the join result it tests (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_previous_join_status_race.c",
             .intent = "a kept join status tested before the next join is that earlier join's "
                       "(#121)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_success_no_fp.c",
             .intent = "a helper returning its join's status is tested like the join (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_failure_race.c",
             .intent = "on the branch where a helper's join status reports a failure the thread "
                       "may still run (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_local_helper_join_status_success_no_fp.c",
             .intent = "a helper returning the status of joining the handle it is given ends that "
                       "thread where the status reads as success (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_nested_helper_join_status_success_no_fp.c",
             .intent = "a function returning a helper's join status is tested like the join "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_negated_race.c",
             .intent = "a helper transforming its join's status is not tested like the join "
                       "(#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_logged_no_fp.c",
             .intent = "a helper that also logs its join's status still returns that status "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_recorded_no_fp.c",
             .intent = "a helper that also records its join's status still returns that status "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_logged_on_failure_no_fp.c",
             .intent = "a status read back past a branch is still the one the helper kept (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_status_compared_race.c",
             .intent = "a helper returning a comparison of its join's status does not return the "
                       "status (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_join_failure_swallowed_race.c",
             .intent = "a helper overwriting its kept status no longer returns it (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_aborting_on_join_status_no_fp.c",
             .intent = "a function testing a helper's join status before every return joins like "
                       "the join (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_helper_ignoring_join_status_race.c",
             .intent = "a function going on past a helper's failed join has not joined (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_failed_local_join_race.c",
             .intent = "a join proven to cover every return ends a local handle's thread only where "
                       "it succeeded (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_failed_global_join_race.c",
             .intent = "the failed-join read with a global handle, which no completion proof "
                       "covers (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_successful_local_join_no_fp.c",
             .intent = "past a local handle's successful join the thread has ended (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_local_join_status_reported_no_fp.c",
             .intent = "a local handle's join status tested, printed and aborted on ends the "
                       "thread past the test (#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_failed_local_join_status_stored_race.c",
             .intent = "a kept join status reporting a failure leaves a local handle's thread "
                       "running (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_read_after_local_helper_join_status_failure_race.c",
             .intent = "a helper reporting a failed join has not ended the thread of the handle it "
                       "was given (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_started_after_failed_local_join_race.c",
             .intent = "a thread started where joining another failed runs beside it (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_started_after_failed_global_join_race.c",
             .intent = "the same with the first handle in a global (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_writer_after_join_loop_aborting_on_failure_no_fp.c",
             .intent = "a join loop that aborts on a failed join ends every thread it joins "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_writer_after_join_loop_ignoring_failure_race.c",
             .intent = "a join loop going on past a failed join joins every handle but ends "
                       "nothing (#121)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_writer_after_join_loop_checking_stored_status_no_fp.c",
             .intent = "a join loop that aborts on a failed kept status ends every thread it joins "
                       "(#121)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_restarted_in_other_function_race.c",
             .intent = "a join before a restart ends the earlier thread, not the restarted one "
                       "(#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_stopped_then_restarted_race.c",
             .intent = "a stop before the only start in a function does not end that start's "
                       "thread (#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_joined_then_restarted_race.c",
             .intent = "a join before the only start in a function does not end that start's "
                       "thread (#99)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_refilled_by_other_entry_then_stopped_race.c",
             .intent = "a callee's join through a field refilled by another thread ends that one "
                       "(#126)",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_started_twice_joined_once_race.c",
             .intent = "a join ends the thread its field holds, not the one it replaced (#99)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_handle_first_started_twice_joined_once_race.c",
             .intent = "starting a task writes its handle, not the whole task, when the handle is "
                       "its first field (#110)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_handle_array_first_started_twice_joined_once_race.c",
             .intent = "a handle in an array that is the task's first field reaches that array, "
                       "not the whole task (#110)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_handle_first_started_through_local_pointer_race.c",
             .intent = "a local pointer to the handle, the task's first field, still designates "
                       "and names that field (#110)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_started_again_in_other_function_race.c",
             .intent = "a function starting the task it received again joins only its own "
                       "thread (#99)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_task_detached_then_run_again_in_other_function_race.c",
             .intent = "a function joining the thread it started leaves the detached one running "
                       "(#99)",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/data_race_owner_writes_local_object.c",
             .intent = "the owner of a local object handed to a thread still races with it (#103)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_writes_local_object_through_pointer.c",
             .intent = "the owner's write through a local pointer to the object still races (#103)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_owner_writes_global_object.c",
             .intent = "the owner of a global object handed to a thread still races with it (#103)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_writes_other_field_of_local_object_no_fp.c",
             .intent = "the owner's field of a local object is not the thread's field (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_writes_other_field_of_global_object_no_fp.c",
             .intent = "the owner's field of a global object is not the thread's field (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_writes_local_object_before_spawn_no_fp.c",
             .intent = "the owner's write before the spawn is ordered before the thread (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_writes_local_object_after_join_no_fp.c",
             .intent = "the owner's write after the join is ordered after the thread (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_reassigns_object_pointer_no_fp.c",
             .intent = "repointing the variable the object was read from is not writing it (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_owner_locks_local_object_no_fp.c",
             .intent = "a local object's mutex has one name on both sides (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_locks_global_object_no_fp.c",
             .intent = "a global object's mutex has one name on both sides (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_locks_local_object_no_fp.c",
             .intent = "a helper's lock on its parameter's mutex is the local object's (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_locks_global_object_no_fp.c",
             .intent = "a helper's lock on its parameter's mutex is the global object's (#103)"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_owner_locks_other_mutex.c",
             .intent = "a mutex of the owner's own does not guard the object's field (#103)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_owner_unlocked_while_thread_locks.c",
             .intent = "one side under the object's mutex does not guard the other (#103)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helpers_lock_different_objects.c",
             .intent = "helpers locking the mutexes of different objects share no lock (#103)",
             .dataRace = 1, .racingSymbol = "total"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_workers_lock_their_own_objects.c",
             .intent = "threads locking the mutex of their own objects share no lock (#103)",
             .dataRace = 1, .racingSymbol = "total"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_sequential_threads_no_fp.c",
             .intent = "a join separates the two spawns"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_read_after_helper_join_no_fp.c",
             .intent = "a helper's join orders the worker's write before main's read (#89)"},
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
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_alias_sole_global_untouched_no_fp.cpp",
             .intent = "an unresolved container element is not the module's only global"},

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
             .intent = "a helper called from main and from a worker races with itself, on the "
                       "index and on the element it writes (#155)",
             .dataRace = 2, .racingSymbol = "buffer_index"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_main_and_thread.c",
             .intent = "a helper's single write races between main's call and the thread's (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_write_called_by_main_while_running.c",
             .intent = "an entry's single write races between main's call and its thread (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_two_calls_below_main.c",
             .intent = "a helper main reaches two calls down races with the thread's call (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_passes_global_from_main_and_thread.c",
             .intent = "a global a helper hands on races between main's call and the thread's "
                       "(#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_increment_from_main_and_thread.c",
             .intent = "a helper's increment from main and a thread is one race, not two (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_increment_called_by_main_while_running.c",
             .intent = "an entry's increment called by main while its thread runs is one race "
                       "(#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_started_twice_and_called_by_main.c",
             .intent = "an entry started twice and called by main is one race (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_called_by_main_on_its_thread_object.c",
             .intent = "the thread's object and the call's argument are two accesses, one race "
                       "(#155)",
             .dataRace = 1, .racingSymbol = "result"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_increment_called_by_main_on_its_thread_object.c",
             .intent = "the accesses bound by the spawn are the thread's alone, so the "
                       "increment is one race (#155)",
             .dataRace = 1, .racingSymbol = "result"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_two_threads.c",
             .intent = "a helper's single write races between two different threads (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_two_threads_after_main_call.c",
             .intent = "two threads race on a helper main called before starting them (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_thread_and_its_child.c",
             .intent = "a thread and the child it runs beside race on a helper's write (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_write_called_by_other_thread.c",
             .intent = "an entry's single write races between its thread and another thread's "
                       "call (#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_called_by_other_thread_on_its_thread_object.c",
             .intent = "the access bound by the spawn is its thread's alone, even where another "
                       "thread calls the entry: one race (#155)",
             .dataRace = 1, .racingSymbol = "a"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_called_on_other_object_by_main_and_thread.c",
             .intent = "a copy of the spawn's access at a call stays the thread's: only the "
                       "calls' own object races (#155)",
             .dataRace = 1, .racingSymbol = "b"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_opaque_call_from_two_threads_beside_observed_race.c",
             .intent = "an inferred effect compared with itself is noise beside a seen race "
                       "(#155)",
             .dataRace = 1, .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_main_after_join_no_fp.c",
             .intent = "main's call after the join is ordered after the thread's (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_entry_write_called_by_main_after_join_no_fp.c",
             .intent = "main's call of the entry after the join is ordered after its thread "
                       "(#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_main_before_spawn_no_fp.c",
             .intent = "main's call before the spawn is ordered before the thread's (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_under_lock_from_main_and_thread_no_fp.c",
             .intent = "a helper's write under its own lock is guarded in both callers (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_read_from_main_and_thread_no_fp.c",
             .intent = "a helper's single read races with nothing (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_atomic_store_from_main_and_thread_no_fp.c",
             .intent = "a helper's atomic store never races with itself (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_sequenced_threads_no_fp.c",
             .intent = "a join between two threads orders their calls of a helper (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_write_from_two_threads_under_lock_no_fp.c",
             .intent = "two threads calling a helper under one lock are ordered (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_read_from_two_threads_no_fp.c",
             .intent = "a helper's single read races with nothing across threads (#155)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_lambda_trampoline_thread_entry.cpp",
             .intent = "a captureless lambda converted to a function pointer is a thread entry",
             .dataRace = 1, .racingSymbol = "shared_counter"},

            // --- imported data-race fixtures (Nihil, 91e7431; #4) ---
            {.path = "tests/fixtures/concurrency/data-race/c_bitfield_race.c",
             .intent = "distinct bitfields share a storage unit and race",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_iterator_invalidation_race.cpp",
             .intent = "vector insertion races with iteration; counts vary with standard-library lowering",
             .dataRace = 2,
             .requiresCxx20 = true,
             .countsAreMinimums = true},
            {.path = "tests/fixtures/concurrency/data-race/cpp_lambda_capture_race.cpp",
             .intent = "known gap #54: vector-emplaced closures sharing a stack counter are not resolved",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/cpp_race_shared_map.cpp",
             .intent = "map insertion and traversal race; counts vary with standard-library lowering",
             .dataRace = 2,
             .requiresCxx20 = true,
             .countsAreMinimums = true},
            {.path = "tests/fixtures/concurrency/data-race/cpp_shared_ptr_race.cpp",
             .intent = "the shared pointer object races despite its atomic reference count; library-dependent accesses",
             .dataRace = 5,
             .requiresCxx20 = true,
             .countsAreMinimums = true},
            {.path = "tests/fixtures/concurrency/data-race/data_race_lazy_init.c",
             .intent = "lazy global initialization races; the separate full join loop completes every worker",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_stats_counter.c",
             .intent = "three shared statistics race; full joins complete the workers, so their ids outlive them",
             .dataRace = 3},
            {.path = "tests/fixtures/concurrency/data-race/data_race_stop_flag.c",
             .intent = "the creator writes the stop flag while its worker reads it",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_struct_fields.c",
             .intent = "only the common x field races, not the disjoint y and z fields",
             .dataRace = 1},

            // --- deadlock -----------------------------------------------------------------
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_through_lock_wrappers.c",
             .intent = "an inversion expressed only through lock helpers is still a cycle",
             .deadlock = 1},
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
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_std_lock_adopted_by_guards_no_diagnostic.cpp",
             .intent = "guards adopting the mutexes std::lock took wait for nothing (#114)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_deadlock_locks_adopted_by_guards.cpp",
             .intent = "locks taken in opposite orders deadlock, and adopting guards reacquire "
                       "nothing (#114)",
             .deadlock = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_locks_ordered_by_address_no_diagnostic.c",
             .intent = "an if/else taking the lower address first orders every thread alike "
                       "(#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_order_chosen_by_flag.c",
             .intent = "a flag choosing between opposite orders is no address order (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_order_chosen_by_other_addresses.c",
             .intent = "comparing the addresses of anything but the locks orders nothing (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_local_lock_order_chosen_by_other_addresses.c",
             .intent = "comparing the global lock with pointers other than the local lock's "
                       "object orders nothing (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_order_chosen_by_inequality.c",
             .intent = "testing two addresses for inequality says neither is lower (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_address_order_on_one_path.c",
             .intent = "an address order on one path still meets the fixed order of another "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_order_and_fixed_order_in_one_macro.c",
             .intent = "an order a macro takes twice, by address and in a fixed order, is not "
                       "address-ordered (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_opposite_address_orders.c",
             .intent = "taking the lower address first and taking the higher one first conflict "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_opposite_orders_inside_one_object.c",
             .intent = "comparing two locks of one object cannot tell which object is lower "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_opposite_orders.c",
             .intent = "a helper locks the accounts it is handed in the order it is given them "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_local_accounts.c",
             .intent = "the accounts a helper locks are fields of a local handed to the thread "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_deadlock_transfer_helper_lock_guards.cpp",
             .intent = "a helper guards the accounts it is handed with lock_guard (#114)",
             .deadlock = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_deadlock_transfer_helper_unique_locks.cpp",
             .intent = "a helper locks the accounts it is handed with unique_lock (#114)",
             .deadlock = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_deadlock_member_locks_other_account.cpp",
             .intent = "a member locks its own mutex, then the other object's (#114)",
             .deadlock = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_deadlock_member_calls_other_account.cpp",
             .intent = "a member holds its own mutex while a member of the other object locks "
                       "the other's (#114)",
             .deadlock = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_split_in_two_levels.c",
             .intent = "the two accounts are locked by two helpers, one calling the other (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_ten_levels_down.c",
             .intent = "the locking helper is ten calls down, each caller defined first (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_parameter_lock_under_global_two_callers.c",
             .intent = "a helper locks the account it is handed under the caller's global lock, "
                       "at one of two calls (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_parameter_lock_two_levels_under_global.c",
             .intent = "the caller's global lock is held two calls above the account's lock "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_named_lock_under_caller_lock.c",
             .intent = "a helper's named lock is taken under the lock one of its callers holds "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_parameter_lock_reacquired.c",
             .intent = "a helper locks again the account lock its caller holds (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_mutexes_passed_to_helper.c",
             .intent = "a helper locks the two mutexes it is handed in the order it is given them "
                       "(#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency-cxx20/cpp_deadlock_method_reenters_mutex.cpp",
             .intent = "a method locks again through `this` the mutex its caller holds, once per "
                       "call to the caller (#114)",
             .deadlock = 2, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_consistent_order_no_diagnostic.c",
             .intent = "both threads hand the helper the accounts in the same order (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_one_thread_no_diagnostic.c",
             .intent = "only one thread calls the helper in both orders (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_settle_helper_own_accounts_no_diagnostic.c",
             .intent = "each thread hands the helper its own account (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_same_account_guard_no_diagnostic.c",
             .intent = "one account passed for both parameters is no reacquisition: the helper "
                       "may test for it (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_before_spawn_no_diagnostic.c",
             .intent = "the inverted call is over before the other thread exists (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_address_order_no_diagnostic.c",
             .intent = "a helper taking the lower address first orders every call alike (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_address_order_then_fixed_order.c",
             .intent = "a helper's fixed order stays apart from its address order on the same "
                       "locks at each call (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_address_order_under_caller_lock_no_diagnostic.c",
             .intent = "a lock one caller holds before an address-ordered pair closes no cycle "
                       "(#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_ordered_pair_on_longer_cycle.c",
             .intent = "an address-ordered pair another order leaves may lie on a longer cycle "
                       "(#114)",
             .deadlock = 2},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_hand_over_hand_recursion_no_diagnostic.c",
             .intent = "hand-over-hand locking by a recursion moving its pointer, one direction "
                       "(#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_gated_calls_no_diagnostic.c",
             .intent = "the gate held at both concurrent calls serializes the helper's orders "
                       "(#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_gated_pair_no_diagnostic.c",
             .intent = "the gate held at the only two inverted calls serializes them (#114)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_parameter_locks_held_around_inversion.c",
             .intent = "an account lock each thread holds through its own parameter is no common "
                       "gate (#114)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_escrow_account_no_diagnostic.c",
             .intent = "a lock the caller cannot name is not the caller's parameter in that "
                       "position (#114)"},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_transfer_helper_std_lock_adopted_no_diagnostic.cpp",
             .intent = "std::lock in a helper takes the mutexes it is handed without deadlocking "
                       "(#114)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_recursive_mutex_reentered_by_method_no_diagnostic.cpp",
             .intent = "a recursive mutex locked again through `this` is no reacquisition (#114)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_pair_taken_before_and_after_spawn.c",
             .intent = "an order taken before the worker exists hides none taken while it runs "
                       "(#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_pair_taken_before_and_after_spawn_other_names.c",
             .intent = "the same program, other lock names: the search starts from the other lock "
                       "(#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_before_and_after_spawn.c",
             .intent = "a transfer made before the worker exists hides none made while it runs "
                       "(#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gated_section_before_ungated_section.c",
             .intent = "an order made under a common gate hides no order made without it (#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_ungated_section_before_gated_section.c",
             .intent = "the same two sections, the ungated one first (#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_longer_cycle_behind_one_thread_inversion.c",
             .intent = "an inversion one thread takes alone hides no longer cycle through its locks "
                       "(#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_longer_cycle_behind_one_thread_inversion_other_names.c",
             .intent = "the same program, other lock names: the search starts from the ring's "
                       "third lock (#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_gated_calls_ungated_worker.c",
             .intent = "main's gated transfer meets the worker's ungated one, behind a transfer "
                       "made before the worker exists (#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_ungated_worker_no_call_before_spawn.c",
             .intent = "the same gated transfer, without the one made before the worker exists "
                       "(#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_cycle_reported_from_lock_reached_first.c",
             .intent = "a cycle is reported from the lock the search reaches first; the fixture's "
                       "golden block pins its first order (#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_cycle_search_reaches_exploration_limit.c",
             .intent = "a search stopped at its bound keeps the cycles found before and after the "
                       "cut; the golden block pins them and the notice (#127)",
             .deadlock = 4},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_cycle_search_finishes_by_pruning.c",
             .intent = "orders that cannot run together, and locks that lead nowhere back, keep "
                       "the search within its bound: no notice (#127)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_ordered_pair_entered_and_left_no_diagnostic.c",
             .intent = "an address-ordered pair other orders both enter and leave closes no cycle "
                       "of its own (#138)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_ordered_pair_left_only_no_diagnostic.c",
             .intent = "an address-ordered pair other orders only leave (#138)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_ordered_pair_entered_and_left_direct_no_diagnostic.c",
             .intent = "the same pair entered and left, each thread locking it by address itself "
                       "(#138)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_ring_through_address_ordered_pair.c",
             .intent = "a ring through an address-ordered pair is reported, the pair alone is not "
                       "(#138)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_ring_through_address_ordered_pair_other_names.c",
             .intent = "the same ring, other lock names: the search starts from its third lock "
                       "(#138)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_ordered_pair_and_order_before_spawn_no_diagnostic.c",
             .intent = "an order closing a larger cycle before any worker exists keeps no report "
                       "on the pair (#138)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_three_locks_each_pair_by_address_no_diagnostic.c",
             .intent = "three locks, each pair taken lower address first by its own worker: no "
                       "ring closes (#138)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_ordered_pair_and_fixed_order_before_spawn_no_diagnostic.c",
             .intent = "an address-ordered choice stays rejected when a fixed order also joins the "
                       "pair (#138)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_address_order_on_one_path_fixed_order_reversed.c",
             .intent = "a cycle's order taken by address, followed by a fixed one, still deadlocks "
                       "(#138)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_two_pairs_by_address_third_fixed.c",
             .intent = "a ring whose orders are all taken by address but one still deadlocks "
                       "(#138)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_starts_worker_then_locks.c",
             .intent = "a worker the helper starts runs beside the orders it then takes (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_joins_worker_then_locks_no_diagnostic.c",
             .intent = "a worker the helper joins has ended before the orders it then takes "
                       "(#124)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_helper_starts_worker_then_locks_named_accounts.c",
             .intent = "a helper starts the worker, then locks two accounts by name (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_called_while_worker_runs.c",
             .intent = "the worker main started runs beside the helper's orders (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_helper_joins_worker_then_locks_named_accounts_no_diagnostic.c",
             .intent = "a helper joins the worker, then locks two accounts by name (#124)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_called_after_worker_joined_no_diagnostic.c",
             .intent = "the worker main joined is over before the helper's orders (#124)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_called_by_helper_starting_worker.c",
             .intent = "a worker started one call above the locking helper runs beside its "
                       "orders (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_called_by_helper_joining_worker_no_diagnostic.c",
             .intent = "a worker joined one call above the locking helper has ended before its "
                       "orders (#124)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_starts_worker_also_passed_in.c",
             .intent = "a worker the helper starts runs beside its orders even at a call no "
                       "worker runs at, though another call passes one in (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_locks_before_and_after_starting_worker.c",
             .intent = "two acquisitions of the helper that give one order at the call keep the "
                       "threads beside either (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_transfer_helper_after_call_starting_worker.c",
             .intent = "a worker a call of the helper leaves running runs beside the helper's "
                       "orders (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_relayed_helper_locks_before_and_after_starting_worker.c",
             .intent = "an order a caller keeps open widens once a later round finds another "
                       "acquisition behind it, and its callers see it (#124)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gate_taken_through_parameter_no_diagnostic.c",
             .intent = "the account lock both threads pass their helper is a gate around its "
                       "orders (#136)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gate_taken_by_name_in_helpers_no_diagnostic.c",
             .intent = "the helpers take the gate by name (#136)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gate_and_inversion_through_parameters_no_diagnostic.c",
             .intent = "the gate and the inverted locks are all passed to the helpers (#136)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gate_held_at_concurrent_calls_only_no_diagnostic.c",
             .intent = "a gate held at the calls that can run together, not at one made before "
                       "the worker exists (#136)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gate_held_at_every_call_no_diagnostic.c",
             .intent = "a gate held at every call to the helpers (#136)"},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_helper_order_kept_where_taken.c",
             .intent = "calls that add no lock leave the helper's order where it is taken (#136)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_ledger_reacquired_under_different_locks.c",
             .intent = "a reacquisition in a helper called under different locks is one, where it "
                       "is taken (#136)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_settle_run_directly_and_as_threads.c",
             .intent = "a function also started as a thread keeps its own orders for the threads "
                       "(#136)",
             .deadlock = 1},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_deadlock_default_delete_locks_resource.cpp",
             .intent = "a function of namespace std keeps its own orders, which no call takes "
                       "over (#136)",
             .deadlock = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/"
                     "deadlock_gated_helper_starts_worker_then_locks.c",
             .intent = "an order judged at the call keeps the worker the helper starts before it "
                       "(#136)",
             .deadlock = 1},

            // --- imported deadlock fixtures (Nihil, 91e7431; #4) ---
            {.path = "tests/fixtures/concurrency/deadlock/cpp_deadlock_self_lock.cpp",
             .intent = "a nonrecursive mutex is acquired again through a nested helper",
             .deadlock = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/cpp_future_mutex_deadlock.cpp",
             .intent = "known gap #54: future waits are not lock-order edges; std::async lowering varies by library",
             .requiresCxx20 = true, .countsAreMinimums = true},
            {.path = "tests/fixtures/concurrency/deadlock/cpp_scoped_lock_order.cpp",
             .intent = "opposite lock_guard acquisition orders form a cycle",
             .deadlock = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/deadlock/deadlock_condvar_wrong_mutex.c",
             .intent = "negative control: wait releases mutex_b and notifier never acquires mutex_a"},

            // --- missing join --------------------------------------------------------------
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_basic.c",
             .intent = "a created handle is never joined",
             .dataRace = 1, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_detach_mix.c",
             .intent = "only the handle that is neither joined nor detached is reported",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_multiple.c",
             .intent = "loop-created handles joined once; the ids they read are main's own "
                       "locals, which the unjoined threads outlive",
             .dataRace = 1, .missingJoin = 1, .threadArgumentEscape = 1},
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
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_over_consecutive_spawn_loops_no_missing_join.c",
             .intent = "one join loop over [0, 4) joins the spawn loops over [0, 2) and [2, 4) "
                       "(#151)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_short_of_last_spawn_loop_reports_missing_join.c",
             .intent = "a join loop over [0, 3) leaves the last slot of [2, 4) unjoined (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_over_other_row_reports_missing_join.c",
             .intent = "a join loop over row 0 joins none of row 1, a constant offset away (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_down_column_reports_missing_join.c",
             .intent = "a join loop down a column misses the row the spawn loop filled (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_unsigned_spawn_loop_signed_join_loop_reports_missing_join.c",
             .intent = "an unsigned spawn loop and a signed join loop to the same bound may part (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_spawn_loop_slot_from_other_local_reports_missing_join.c",
             .intent = "handles stored at a slot another local picks overwrite each other (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_spawn_loop_with_second_index_reports_missing_join.c",
             .intent = "a second varying index in the slot leaves the join loop's slots unproven (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_from_huge_unsigned_start_reports_missing_join.c",
             .intent = "an unsigned join loop starting past its bound joins nothing (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_other_spawn_hands_handle_array_to_its_thread_reports_missing_join.c",
             .intent = "another spawn handing the handle array to its threads leaves it unproven (#151)",
             .dataRace = 1, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_up_to_count_of_created_handles_no_missing_join.c",
             .intent = "a loop up to the count of created handles joins every one (#157)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_up_to_count_raised_at_each_store_no_missing_join.c",
             .intent = "a count raised before each next store fills [0, count) (#157)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_up_to_count_of_rounds_no_missing_join.c",
             .intent = "a count raised once a round ends at the number of rounds (#157)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_of_stored_handles_joined_over_constant_range_no_missing_join.c",
             .intent = "handles stored at threads[started++] for four rounds fill [0, 4) (#157)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_short_of_count_of_created_handles_reports_missing_join.c",
             .intent = "a join loop stopping short of the count leaves handles unjoined (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_lowered_before_join_loop_reports_missing_join.c",
             .intent = "a count lowered before the join loop leaves handles unjoined (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_of_created_handles_stored_by_round_reports_missing_join.c",
             .intent = "handles stored by round but counted by success are not all below the count (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_raised_on_failed_creation_reports_missing_join.c",
             .intent = "a count raised on a failed creation names a slot no thread holds (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_raised_on_some_successes_reports_missing_join.c",
             .intent = "a count raised on some successes only leaves handles past it (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_raised_on_some_rounds_reports_missing_join.c",
             .intent = "a count raised on some rounds only stops short of the threads (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_from_one_joined_over_constant_range_reports_missing_join.c",
             .intent = "a count from 1 fills [1, 4), which a loop over [0, 3) does not cover (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_of_rounds_cut_by_break_reports_missing_join.c",
             .intent = "a break before the raise leaves the last thread uncounted (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_reset_inside_spawn_loop_reports_missing_join.c",
             .intent = "a count reset inside the spawn loop lets handles be stored over (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_spawn_loop_bound_lowered_inside_reports_missing_join.c",
             .intent = "a spawn loop whose bound changes fills no range a join loop can match (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_of_rounds_of_breaking_loop_reports_missing_join.c",
             .intent = "a count of the rounds of a loop that may break is not its trip count (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_of_rounds_of_nested_loop_reports_missing_join.c",
             .intent = "a count raised in a nested loop is not the inner loop's trip count (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_count_lowered_each_round_reports_missing_join.c",
             .intent = "a count lowered each round is no count of rounds (#157)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_over_handles_created_one_by_one_no_missing_join.c",
             .intent = "a loop over threads created one by one into its slots joins them all (#161)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_by_pointer_over_handles_no_missing_join.c",
             .intent = "a loop walking the handles by pointer joins them all (#161)"},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_short_of_created_slot_reports_missing_join.c",
             .intent = "a loop over [0, 1) leaves the handle created into threads[1] unjoined (#161)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_by_pointer_stopping_short_reports_missing_join.c",
             .intent = "a pointer loop stopping at threads + 1 leaves threads[1] unjoined (#161)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_by_pointer_two_slots_a_round_reports_missing_join.c",
             .intent = "a pointer loop moving two slots a round skips half the threads; the "
                       "thread left unjoined runs when main writes (#113)",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_handle_created_twice_into_one_slot_reports_missing_join.c",
             .intent = "a thread created over a slot already filled is lost to the join loop (#161)",
             .dataRace = 1, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_down_column_misses_created_slot_reports_missing_join.c",
             .intent = "a handle between a column's slots is not one the column loop joins (#161)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_past_created_slot_reports_missing_join.c",
             .intent = "a loop from threads[1] leaves the handle created into threads[0] unjoined (#161)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_pointer_loop_while_equal_joins_nothing_reports_missing_join.c",
             .intent = "a pointer loop running while equal to its end joins nothing (#161)",
             .dataRace = 1, .missingJoin = 2},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_pointer_loop_joining_after_moving_reports_missing_join.c",
             .intent = "a pointer loop joining after it moves joins the next slot, not its own (#161)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_pointer_loop_joining_through_other_cursor_reports_missing_join.c",
             .intent = "a join through another local than the cursor joins no slot of the walk; "
                       "the thread left unjoined runs when main writes (#113)",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_handle_array_reached_through_escaping_local_reports_missing_join.c",
             .intent = "the handle array kept in a local whose address escapes may be changed; "
                       "the thread left unjoined runs when main writes (#113)",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_handle_array_handed_to_thread_through_local_reports_missing_join.c",
             .intent = "the handle array read back from a local and handed to a thread may be "
                       "changed; the thread left unjoined runs when main writes (#113)",
             .dataRace = 2, .missingJoin = 1},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_array_joined_by_range_for_no_fp.cpp",
             .intent = "a range-for joining a std::thread array ends every thread (#161)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_array_joined_by_index_loop_no_fp.cpp",
             .intent = "an index loop joining a std::thread array ends every thread (#161)",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_array_written_before_range_for_join_race.cpp",
             .intent = "main's write before the range-for joins races with the threads (#161)",
             .dataRace = 1, .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_join_loop_up_to_count_of_created_handles_no_fp.c",
             .intent = "a loop up to the count of created handles ends every worker (#157)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_join_loop_up_to_count_ignoring_failure.c",
             .intent = "the same loop going on past a failed join ends none (#157)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_past_first_spawn_slot_reports_missing_join.c",
             .intent = "a join loop over [1, 4) leaves the first slot of [0, 2) unjoined (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_join_loop_over_consecutive_spawn_loops_breaking_early.c",
             .intent = "a join loop over the union that may break early joins none of it (#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_overlapping_spawn_loops_report_missing_join.c",
             .intent = "a second spawn loop over [1, 3) loses the first one's thread in slot 1 "
                       "(#151)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/"
                     "pthread_spawn_loop_raising_counter_first_reports_missing_join.c",
             .intent = "a counter raised before the spawn reads it fills the slots one further "
                       "on (#151)",
             .missingJoin = 1},

            // --- imported missing-join fixtures (Nihil, 91e7431; #4) ---
            {.path = "tests/fixtures/concurrency/missing-join/cpp_missing_join_scope_exit.cpp",
             .intent = "an early return abandons a joinable C++ thread",
             .missingJoin = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_early_return.c",
             .intent = "the error return bypasses the pthread join",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/missing-join/missing_join_overwritten_handle.c",
             .intent = "reusing one handle loses earlier threads; unresolved array indices also report a race",
             .dataRace = 1,
             .missingJoin = 1},

            // --- signal handlers -----------------------------------------------------------
            {.path = "tests/fixtures/concurrency/signal/signal_handler_unsafe_call.c",
             .intent = "the handler prints and allocates, both of which it may interrupt",
             .unsafeSignalHandler = 1},
            {.path = "tests/fixtures/concurrency/signal/signal_handler_unsafe_via_helper.c",
             .intent = "a handler is no safer than the helper it calls",
             .unsafeSignalHandler = 1},
            {.path = "tests/fixtures/concurrency/signal/signal_handler_flag_only_no_fp.c",
             .intent = "writing a volatile sig_atomic_t flag is the shape the standard allows"},
            {.path = "tests/fixtures/concurrency/signal/signal_handler_strtok_r_no_fp.c",
             .intent = "strtok_r keeps its position in caller-owned state and is async-signal-safe"},

            // --- imported signal-handler fixtures (Nihil, 91e7431; #4) ---
            {.path = "tests/fixtures/concurrency/signal-handler/signal_handler_data_race_global.c",
             .intent = "handler printf is unsafe; global signal-state modeling remains outside this rule (#54)",
             .unsafeSignalHandler = 1},
            {.path = "tests/fixtures/concurrency/signal-handler/signal_handler_longjmp.c",
             .intent = "siglongjmp is not blanket-unsafe; interrupted-operation restrictions are not modeled (#54)"},
            {.path = "tests/fixtures/concurrency/signal-handler/signal_handler_non_safe_func.c",
             .intent = "unsafe allocation and stdio calls produce one aggregated handler diagnostic",
             .unsafeSignalHandler = 1},
            {.path = "tests/fixtures/concurrency/signal-handler/signal_mutex_deadlock.c",
             .intent = "mutex operations are not async-signal-safe",
             .unsafeSignalHandler = 1},
            {.path = "tests/fixtures/concurrency/signal-handler/signal_reentrant_function.c",
             .intent = "strtok resumes from hidden state the interrupted scan is still using",
             .unsafeSignalHandler = 1},
            {.path = "tests/fixtures/concurrency/signal-handler/signal_thread_mask.c",
             .intent = "main writes admin_state while the workers read it (#113); signal "
                       "delivery is not modeled (#54)",
             .dataRace = 1},

            // --- thread arguments ----------------------------------------------------------
            {.path = "tests/fixtures/concurrency/thread-escape/thread_argument_stack_escape.c",
             .intent = "a detached thread keeps reading a frame that is already gone",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/thread_argument_joined_no_fp.c",
             .intent = "the join keeps the frame alive at least as long as the thread"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_static_storage_no_fp.c",
             .intent = "the argument outlives every frame, so detaching is fine"},
            {.path = "tests/fixtures/concurrency/thread-escape/thread_argument_join_loop_no_fp.c",
             .intent = "a join loop over the whole creation range keeps the frame alive"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_joined_by_helper_no_fp.c",
             .intent = "a helper joining the handle it is given on every path waits like a join (#89)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_joined_by_nested_helper_no_fp.c",
             .intent = "a helper calling a joining helper on every path joins too (#89)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_joined_by_helper_loop_no_fp.c",
             .intent = "a join loop through a helper over the whole creation range (#89)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_join_loop_over_consecutive_spawn_loops_no_fp.c",
             .intent = "a join loop over the union of two spawn ranges ends every worker (#151)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_join_loop_over_consecutive_spawn_loops_ignoring_failure.c",
             .intent = "the same loop going on past a failed join joins every worker but ends "
                       "none (#151)",
             .threadArgumentEscape = 2},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_helper_joins_one_branch.c",
             .intent = "a helper joining on one branch only cannot stand for a join (#89)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_helper_joins_other_handle.c",
             .intent = "a helper joining another parameter does not join this thread (#89)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/thread_argument_partial_join_loop.c",
             .intent = "a join loop one short of the creation range leaves a worker reading a gone frame",
             .missingJoin = 1,
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_returned_after_failed_join_in_branch.c",
             .intent = "a join every return passes has not kept the frame alive where it failed (#121)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_aborts_on_failed_join_in_branch_no_fp.c",
             .intent = "past a join's success on every return the frame outlives the thread (#121)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_joined_by_status_helper_no_fp.c",
             .intent = "a helper's join status tested before every return keeps the frame alive (#121)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_returned_after_status_helper_failure.c",
             .intent = "a helper reporting a failed join has not kept the frame alive (#121)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_returned_after_failed_join.c",
             .intent = "a join that failed has not kept the frame alive (#122)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_returned_after_failed_local_join.c",
             .intent = "a join of a local handle that failed has not kept the frame alive (#122)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_aborts_on_failed_join_no_fp.c",
             .intent = "returning only past a successful join keeps the frame alive (#122)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_returned_without_join.c",
             .intent = "returning without a join leaves the thread reading a gone frame (#122)",
             .dataRace = 1, .missingJoin = 1, .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_joined_on_both_branches_no_fp.c",
             .intent = "joins on both branches keep the frame alive though neither dominates the "
                       "return (#122); the missing join is #143, the race is gone (#113)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/thread_argument_joined_once_no_fp.c",
             .intent = "one join before the return keeps the frame alive (#122)"},
            {.path = "tests/fixtures/concurrency/thread-escape/"
                     "thread_argument_joined_on_one_branch.c",
             .intent = "a join on one branch leaves the other returning while the thread runs "
                       "(#122)",
             .dataRace = 1, .missingJoin = 1, .threadArgumentEscape = 1},

            {.path = "tests/fixtures/concurrency/thread-escape/std_thread_ref_capture_joined_no_fp.cpp",
             .intent = "a by-reference capture is safe when the std::thread is joined before return"},
            {.path = "tests/fixtures/concurrency/thread-escape/std_thread_value_capture_detached_no_fp.cpp",
             .intent = "a by-value capture leaves nothing pointing into the frame"},
            {.path = "tests/fixtures/concurrency/thread-escape/std_thread_joined_by_helper_no_fp.cpp",
             .intent = "a helper joining the std::thread it is given by reference waits like a join (#89)"},
            {.path = "tests/fixtures/concurrency/thread-escape/std_thread_helper_detaches.cpp",
             .intent = "a helper given the std::thread by reference that detaches it joins nothing (#89)",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/std_thread_pointer_arg_detached.cpp",
             .intent = "a detached std::thread handed a pointer to a local outlives the frame",
             .threadArgumentEscape = 1},

            // --- imported thread-escape fixtures (Nihil, 91e7431; #4) ---
            {.path = "tests/fixtures/concurrency/thread-escape/thread_escape_loop_variable.c",
             .intent = "workers receive the loop-counter address; with the counter escaped, no join range is provable, and the loop's increment races with their reads",
             .dataRace = 1,
             .missingJoin = 1,
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-escape/thread_escape_stack_ptr.c",
             .intent = "a global handle survives the helper frame whose local it references",
             .threadArgumentEscape = 1},

            // --- process lifecycle ---------------------------------------------------------
            {.path = "tests/fixtures/concurrency/process/fork_after_thread_creation.c",
             .intent = "the child inherits a mutex no surviving thread can unlock",
             // The child returns without joining, which is right: its copy of the handle names
             // a thread that does not exist there. The join rule sees the path all the same.
             .missingJoin = 1, .forkAfterThread = 1, .unreapedChild = 1},
            {.path = "tests/fixtures/concurrency/process/fork_without_wait.c",
             .intent = "the pid is dropped, so every finished child stays in the table",
             .unreapedChild = 1},
            {.path = "tests/fixtures/concurrency/process/fork_then_exec_no_fp.c",
             .intent = "the child replaces its image at once and keeps nothing it inherited",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/process/fork_sigchld_ignored_no_fp.c",
             .intent = "handing SIGCHLD to SIG_IGN lets the system reap, so nothing is owed"},
            {.path = "tests/fixtures/concurrency/process/fork_reaped_no_fp.c",
             .intent = "no thread, and every child is collected"},

            // --- condition variables ------------------------------------------------------
            // The state around a condition variable is ordinary shared data, so the data-race
            // rule applies to it as well; the wait protocol itself is a separate question.
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "condition_variable_unprotected_predicate.c",
             .intent = "the predicate is published without the mutex the waiter holds",
             .dataRace = 2,
             .racingSymbol = "data_ready"},
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "condition_variable_predicate_loop_no_diagnostic.c",
             .intent = "rechecking the predicate in a loop is the correct protocol"},
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "cpp_condition_wait_predicate_no_fp.cpp",
             .intent = "every safe C++ spelling of a wait, including the bare one inside a loop",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "cpp_condition_wait_without_predicate.cpp",
             .intent = "wait and wait_for that read waking up as proof the condition holds",
             .conditionWait = 2,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/condition-variable/"
                     "cpp_condition_wait_through_helper.cpp",
             .intent = "the loop belongs to the caller of a forwarding helper, not to the helper",
             .conditionWait = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/condition-variable/condition_variable_spurious.c",
             .intent = "checking the predicate with if instead of while leaves a wake-up "
                       "unverified",
             .conditionWait = 1},

            // --- imported condition-variable fixtures (Nihil, 91e7431; #4) ---
            {.path = "tests/fixtures/concurrency/condition-variable/condition_variable_destructor_race.cpp",
             .intent = "resetting the shared condition-variable pointer races; destruction safety is not modeled",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/condition-variable/condition_variable_lost_wakeup.c",
             .intent = "waiting without a predicate can lose a notification",
             .conditionWait = 1},
            {.path = "tests/fixtures/concurrency/condition-variable/condition_variable_no_predicate_recheck.c",
             .intent = "broadcast consumers must recheck the predicate after waking",
             .conditionWait = 1},

            // --- atomic publication (#47) ---
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_release_acquire_no_fp.cpp",
             .intent = "a release store observed by an acquire load orders the payload"},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_int_flag_no_fp.cpp",
             .intent = "an integer flag publishes as a boolean one, whatever the library inlines"},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_fences_no_fp.c",
             .intent = "release and acquire fences around relaxed flag operations order the payload"},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_relaxed_nonatomic.cpp",
             .intent = "a relaxed flag orders nothing: the plain payload races, and the flag does not publish it",
             .dataRace = 1,
             .weakPublication = 1},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_relaxed_store.cpp",
             .intent = "a relaxed store publishes nothing even to an acquiring reader",
             .weakPublication = 1},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_relaxed_load.c",
             .intent = "a relaxed observing load with no acquire fence is not ordered after the release",
             .weakPublication = 1},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_seq_cst_default_no_fp.cpp",
             .intent = "atomic assignment and conversion are sequentially consistent and publish"},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_write_after_flag.cpp",
             .intent = "a payload written after the release store is not published by it",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_inverted_guard.cpp",
             .intent = "a read guarded by the flag still being unset observed no publication",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/atomic-ordering/publish_two_writers.cpp",
             .intent = "with a second writer, observing the flag does not prove whose store was read",
             .dataRace = 1},

            // --- imported atomic-ordering fixtures (Nihil, 91e7431; #47) ---
            {.path = "tests/fixtures/concurrency/atomic-ordering/cpp_aba_compare_exchange.cpp",
             .intent = "a value CAS on an int has value semantics; ABA needs reused identity (#47)"},
            {.path = "tests/fixtures/concurrency/atomic-ordering/cpp_relaxed_ordering_publish.cpp",
             .intent = "a relaxed flag does not publish its atomic payload",
             .weakPublication = 1},
            {.path = "tests/fixtures/concurrency/atomic-ordering/seqlock_missing_fence.cpp",
             .intent = "the non-atomic payload races whatever the sequence ordering; validated reads are outside #47",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/atomic-ordering/store_load_reorder.cpp",
             .intent = "relaxed Dekker lets both threads race on shared; store-load ordering is outside #47",
             .dataRace = 3},

            // --- once initialization (#48) ---
            {.path = "tests/fixtures/concurrency/once-init/magic_static_no_fp.cpp",
             .intent = "a function-local static is initialized once, before every thread reads it"},
            {.path = "tests/fixtures/concurrency/once-init/magic_static_mutated_after_init.cpp",
             .intent = "safe initialization does not protect what threads do with the object afterwards",
             .dataRace = 1,
             .racingSymbol = "_ZZL11next_ticketvE8instance"},
            {.path = "tests/fixtures/concurrency/once-init/pthread_once_no_fp.c",
             .intent = "pthread_once initializes once; the callback is not followed, so nothing pairs its writes"},
            {.path = "tests/fixtures/concurrency/once-init/call_once_no_fp.cpp",
             .intent = "std::call_once initializes once; the callable is not followed, so nothing pairs its writes"},

            // --- imported once-init fixtures (Nihil, 91e7431; #48) ---
            {.path = "tests/fixtures/concurrency/once-init/cpp_once_init_static_side_effects.cpp",
             .intent = "workers race on registry_count; initializing the local static is thread-safe",
             .dataRace = 1,
             .racingSymbol = "_ZL14registry_count"},
            {.path = "tests/fixtures/concurrency/once-init/once_init_dclp_no_barrier.c",
             .intent = "the unlocked first check races with the locked write of the pointer",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/once-init/once_init_double_checked_locking.c",
             .intent = "volatile is not atomic: the unlocked check races with the locked write",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/once-init/once_init_manual_flag_race.c",
             .intent = "a plain flag guards nothing: the flag, the pointer and the size race",
             .dataRace = 3},

            // --- imported use-after-free fixtures (Nihil, 91e7431; #49) ---
            {.path = "tests/fixtures/concurrency/use-after-free/cpp_detached_thread_dangling_ref.cpp",
             .intent = "a detached std::thread keeps a reference into the frame that created it",
             .threadArgumentEscape = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/use_after_free_callback.c",
             .intent = "the creator frees the thread's argument before joining the thread",
             .threadArgumentFreed = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/heap_arg_joined_then_freed_no_fp.c",
             .intent = "freeing after the join leaves the thread done with the memory"},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_joined_by_helper_then_freed_no_fp.c",
             .intent = "freeing after a helper has joined the thread (#89)"},
            {.path = "tests/fixtures/concurrency/use-after-free/heap_arg_freed_unused_no_fp.c",
             .intent = "a thread that never touches its argument cannot use it after the free"},
            {.path = "tests/fixtures/concurrency/use-after-free/heap_arg_reassigned_then_freed_no_fp.c",
             .intent = "the freed pointer is another allocation than the one the thread was given"},
            {.path = "tests/fixtures/concurrency/use-after-free/heap_arg_freed_after_failed_join.c",
             .intent = "a free on the branch where the join failed comes too early (#122)",
             .threadArgumentFreed = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_freed_after_failed_local_join.c",
             .intent = "the same with a local handle, which the completion proof covers (#122)",
             .threadArgumentFreed = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_freed_after_successful_join_no_fp.c",
             .intent = "a free on the branch where the join succeeded comes after the thread (#122)"},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_joined_on_both_branches_then_freed_no_fp.c",
             .intent = "joins on both branches precede the free though neither dominates it "
                       "(#122); the missing join is #143, the race is gone (#113)",
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_joined_once_then_freed_no_fp.c",
             .intent = "one join precedes the free (#122)"},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_joined_on_one_branch_then_freed.c",
             .intent = "a join on one branch leaves the other freeing while the thread runs (#122)",
             .dataRace = 1, .missingJoin = 1, .threadArgumentFreed = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/"
                     "heap_arg_freed_before_join_after_branch.c",
             .intent = "a free before the join, past a branch, comes too early (#122)",
             .threadArgumentFreed = 1},
            {.path = "tests/fixtures/concurrency/use-after-free/use_after_free_concurrent.c",
             .intent = "freeing through a shared pointer another thread reads races on the pointer",
             .dataRace = 1,
             .racingSymbol = "shared_task"},

            // --- thread-local lifetime (#50) ---
            {.path = "tests/fixtures/concurrency/thread-local/tls_destructor_only_race.cpp",
             .intent = "thread-local destructors run at the end of their threads and race on a plain counter",
             .dataRace = 1,
             .racingSymbol = "_ZL8finished"},
            {.path = "tests/fixtures/concurrency/thread-local/tls_destructor_atomic_no_fp.cpp",
             .intent = "thread-local destructors updating an atomic counter do not race"},
            {.path = "tests/fixtures/concurrency/thread-local/tls_private_no_fp.c",
             .intent = "each thread updates its own thread-local copy"},
            {.path = "tests/fixtures/concurrency/thread-local/tls_published_while_alive_no_fp.c",
             .intent = "a thread-local address read while its owner still runs"},
            {.path = "tests/fixtures/concurrency/thread-local/tls_read_after_helper_join.c",
             .intent = "a helper's join ends the thread, and its thread-local with it (#89)",
             .threadLocalEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-local/tls_pointer_reassigned_no_fp.c",
             .intent = "a pointer that also receives heap memory is not known to reach a dead thread-local"},
            {.path = "tests/fixtures/concurrency/thread-local/"
                     "tls_read_after_joins_on_both_branches.c",
             .intent = "joins on both branches end the thread and its thread-local (#122); the "
                       "missing join is #143, the race is gone (#113)",
             .missingJoin = 1, .threadLocalEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-local/tls_read_after_failed_join_race.c",
             .intent = "where the join failed the thread and its thread-local may still live "
                       "(#122)",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/thread-local/tls_read_after_successful_join.c",
             .intent = "where the join succeeded the thread-local has ended with its thread (#122)",
             .threadLocalEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-local/"
                     "tls_read_before_thread_started_no_fp.c",
             .intent = "a read before the thread starts is not after its end (#122)"},

            // --- imported thread-local fixtures (Nihil, 91e7431; #50) ---
            {.path = "tests/fixtures/concurrency/thread-local/c_tls_dangling_ptr.c",
             .intent = "main reads and writes a thread-local through the address its joined thread published",
             .threadLocalEscape = 1},
            {.path = "tests/fixtures/concurrency/thread-local/cpp_tls_destructor_race.cpp",
             .intent = "thread-local constructors and main race on active_count",
             .dataRace = 1,
             .racingSymbol = "_ZL12active_count"},

            // --- rules that do not exist yet: pinned as silent -----------------------------
            {.path = "tests/fixtures/concurrency/memory-barrier/missing_memory_barrier.c",
             .intent = "no rule models memory ordering yet; the plain accesses still race",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/thread-escape/fork_thread_race.c",
             .intent = "forking while a thread runs, never collecting the child, and racing on "
                       "the global besides",
             .dataRace = 1, .forkAfterThread = 1, .unreapedChild = 1},
            {.path = "tests/fixtures/concurrency/data-race/cpp_race_std_async.cpp",
             // The race is real — the fixture exists for it — and whether it is seen depends on
             // how the standard library implements std::async. libstdc++ builds it on a
             // std::thread the analyzer recognizes, so the race surfaces there; libc++ does not.
             // Pinned as a minimum rather than split by platform, since neither answer is wrong.
             .intent = "std::async is only reachable through the thread some libraries build it "
                       "on",
             .countsAreMinimums = true},

            // --- contained thread phases (#39) ---
            {.path = "tests/fixtures/concurrency/data-race/data_race_seq_join_inside_helper.c",
             .intent = "two calls bind different entries and each joins before returning"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_seq_template_two_phases.cpp",
             .intent = "template trampoline entries are contained by their helper calls",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/data_race_joined_loop_phases.c",
             .intent = "join every array element before the next phase; retain both intra-phase races",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/data_race_joined_vector_phases.cpp",
             .intent = "join a fixed-size vector range before the next phase; retain intra-phase races",
             .dataRace = 2,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/data_race_unjoined_helper_phases.c",
             .intent = "a helper that returns with a live thread cannot sequence its callers",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_detached_helper_phases.c",
             .intent = "detach resolves ownership but does not complete the worker",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_conditional_helper_join.c",
             .intent = "one unjoined path prevents a contained-entry summary",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_incomplete_join_loop.c",
             .intent = "a shortened join range leaves a worker concurrent with the next phase; "
                       "main's final read races with the workers left running (#113)",
             .dataRace = 4,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_conditional_join_loop.c",
             .intent = "a conditional join does not discharge every array element; main's final "
                       "read races with the workers left running (#113)",
             .dataRace = 4,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_overlapping_helper_context.c",
             .intent = "one sequential context cannot hide another live instance of the same entry",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_nested_joined_helpers.c",
             .intent = "entry bindings and completion propagate through nested helpers"},
            {.path = "tests/fixtures/concurrency/data-race/data_race_early_break_join_loop.c",
             .intent = "an early break prevents full join-range coverage; main's final read races "
                       "with the workers left running (#113)",
             .dataRace = 4,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_replaced_handle_loop.c",
             .intent = "overwriting one handle invalidates an otherwise matching join range; "
                       "main's final read races with the workers left running (#113)",
             .dataRace = 4,
             .missingJoin = 1},

            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_handle_mutation.c",
             .intent = "an opaque call can replace the handle before its join; containment is unknown",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/data_race_helper_dynamic_handle_index.c",
             .intent = "wildcard storage-group indices do not prove the same handle was joined",
             .dataRace = 1},

            // --- a std::thread moved into a vector and joined through it (#162) ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_joined_no_fp.cpp",
             .intent = "the loop joining a vector ends the thread push_back moved into it",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_thread_joined_no_fp.cpp",
             .intent = "a loop over a vector member of a local object ends its threads",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_after_other_member_joined_no_fp.cpp",
             .intent = "a vector member after another member is ended by its join loop",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_after_other_vector_member_joined_no_fp.cpp",
             .intent = "another vector member's uses leave the joined thread vector alone",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_threads_joined_race.cpp",
             .intent = "threads of a joined vector member still race with each other",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_thread_written_before_join_race.cpp",
             .intent = "main's write before the member's join loop races with its thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_thread_other_member_joined_race.cpp",
             .intent = "a loop joining the other vector member ends none of this one's threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_thread_taken_out_by_method_race.cpp",
             .intent = "a method taking the thread out of the member leaves it running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_thread_taken_out_through_returned_object_race.cpp",
             .intent = "a thread taken out through the object a method returns stays running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_joined_through_arrow_no_fp.cpp",
             .intent = "an iterator loop calling it->join() ends the threads of the vector",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_written_before_arrow_join_race.cpp",
             .intent = "main's write before the it->join() loop races with the thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_through_arrow_race.cpp",
             .intent = "threads joined through it->join() still race with each other",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_partly_joined_through_arrow_race.cpp",
             .intent = "an it->join() loop from begin() + 1 leaves the first thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_arrow_join_failure_caught_race.cpp",
             .intent = "a round going on past a failed it->join() leaves its thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_other_vector_joined_through_arrow_race.cpp",
             .intent = "an it->join() loop over another vector ends none of this one's threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_below_constant_no_fp.cpp",
             .intent = "a loop joining below the count of threads moved in ends them all",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_below_constant_race.cpp",
             .intent = "threads joined below their count still race with each other",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_written_before_join_below_constant_race.cpp",
             .intent = "main's write before the join below a constant races with the threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_below_smaller_constant_race.cpp",
             .intent = "a join loop below fewer than the threads moved in leaves one running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_appended_past_counted_loop_race.cpp",
             .intent = "a thread moved in past the counted loop is not joined below its count",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_join_below_constant_failure_caught_race.cpp",
             .intent = "a round going on past a failed join below a constant leaves its thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_built_from_other_vector_joined_below_constant_race.cpp",
             .intent = "a vector built from another holds more than its loop moves in",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_inserted_in_nested_loop_joined_below_constant_race.cpp",
             .intent = "a counted loop run twice moves in twice its count",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_joined_in_batches_no_fp.cpp",
             .intent = "each round of a loop joins the vector it fills before the next one",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_joined_and_cleared_in_batches_no_fp.cpp",
             .intent = "one vector filled, joined and cleared each round ends its threads",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_in_batches_race.cpp",
             .intent = "the threads of one round still race with each other",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_written_before_batch_join_race.cpp",
             .intent = "main's write before a round's join races with the round's thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_inserted_after_batch_join_race.cpp",
             .intent = "a thread moved in after a round's join loop runs past the last round",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_appended_during_join_loop_below_read_size_race.cpp",
             .intent = "threads appended past a size read before the join loop are not joined",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_joined_through_back_no_fp.cpp",
             .intent = "joining back() ends the only thread moved into the vector",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_joined_through_front_no_fp.cpp",
             .intent = "joining front() ends the only thread moved into the vector",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_back_joined_after_second_race.cpp",
             .intent = "back() joins the idle second thread moved in, not the first",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_front_joined_after_second_race.cpp",
             .intent = "front() joins the idle first thread moved in, not the second",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_written_before_back_join_race.cpp",
             .intent = "main's write before joining back() races with its thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_swapped_out_before_back_join_race.cpp",
             .intent = "a thread swapped out of back() is not the one its join ends",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_back_join_failure_caught_race.cpp",
             .intent = "going on past a failed join of back() leaves its thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_front_of_moved_in_vector_joined_race.cpp",
             .intent = "front() of a vector built from another is not the thread moved in",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_back_kept_swapped_before_join_race.cpp",
             .intent = "the element back() reads, swapped before its join, ends another thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_back_insertion_failure_caught_race.cpp",
             .intent = "a push_back that throws leaves back() on the thread moved in before",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_emplaced_temporary_joined_no_fp.cpp",
             .intent = "the loop joining a vector ends the thread emplace_back moved into it",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_local_moved_in_joined_no_fp.cpp",
             .intent = "a local moved into the vector with std::move is ended by its loop",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_index_joined_no_fp.cpp",
             .intent = "an index loop below the size ends the threads moved into the vector",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_phases_no_fp.cpp",
             .intent = "a phase whose thread is joined through its vector precedes the next",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_written_before_join_race.cpp",
             .intent = "the thread moved into the vector runs until the loop joins it",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_partly_joined_race.cpp",
             .intent = "an index loop from 1 does not end the thread moved in at 0",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_detached_race.cpp",
             .intent = "a loop detaching the vector's threads ends none of them",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_join_failure_caught_race.cpp",
             .intent = "a join whose exception the round catches ends nothing past the loop",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_out_again_race.cpp",
             .intent = "a thread moved out of the vector again escapes its join loop",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_swapped_out_race.cpp",
             .intent = "a thread swapped out of the vector escapes its join loop",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_other_vector_joined_race.cpp",
             .intent = "a loop joining another vector ends none of this one's threads",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_on_one_path_race.cpp",
             .intent = "a thread moved into the vector on one path only is not ended by it",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_moved_in_loop_joined_race.cpp",
             .intent = "threads moved into the vector in a loop race with each other only",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_swapped_before_insertion_race.cpp",
             .intent = "a local whose thread was swapped out moves another thread in",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_insertion_failure_caught_race.cpp",
             .intent = "a push_back whose exception is caught may move nothing in",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_after_join_loop_race.cpp",
             .intent = "a thread moved in after the join loop is not ended by it",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_swapped_in_join_loop_race.cpp",
             .intent = "an element swapped before its join is not the thread moved in",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_into_derived_container_race.cpp",
             .intent = "a push_back of the program's own, hiding std::vector's, inserts nothing",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_into_vector_of_other_namespace_race.cpp",
             .intent = "a vector outside std, whose push_back detaches, inserts nothing",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_joined_below_other_bound_race.cpp",
             .intent = "an index loop below a bound other than the size may stop short",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_joined_below_helper_bound_race.cpp",
             .intent = "an index loop below a helper's count other than the size may stop short",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_aliased_parameter_race.cpp",
             .intent = "a vector the function does not own may lose its threads",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_exchanged_by_helper_race.cpp",
             .intent = "a helper swapping the local's thread out before push_back hides the writer",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_moved_in_join_loop_break_race.cpp",
             .intent = "a join loop that may break ends nothing past it",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_swapped_out_by_block_laid_out_after_insertion_race.cpp",
             .intent = "a swap laid out after push_back but run before it is a second taker",
             .dataRace = 1,
             .requiresCxx20 = true},
            // --- a vector of std::thread handed to a function joining every thread of it ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_by_helper_no_fp.cpp",
             .intent = "a helper joining every thread of the vector handed to it ends them",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_by_destructor_no_fp.cpp",
             .intent = "the destructor of the object holding the vector ends its threads",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_written_before_helper_join_race.cpp",
             .intent = "main's write before the helper's join races with the threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_inserted_after_helper_join_race.cpp",
             .intent = "a thread moved in after the helper's join is not joined by it",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_taken_out_by_helper_race.cpp",
             .intent = "a thread the helper takes out before its loop is not joined",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_taken_out_through_aliased_parameter_race.cpp",
             .intent = "a helper taking a thread out through a second reference to the vector",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_helper_join_failure_caught_race.cpp",
             .intent = "a helper going on past a failed join leaves its thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_helper_joins_on_one_path_race.cpp",
             .intent = "a helper returning early on one path joins nothing there",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_member_threads_joined_by_destructor_no_fp.cpp",
             .intent = "a destructor joining a vector lying past another member ends its threads",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_in_member_detached_by_destructor_race.cpp",
             .intent = "a destructor joining one member vector ends none of the other's threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_taken_out_before_helper_join_race.cpp",
             .intent = "a thread taken out of the vector before the helper's join is not joined",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_helper_joins_below_constant_race.cpp",
             .intent = "a helper joining below a constant may leave threads of the vector",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_started_after_helper_join_in_loop_race.cpp",
             .intent = "a thread started after the helper's join in each round outlives the loop",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_started_after_helper_join_in_round_race.cpp",
             .intent = "a thread started after the helper's join runs during its round",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_started_after_helper_join_in_round_race.cpp",
             .intent = "two threads started after the helper's join run together in their round",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_insertion_failure_caught_before_helper_join_race.cpp",
             .intent = "a push_back that throws leaves its thread out of the helper's join",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            // --- a vector's whole range handed to a function joining each element of it ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_by_for_each_no_fp.cpp",
             .intent = "std::for_each with a joining lambda over the whole vector ends its threads",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_by_range_template_no_fp.cpp",
             .intent = "a template joining each element of the whole range it is handed ends them",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_joined_by_helper_for_each_no_fp.cpp",
             .intent = "a helper handing its vector's whole range to std::for_each ends its threads",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_written_before_for_each_join_race.cpp",
             .intent = "main's write before std::for_each joins the threads races with them",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_for_each_over_first_only_race.cpp",
             .intent = "std::for_each over the first element only leaves the second thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_for_each_over_other_vector_race.cpp",
             .intent = "std::for_each over another vector ends none of this one's threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_for_each_over_empty_range_race.cpp",
             .intent = "std::for_each from begin() to begin() joins none of the threads",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_for_each_join_failure_caught_race.cpp",
             .intent = "a lambda going on past a failed join leaves its thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_range_template_skipping_first_race.cpp",
             .intent = "a template stepping past the first element leaves its thread running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_range_template_returning_early_race.cpp",
             .intent = "a template returning early on one path joins nothing there",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_swapped_through_iterator_copy_race.cpp",
             .intent = "a template swapping a thread out through a copy of its iterator",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_swapped_through_begin_before_range_join_race.cpp",
             .intent = "a thread swapped out through begin() before the range is joined",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_swapped_through_vector_handed_with_range_race.cpp",
             .intent = "a function handed the vector with its range swaps a thread out",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            // --- a vector filled by a function with every thread it leaves running ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_added_by_helper_joined_no_fp.cpp",
             .intent = "the loop joining the vector ends the threads a helper moved into it",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_returned_by_function_joined_no_fp.cpp",
             .intent = "the loop joining a returned vector ends the threads its function started",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_added_through_nested_helper_joined_no_fp.cpp",
             .intent = "the loop ends the threads a helper's helper moved into the vector",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_added_by_helper_joined_by_helper_no_fp.cpp",
             .intent = "a helper joining the vector ends the threads another helper moved in",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_written_before_join_of_added_race.cpp",
             .intent = "main's write before joining the threads a helper moved in races",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_added_after_join_loop_race.cpp",
             .intent = "a thread a helper moves in after the join loop still runs",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_also_detaches_race.cpp",
             .intent = "a helper detaching a thread besides those it moves in leaves it running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_adds_to_other_vector_race.cpp",
             .intent = "a helper filling another vector than the one joined leaves its thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_takes_thread_out_race.cpp",
             .intent = "a helper swapping its thread out of the vector leaves it running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_added_by_helper_joined_below_constant_race.cpp",
             .intent = "a join loop below a constant may miss threads a helper moved in",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_added_by_helper_front_joined_race.cpp",
             .intent = "front() joins one of the threads a helper moved in, not the other",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_detaches_on_one_path_race.cpp",
             .intent = "a helper detaching its thread on one path leaves it out of the vector",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_calls_detaching_starter_race.cpp",
             .intent = "a helper calling a function that starts a detached thread leaves it",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_fills_two_vectors_race.cpp",
             .intent = "a helper filling two vectors leaves a thread in the one not joined",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_helper_moves_thread_out_race.cpp",
             .intent = "a helper moving its thread back out of the vector leaves it running",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_threads_counted_and_added_joined_below_constant_race.cpp",
             .intent = "a counted loop and a helper both fill the vector a constant bounds",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_vector_thread_added_then_taken_through_aliased_parameter_race.cpp",
             .intent = "a thread added through one reference and taken through another",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},
            // --- a std::thread move-assigned into an array slot and joined through it (#162) ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_joined_no_fp.cpp",
             .intent = "the range-for joining an array ends the thread move-assigned into its slot",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_in_loop_joined_no_fp.cpp",
             .intent = "a range-for ends each thread a loop move-assigned into its own slot",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_up_to_bound_joined_no_fp.cpp",
             .intent = "a loop below a bound ends each thread a loop below it move-assigned",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_at_count_joined_no_fp.cpp",
             .intent = "a loop up to the count ends the threads move-assigned at that count",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_two_slots_move_assigned_no_fp.cpp",
             .intent = "threads move-assigned into two slots are both ended by the range-for",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_written_before_join_race.cpp",
             .intent = "the thread move-assigned into the array runs until the loop joins it",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_slot_not_joined_race.cpp",
             .intent = "a loop joining slot 0 alone does not end the thread moved into slot 1",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_move_assigned_swapped_out_race.cpp",
             .intent = "a thread swapped out of the array escapes its join loop",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "cpp_thread_array_kept_by_helper_before_move_race.cpp",
             .intent = "an array whose address a helper keeps may lose its thread later",
             .dataRace = 1,
             .requiresCxx20 = true},
            // --- each thread of a spawn loop gets an element of its own (#108) ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_local_array_no_fp.c",
             .intent = "each thread writes only the element of a local array it was handed"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_global_array_no_fp.c",
             .intent = "each thread writes only the element of a global array it was handed"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_read_modify_write_no_fp.c",
             .intent = "each thread reads and writes only its own element"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_struct_fields_no_fp.c",
             .intent = "main fills a field of the element before the spawn; the thread uses "
                       "its own"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_written_by_helper_no_fp.c",
             .intent = "the thread's helper writes the element the thread was handed"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_while_loop_no_fp.c",
             .intent = "a while loop raising its counter last hands each thread its own element"},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_per_thread_element_no_fp.cpp",
             .intent = "each std::thread writes only the element it was handed",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_constant_index_race.c",
             .intent = "every thread is handed the same element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_half_index_race.c",
             .intent = "threads 2k and 2k + 1 are handed the same element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_main_writes_after_spawn_race.c",
             .intent = "main writes the element of the thread it just started",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_previous_after_spawn_race.c",
             .intent = "main writes the previous thread's element after a spawn",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_whole_array_to_other_thread_race.c",
             .intent = "a thread handed the whole array writes every element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_alias_pointer_in_main_race.c",
             .intent = "main writes an element through a pointer it kept",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_union_neighbour_race.c",
             .intent = "another thread writes a union member overlaying two elements",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_wider_write_race.c",
             .intent = "a thread writes past its 4-byte element through an 8-byte type",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_two_spawn_loops_race.c",
             .intent = "two spawn loops hand out the same elements",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_nested_rounds_race.c",
             .intent = "the spawn loop runs twice, handing every element out twice",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_nested_rounds_joined_race.c",
             .intent = "the nested rounds, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "main", .line = 19, .column = 13}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_helper_called_twice_race.c",
             .intent = "a helper holding the spawn loop is called twice",
             .dataRace = 1,
             .missingJoin = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_helper_called_twice_joined_race.c",
             .intent = "the helper called twice, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "start", .line = 17, .column = 9},
                                     {.issue = "#173", .function = "start", .line = 17, .column = 9}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_counter_rewound_race.c",
             .intent = "the counter is rewound in the loop",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_counter_rewound_through_address_race.c",
             .intent = "a callee rewinds the counter through its address",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_counter_rewound_through_pointer_race.c",
             .intent = "a pointer to the counter rewinds it in the loop",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_counter_rewound_after_increment_race.c",
             .intent = "the counter is rewound right after its increment",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_index_from_other_variable_race.c",
             .intent = "the element is picked by a variable other than the loop's counter",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_counter_every_other_round_race.c",
             .intent = "the counter advances every other round only",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_counter_recomputed_race.c",
             .intent = "the counter is recomputed from another count",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_zero_step_race.c",
             .intent = "a counter whose step is zero hands out one element twice",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_zero_step_joined_race.c",
             .intent = "step zero, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "main", .line = 19, .column = 9}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_two_entries_race.c",
             .intent = "two entries are handed the same element each round",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_same_entry_twice_race.c",
             .intent = "one entry is started twice on the same element each round",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_main_called_again_race.c",
             .intent = "main calls itself, so its spawn loop runs twice",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_extra_spawn_through_helper_race.c",
             .intent = "a helper starts the entry on an element the loop hands out",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_base_moved_race.c",
             .intent = "the base pointer moves inside the loop",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_worker_publishes_race.c",
             .intent = "a thread publishes its element's address for an observer",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_worker_publishes_locked_race.c",
             .intent = "a thread publishes its element's address under a lock",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_main_publishes_race.c",
             .intent = "main publishes the element it hands over",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_indexes_past_element_race.c",
             .intent = "a thread indexes past its element into the next one",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_helper_stashes_race.c",
             .intent = "main hands the element to a helper that stashes it",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_helper_stashes_before_loop_race.c",
             .intent = "a helper stashes an element before the loop: no thread runs at the call",
             .dataRace = 1},
            // --- the spawning thread's accesses to elements, with liveness by reach (#108) ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_main_writes_between_loops_race.c",
             .intent = "main writes an element between the spawn loop and the join loop",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_previous_before_spawn_race.c",
             .intent = "main writes the previous thread's element before a spawn",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_main_writes_through_other_index_race.c",
             .intent = "main writes an element picked by another variable than the counter",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_write_after_break_race.c",
             .intent = "main writes the element of the thread the loop broke after",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_write_after_break_joined_race.c",
             .intent = "the write after a break, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "main", .line = 20, .column = 9}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_spawn_after_increment_race.c",
             .intent = "a spawn after the increment hands out the next element",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_spawn_after_increment_joined_race.c",
             .intent = "a spawn after the increment, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "main", .line = 22, .column = 9}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_shifted_base_race.c",
             .intent = "main writes through the same array shifted by one element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_union_shifted_view_race.c",
             .intent = "main writes through a union view shifted by one element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_union_narrower_view_race.c",
             .intent = "main writes through a narrower union view of the elements",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_decreasing_wide_write_race.c",
             .intent = "with a decreasing counter, main writes into the next element",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_decreasing_wide_write_joined_race.c",
             .intent = "the decreasing wide write, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "main", .line = 20, .column = 9}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_decreasing_shifted_pointer_race.c",
             .intent = "with a decreasing counter, main writes the next element",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_decreasing_shifted_pointer_joined_race.c",
             .intent = "the decreasing shifted pointer, joined; its join loop is missed",
             .dataRace = 1,
             .trackedMissingJoins = {{.issue = "#173", .function = "main", .line = 21, .column = 9}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_reads_next_round_race.c",
             .intent = "a thread reads the element main writes in the next round",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_shared_round_race.c",
             .intent = "main writes a shared round while the previous reader runs",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_per_thread_element_ids_with_total_race.c",
             .intent = "the workers race on a total, not on their ids",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_joined_thread_element_no_fp.c",
             .intent = "the join loop reads the element of the thread it has just joined"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_next_thread_element_race.c",
             .intent = "the join loop reads the element of the next thread, still running",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_element_of_conditionally_joined_thread_race.c",
             .intent = "the join loop reads the element of a thread it skipped",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_element_after_failed_join_race.c",
             .intent = "the join loop reads an element whether or not its join succeeded",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_element_before_join_race.c",
             .intent = "the join loop reads an element before joining its thread",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_element_after_increment_race.c",
             .intent = "the join loop reads the next element after raising its counter",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_writes_past_joined_thread_element_race.c",
             .intent = "the join loop writes past the joined element into a running thread's",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_writes_across_joined_thread_element_race.c",
             .intent = "the join loop writes across the end of the joined element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_join_loop_reads_element_of_handle_counted_apart_race.c",
             .intent = "handles counted apart from the elements do not tie a slot to an element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_sets_element_before_spawn_no_fp.c",
             .intent = "a helper writes the element before the round's spawn hands it over"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_sets_element_after_spawn_race.c",
             .intent = "a helper writes the element after its thread started",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_sets_previous_element_race.c",
             .intent = "a helper writes the previous thread's element",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_writes_before_its_element_race.c",
             .intent = "a helper handed an element also writes the one before it",
             .dataRace = 1},
            // --- a thread runs wherever its start reaches without a certain end (#113) ---
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_on_one_branch_by_callee_race.c",
             .intent = "a callee starting a thread on one branch leaves it running",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_call_on_one_branch_race.c",
             .intent = "a call made on one branch leaves its thread running",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_callee_early_return_after_spawn_race.c",
             .intent = "a callee returning early after its spawn leaves the thread running",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_left_running_by_early_return_race.c",
             .intent = "an early return leaves the thread running; both writes race",
             .dataRace = 1,
             .missingJoin = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_joined_on_one_branch_race.c",
             .intent = "a thread joined on one branch only still runs after the branches",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_joined_on_both_branches_written_before_race.c",
             .intent = "joined with success on both branches: the write before the if races, "
                       "the one after does not (L3)",
             .dataRace = 1,
             .racingSymbol = "before_value",
             .trackedMissingJoins = {{.issue = "#143", .function = "main", .line = 24, .column = 5}}},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_helper_in_loop_race.c",
             .intent = "threads a helper starts in a loop run together (#126)",
             .dataRace = 1,
             .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_helper_twice_race.c",
             .intent = "threads a helper called twice starts run together (#126)",
             .dataRace = 1,
             .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_helper_joined_each_time_no_fp.c",
             .intent = "a helper's thread joined before the helper is called again (#126)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_helper_in_loop_joined_each_round_no_fp.c",
             .intent = "a join ends the thread a helper started into storage refilled each round "
                       "(#126)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_helper_after_joining_previous_no_fp.c",
             .intent = "each round joins the previous thread before the helper starts the next "
                       "(#126)"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_started_by_helper_after_failed_join_race.c",
             .intent = "a round going on past a failed join starts beside the previous thread "
                       "(#126)",
             .dataRace = 1,
             .racingSymbol = "shared"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_from_previous_round_race.c",
             .intent = "the previous round's thread runs while main writes",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_joined_in_its_own_round_no_fp.c",
             .intent = "each round joins its thread before main writes again"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_threads_running_inside_join_loop_race.c",
             .intent = "later threads run between and inside the join loop, not after it",
             .dataRace = 2},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_restarted_into_joined_handle_race.c",
             .intent = "a thread restarted into a joined handle runs beside main",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_thread_joined_by_callee_before_its_write_no_fp.c",
             .intent = "a callee joins the thread its caller started before writing"},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_started_on_one_branch_race.cpp",
             .intent = "a thread started on one branch runs until its guarded join",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_started_by_method_on_one_branch_race.cpp",
             .intent = "a method starting a thread on one branch leaves it running",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_started_in_switch_case_race.cpp",
             .intent = "a thread started in one case runs until the join of that case",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_started_on_one_branch_then_helper_race.cpp",
             .intent = "a helper called while the thread may run races with it",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_started_flag_race.cpp",
             .intent = "a flag set with the start guards the join",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_correlated_start_and_join_race.cpp",
             .intent = "the same test of argc guards the start and the join",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_correlated_bool_local_race.cpp",
             .intent = "a bool local stored once guards the start and the join",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_flag_changed_between_start_and_join_race.cpp",
             .intent = "a flag changed between the two tests does not tie them",
             .dataRace = 2,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_condition_varying_across_rounds_race.cpp",
             .intent = "a test computed anew each round does not tie the rounds",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_called_before_and_beside_thread_no_fp.c",
             .intent = "a helper's access is seen with the threads running at each of its calls"},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_called_beside_thread_on_its_data_race.c",
             .intent = "a helper called beside the thread on the thread's data races with it",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency/data-race/"
                     "data_race_helper_chain_called_before_and_beside_thread_race.c",
             .intent = "through a chain of calls, only the call beside the thread races with it",
             .dataRace = 1},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_owning_objects_built_in_loop_no_fp.cpp",
             .intent = "a constructor builds an object no earlier thread of its class holds",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_constructor_starts_two_entries_on_object_race.cpp",
             .intent = "two entries a constructor starts on its object share it (#126)",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_constructor_started_run_called_by_main_race.cpp",
             .intent = "main calling the entry on the object races with the thread there (#126)",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_constructor_started_instances_write_global_race.cpp",
             .intent = "threads each constructor starts on its own object share a global (#126)",
             .dataRace = 1,
             .racingSymbol = "_ZL5total",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_constructor_counts_then_starts_race.cpp",
             .intent = "each thread races with the next one and with the next constructor (#126)",
             .dataRace = 2,
             .racingSymbol = "_ZL5total",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_built_in_place_by_helper_no_fp.cpp",
             .intent = "a constructor called from a helper builds an object no earlier thread holds",
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_built_in_place_by_helper_written_after_start_race.cpp",
             .intent = "a constructor called from a helper races with the thread it started",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_owning_objects_constructor_writes_global_race.cpp",
             .intent = "a constructor's write to a global races with earlier rounds' threads",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_owning_object_written_after_start_race.cpp",
             .intent = "a constructor races with the thread it started on its object",
             .dataRace = 1,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_started_twice_on_object_by_method_race.cpp",
             .intent = "a method started twice on one object races with its first thread, and "
                       "the two threads race with each other (#126)",
             .dataRace = 2,
             .requiresCxx20 = true},
            {.path = "tests/fixtures/concurrency-cxx20/"
                     "cpp_thread_joined_by_helper_catching_failure_race.cpp",
             .intent = "a helper returning past a failed join it caught has not ended the thread",
             .dataRace = 1,
             .racingSymbol = "_ZL6shared",
             .requiresCxx20 = true},

            // --- compiler error path -------------------------------------------------------
            {.path = "tests/fixtures/concurrency/data-race/cpp_double_checked_locking.cpp",
             .intent = "kept to exercise the compile failure path",
             .expectCompileFailure = true},
        };

        return expectations;
    }
    // clang-format on

    /// How many of the report's missing joins the expectation's tracked entries name, each entry
    /// taking one diagnostic reported where it says. An entry left without one fails the row.
    std::size_t matchTrackedMissingJoins(const DiagnosticReport& report,
                                         const FixtureExpectation& expectation, bool& ok)
    {
        std::vector<bool> taken(report.diagnostics.size(), false);
        std::size_t matched = 0;
        for (const TrackedMissingJoin& tracked : expectation.trackedMissingJoins)
        {
            bool found = false;
            for (std::size_t index = 0; index < report.diagnostics.size() && !found; ++index)
            {
                const Diagnostic& diagnostic = report.diagnostics[index];
                found = !taken[index] && diagnostic.ruleId == RuleId::MissingJoin &&
                        diagnostic.location.function == tracked.function &&
                        diagnostic.location.line == tracked.line &&
                        diagnostic.location.column == tracked.column;
                taken[index] = taken[index] || found;
            }
            if (found)
            {
                ++matched;
                continue;
            }

            std::cerr << "[FAIL] " << expectation.path << " (" << expectation.intent
                      << "): the false missing join " << tracked.issue << " tracks in "
                      << tracked.function << " at " << tracked.line << ":" << tracked.column
                      << " is no longer reported; remove its entry if " << tracked.issue
                      << " fixed it\n";
            ok = false;
        }
        return matched;
    }

    /// Analyzes a fixture once with every rule enabled, then compares each rule's diagnostic
    /// count. Running the rules separately would recompile the fixture three times, and the
    /// checkers are independent, so one pass gives the same counts.
    bool checkFixtureExpectation(const FixtureExpectation& expectation)
    {
        std::vector<std::string> compileArgs;
        if (expectation.requiresCxx20)
            compileArgs.emplace_back(kCxx20Standard);

        const AnalysisOptions options = AnalysisOptions::allAvailable();
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
            {RuleId::ConditionWaitWithoutPredicate, expectation.conditionWait, "condition-wait"},
            {RuleId::ForkAfterThreadCreation, expectation.forkAfterThread, "fork-after-thread"},
            {RuleId::UnreapedChildProcess, expectation.unreapedChild, "unreaped-child"},
            {RuleId::ThreadArgumentEscapesFrame, expectation.threadArgumentEscape,
             "thread-arg-escape"},
            {RuleId::UnsafeSignalHandler, expectation.unsafeSignalHandler, "unsafe-signal-handler"},
            {RuleId::WeakPublicationOrdering, expectation.weakPublication, "weak-publication"},
            {RuleId::ThreadArgumentFreedEarly, expectation.threadArgumentFreed, "thread-arg-freed"},
            {RuleId::ThreadLocalOutlivesThread, expectation.threadLocalEscape,
             "thread-local-escape"},
        };

        bool ok = true;
        const std::size_t trackedMissingJoins = matchTrackedMissingJoins(*report, expectation, ok);
        for (const RuleColumn& column : columns)
        {
            const std::size_t actual =
                countDiagnosticsForRule(*report, column.rule) -
                (column.rule == RuleId::MissingJoin ? trackedMissingJoins : 0);
            const bool satisfied = expectation.countsAreMinimums ? actual >= column.expected
                                                                 : actual == column.expected;
            if (satisfied)
                continue;

            std::cerr << "[FAIL] " << expectation.path << " (" << expectation.intent
                      << "): " << column.name << " expected "
                      << (expectation.countsAreMinimums ? "at least " : "") << column.expected
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

    /// Selecting one rule must report exactly what an all-rules run reports for that rule —
    /// every field of every diagnostic, not just how many.
    ///
    /// The facts a unit analysis builds now depend on the selection, so "the checkers are
    /// independent" stopped being an observation about the checkers and became a claim about
    /// the fact selection: a rule that reads a fact nobody else asked for must still get it,
    /// and a fact left out must not be one it needed. This compares the eight selections
    /// against the single all-rules run the table above already performs.
    bool checkEveryRuleAloneMatchesAllRules(const FixtureExpectation& expectation)
    {
        std::vector<std::string> compileArgs;
        if (expectation.requiresCxx20)
            compileArgs.emplace_back(kCxx20Standard);

        // Compiled once; only the analysis is repeated per selection.
        const std::optional<CompiledFixture> compiled =
            compileFixture(expectation.path, std::move(compileArgs));
        if (!compiled.has_value())
            return false;

        const DiagnosticReport everything =
            SingleTUConcurrencyAnalyzer(AnalysisOptions::allAvailable()).analyze(*compiled->module);

        constexpr RuleId kRules[] = {
            RuleId::DataRaceGlobal,
            RuleId::MissingJoin,
            RuleId::DeadlockLockOrder,
            RuleId::ConditionWaitWithoutPredicate,
            RuleId::ForkAfterThreadCreation,
            RuleId::UnreapedChildProcess,
            RuleId::ThreadArgumentEscapesFrame,
            RuleId::UnsafeSignalHandler,
            RuleId::WeakPublicationOrdering,
            RuleId::ThreadArgumentFreedEarly,
            RuleId::ThreadLocalOutlivesThread,
        };

        bool ok = true;
        for (const RuleId rule : kRules)
        {
            const DiagnosticReport alone =
                SingleTUConcurrencyAnalyzer(AnalysisOptions{.enabledRules = {rule}})
                    .analyze(*compiled->module);

            const std::vector<std::string> expected = describedDiagnosticsForRule(everything, rule);
            const std::vector<std::string> actual = describedDiagnosticsForRule(alone, rule);
            if (expected == actual)
            {
                // And nothing belonging to another rule may appear in a run that excluded it.
                if (alone.diagnostics.size() == actual.size())
                    continue;

                std::cerr << "[FAIL] " << expectation.path << " (" << expectation.intent
                          << "): selecting one rule reported " << alone.diagnostics.size()
                          << " diagnostic(s), " << actual.size() << " of them its own\n";
                ok = false;
                continue;
            }

            std::cerr << "[FAIL] " << expectation.path << " (" << expectation.intent << "): rule "
                      << static_cast<int>(rule) << " alone reported " << actual.size()
                      << " diagnostic(s), the all-rules run " << expected.size() << "\n";
            for (const std::string& description : expected)
            {
                if (std::find(actual.begin(), actual.end(), description) == actual.end())
                    std::cerr << "         only in the all-rules run: " << description << "\n";
            }
            for (const std::string& description : actual)
            {
                if (std::find(expected.begin(), expected.end(), description) == expected.end())
                    std::cerr << "         only in the single-rule run: " << description << "\n";
            }
            ok = false;
        }

        return ok;
    }

    /// #111: the owner's own access to a local object it handed to a thread reads the way the
    /// thread's side of the pair does, as an object shared with a thread. It is never described
    /// as a global named after the object's internal storage label.
    bool testOwnerAccessToLocalObjectNamesSharedObjectNotGlobal()
    {
        constexpr std::string_view kSharedObjectMessage =
            "unsynchronized concurrent access to an object shared with a thread";
        constexpr std::string_view kFixtures[] = {
            "tests/fixtures/concurrency/data-race/data_race_owner_writes_local_object.c",
            "tests/fixtures/concurrency/data-race/"
            "data_race_owner_writes_local_object_through_pointer.c",
            "tests/fixtures/concurrency/data-race/data_race_owner_locks_other_mutex.c",
            "tests/fixtures/concurrency/data-race/data_race_owner_unlocked_while_thread_locks.c",
            // Reached through the pointer the thread was handed: named after the slot holding it.
            "tests/fixtures/concurrency/data-race/data_race_creator_and_thread_share_object.c",
        };

        bool ok = true;
        for (const std::string_view fixture : kFixtures)
        {
            ok = checkEveryDiagnosticMessage(fixture, RuleId::DataRaceGlobal,
                                             kSharedObjectMessage) &&
                 ok;
        }

        return checkEveryDiagnosticMessage(
                   "tests/fixtures/concurrency-cxx20/cpp_owner_writes_local_object_race.cpp",
                   RuleId::DataRaceGlobal, kSharedObjectMessage, {std::string(kCxx20Standard)}) &&
               ok;
    }

    /// The global-object neighbours of #111 must keep naming the global: the fix keys off
    /// whether the symbol names an object handed to a thread, not off dropping every global's
    /// name from the message.
    bool testOwnerAccessToGlobalObjectStillNamesGlobal()
    {
        const bool cOk = checkEveryDiagnosticMessage(
            "tests/fixtures/concurrency/data-race/data_race_owner_writes_global_object.c",
            RuleId::DataRaceGlobal, "unsynchronized concurrent access to global 'shared'");
        const bool cppOk = checkEveryDiagnosticMessage(
            "tests/fixtures/concurrency-cxx20/cpp_owner_writes_global_object_race.cpp",
            RuleId::DataRaceGlobal, "unsynchronized concurrent access to global 'counter'",
            {std::string(kCxx20Standard)});
        return cOk && cppOk;
    }

    /// An access count that was never taken is absent, and a count of zero means the accesses
    /// were examined and there were none. Reporting zero for both would answer a question
    /// nobody asked, and a reader cannot tell the two apart afterwards.
    /// The default analysis runs every rule. Rules used to be added to `allAvailable()` alone
    /// and left out of the default without anyone deciding so (#53); a rule meant to be opt-in
    /// has to change this test, which makes that a decision instead of an omission.
    bool testDefaultOptionsEnableEveryAvailableRule()
    {
        const std::vector<RuleId> byDefault = AnalysisOptions{}.enabledRules;
        const std::vector<RuleId> available = AnalysisOptions::allAvailable().enabledRules;
        if (byDefault == available)
            return true;

        std::cerr << "[FAIL] the default analysis enables " << byDefault.size()
                  << " rule(s), allAvailable() " << available.size() << "\n";
        return false;
    }

    bool testAccessCountsDistinguishNotComputedFromZero()
    {
        const std::optional<CompiledFixture> compiled =
            compileFixture("tests/fixtures/concurrency/deadlock/deadlock_basic.c");
        if (!compiled.has_value())
            return false;

        const DiagnosticReport withAccesses =
            SingleTUConcurrencyAnalyzer(AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal}})
                .analyze(*compiled->module);
        const DiagnosticReport withoutAccesses =
            SingleTUConcurrencyAnalyzer(
                AnalysisOptions{.enabledRules = {RuleId::DeadlockLockOrder}})
                .analyze(*compiled->module);

        // Written as loops rather than std::any_of with a multi-line lambda: clang-format
        // releases disagree on how to break the latter, and the CI check is a different build
        // from the one on a developer's machine.
        const auto countedSomething = [](const DiagnosticReport& report)
        {
            for (const FunctionSummary& function : report.functions)
            {
                if (function.sharedAccessCount.value_or(0) > 0)
                    return true;
            }
            return false;
        };
        const auto everyCountAbsent = [](const DiagnosticReport& report)
        {
            for (const FunctionSummary& function : report.functions)
            {
                if (function.sharedAccessCount.has_value() ||
                    function.protectedAccessCount.has_value() ||
                    function.writeAccessCount.has_value())
                {
                    return false;
                }
            }
            return true;
        };
        // The fixture locks both mutexes around its accesses, so a genuine zero is available:
        // the accesses were counted, and every one of them was protected.
        const auto hasGenuineZero = [](const DiagnosticReport& report)
        {
            for (const FunctionSummary& function : report.functions)
            {
                if (!function.sharedAccessCount.has_value() ||
                    !function.writeAccessCount.has_value())
                {
                    continue;
                }
                if (*function.sharedAccessCount > 0 &&
                    *function.protectedAccessCount == *function.sharedAccessCount)
                {
                    return true;
                }
            }
            return false;
        };

        return assertTrue(!withAccesses.functions.empty() && !withoutAccesses.functions.empty(),
                          "both selections summarise the functions they know about") &&
               assertTrue(countedSomething(withAccesses),
                          "a selection that reads the accesses reports how many it found") &&
               assertTrue(hasGenuineZero(withAccesses),
                          "a count the analysis did take is reported even when it is not news") &&
               assertTrue(everyCountAbsent(withoutAccesses),
                          "a selection that never read the accesses reports no count at all, "
                          "rather than zero");
    }

    /// The optimization level a caller asks for must not change what is reported.
    ///
    /// This is a regression, not a hypothetical: a project whose compilation database said -O3
    /// reported nothing, because a lock helper had been inlined into its caller and a wait
    /// reached through a helper had collapsed into a library primitive. Nothing signalled the
    /// loss — the analysis simply described a program the user never wrote.
    bool testOptimizationRequestDoesNotChangeFindings()
    {
        constexpr std::string_view kStructuralFixtures[] = {
            "tests/fixtures/concurrency/condition-variable/cpp_condition_wait_through_helper.cpp",
            "tests/fixtures/concurrency/deadlock/deadlock_through_lock_wrappers.c",
            "tests/fixtures/concurrency/data-race/data_race_lock_wrapper_protected_no_fp.c",
        };

        bool ok = true;
        for (const std::string_view fixture : kStructuralFixtures)
        {
            std::vector<std::string> unoptimized;
            std::vector<std::string> optimized{"-O2"};
            if (fixture.ends_with(".cpp"))
            {
                unoptimized.emplace_back(kCxx20Standard);
                optimized.emplace_back(kCxx20Standard);
            }

            const std::optional<DiagnosticReport> plain =
                analyzeFixture(fixture, AnalysisOptions::allAvailable(), std::move(unoptimized));
            const std::optional<DiagnosticReport> requested =
                analyzeFixture(fixture, AnalysisOptions::allAvailable(), std::move(optimized));
            if (!plain.has_value() || !requested.has_value())
            {
                ok = false;
                continue;
            }

            ok = assertTrue(plain->diagnostics.size() == requested->diagnostics.size(),
                            std::string("asking for -O2 must not change what ") +
                                std::string(fixture) + " reports") &&
                 ok;
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
            ok = checkEveryRuleAloneMatchesAllRules(expectation) && ok;
        }

        return ok;
    }

    /// A tracked false missing join excuses one diagnostic, reported where the entry says, and
    /// nothing else: the row still checks every other diagnostic, and fails, asking for the entry
    /// to go, once the tracked one is no longer reported.
    bool testTrackedMissingJoinsExcuseOnlyWhatTheyName()
    {
        const auto& expectations = fixtureExpectations();
        const auto row = std::find_if(
            expectations.begin(), expectations.end(), [](const FixtureExpectation& expectation)
            { return expectation.path.ends_with("helper_called_twice_joined_race.c"); });
        if (!assertTrue(row != expectations.end() && row->trackedMissingJoins.size() == 2,
                        "the helper row should track its two false missing joins"))
            return false;

        // The failures below are expected: keep their messages out of the test's output, and
        // read them.
        auto check = [](const FixtureExpectation& expectation, std::string& messages)
        {
            std::ostringstream captured;
            std::streambuf* const previous = std::cerr.rdbuf(captured.rdbuf());
            const bool passed = checkFixtureExpectation(expectation);
            std::cerr.rdbuf(previous);
            messages = captured.str();
            return passed;
        };

        bool ok = true;
        std::string messages;

        FixtureExpectation oneUntracked = *row;
        oneUntracked.trackedMissingJoins.pop_back();
        ok = assertTrue(!check(oneUntracked, messages) &&
                            messages.find("missing-join expected 0 diagnostic(s), got 1") !=
                                std::string::npos,
                        "a missing join no entry names should still be counted: " + messages) &&
             ok;

        FixtureExpectation stale = *row;
        stale.trackedMissingJoins.push_back(
            {.issue = "#173", .function = "start", .line = 17, .column = 9});
        ok =
            assertTrue(
                !check(stale, messages) && messages.find("no longer reported") != std::string::npos,
                "an entry naming no reported missing join should fail the row: " + messages) &&
            ok;

        FixtureExpectation otherRule = *row;
        ++otherRule.dataRace;
        ok = assertTrue(!check(otherRule, messages) &&
                            messages.find("data-race expected 2") != std::string::npos,
                        "the other rules should still be checked: " + messages) &&
             ok;

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
    ok = testHelperOrderReportedWhereTaken() && ok;
    ok = testConsistentLockOrderHasNoDeadlock() && ok;
    ok = testOppositeLockOrderOutsideThreadsHasNoDeadlock() && ok;
    ok = testIndependentLocksHaveNoDeadlock() && ok;
    ok = testLockOrderSearchLimitIsANoticeNotAFinding() && ok;
    ok = testCompleteLockOrderSearchGivesNoNotice() && ok;
    ok = testOwnerAccessToLocalObjectNamesSharedObjectNotGlobal() && ok;
    ok = testOwnerAccessToGlobalObjectStillNamesGlobal() && ok;
    ok = testDefaultOptionsEnableEveryAvailableRule() && ok;
    ok = testAccessCountsDistinguishNotComputedFromZero() && ok;
    ok = testOptimizationRequestDoesNotChangeFindings() && ok;
    ok = testFixtureExpectationTable() && ok;
    ok = testTrackedMissingJoinsExcuseOnlyWhatTheyName() && ok;
    ok = testEveryConcurrencyFixtureIsCovered() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] concurrency analysis tests\n";
    return 0;
}
