// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"
#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/LLVMContext.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace
{
    using ctrace::concurrency::CompileRequest;
    using ctrace::concurrency::CompileResult;
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

    std::optional<DiagnosticReport> analyzeFixture(std::string_view relativePath)
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

        SingleTUConcurrencyAnalyzer analyzer;
        return analyzer.analyze(*compileResult.module);
    }

    std::optional<std::string> symbolOf(const ctrace::concurrency::Diagnostic& diagnostic)
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

    bool hasDiagnosticForSymbol(const DiagnosticReport& report, std::string_view symbol)
    {
        return std::any_of(report.diagnostics.begin(), report.diagnostics.end(),
                           [symbol](const auto& diagnostic)
                           {
                               const std::optional<std::string> value = symbolOf(diagnostic);
                               return value.has_value() && *value == symbol;
                           });
    }

    bool testDataRaceBasicIsReported()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_basic.c");
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

    bool testAtomicVsNonAtomicReportsSharedState()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_atomic_vs_non_atomic.cpp");
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
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_data_race_class.cpp");
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
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_shared_object_by_ref.cpp");
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
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_mutex_protected.c");
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
            analyzeFixture("tests/fixtures/concurrency/data-race/data_race_split_symbols.c");
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
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_thread_local_class.cpp");
        if (!report.has_value())
            return false;

        return assertTrue(report->diagnostics.empty(),
                          "thread-local class fixture should not report a race") &&
               assertTrue(report->diagnosticsSummary.error == 0,
                          "thread-local class fixture should not count error diagnostics");
    }

    bool testMoveSemanticsRaceUsesUserLocations()
    {
        const std::optional<DiagnosticReport> report =
            analyzeFixture("tests/fixtures/concurrency/data-race/cpp_move_semantics_race.cpp");
        if (!report.has_value())
            return false;

        const bool allSharedResource = std::all_of(
            report->diagnostics.begin(), report->diagnostics.end(),
            [](const auto& diagnostic)
            {
                const std::optional<std::string> symbol = symbolOf(diagnostic);
                return symbol.has_value() && *symbol == "shared_resource";
            });

        const bool allPrimaryLocationsInFixture =
            std::all_of(report->diagnostics.begin(), report->diagnostics.end(),
                        [](const auto& diagnostic)
                        {
                            return locationReferencesFixture(
                                diagnostic.location, "cpp_move_semantics_race.cpp");
                        });

        const bool hasLoweredRelatedLocation =
            std::any_of(report->diagnostics.begin(), report->diagnostics.end(),
                        [](const auto& diagnostic)
                        {
                            return std::any_of(
                                diagnostic.relatedLocations.begin(),
                                diagnostic.relatedLocations.end(),
                                [](const auto& related)
                                {
                                    return related.label.starts_with("Lowered ") &&
                                           !locationReferencesFixture(
                                               related.location,
                                               "cpp_move_semantics_race.cpp");
                                });
                        });

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
} // namespace

int main()
{
    bool ok = true;

    ok = testDataRaceBasicIsReported() && ok;
    ok = testAtomicVsNonAtomicReportsSharedState() && ok;
    ok = testClassDataRaceReportsGlobalCounter() && ok;
    ok = testSharedObjectByRefReportsGlobalCounter() && ok;
    ok = testMutexProtectedFixtureHasNoDiagnostics() && ok;
    ok = testTwoGlobalFixtureOnlyReportsRacySymbol() && ok;
    ok = testThreadLocalClassHasNoDiagnostics() && ok;
    ok = testMoveSemanticsRaceUsesUserLocations() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] concurrency analysis tests\n";
    return 0;
}
