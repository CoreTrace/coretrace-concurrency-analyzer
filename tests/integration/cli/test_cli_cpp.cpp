// SPDX-License-Identifier: Apache-2.0
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    /// What `--fail-on` exits with when the report crosses the gate (`kFindingsExitCode` in
    /// main.cpp).
    constexpr int kFindingsExitCode = 2;

    struct RunResult
    {
        int exitCode = -1;
        std::string output;
    };

    std::filesystem::path projectSourceDir()
    {
        return std::filesystem::path(CORETRACE_PROJECT_SOURCE_DIR);
    }

    std::filesystem::path projectBinaryDir()
    {
        return std::filesystem::path(CORETRACE_PROJECT_BINARY_DIR);
    }

    std::filesystem::path analyzerBinaryPath()
    {
        return projectBinaryDir() / "coretrace_concurrency_analyzer";
    }

    std::filesystem::path fixturePath(std::string_view relativePath)
    {
        return projectSourceDir() / "tests" / "fixtures" / relativePath;
    }

    std::string shellEscape(std::string_view value)
    {
        std::string escaped = "'";
        for (const char c : value)
        {
            if (c == '\'')
                escaped += "'\\''";
            else
                escaped += c;
        }
        escaped += "'";
        return escaped;
    }

    RunResult runCommand(const std::string& command)
    {
        RunResult result;

        FILE* pipe = ::popen((command + " 2>&1").c_str(), "r");
        if (!pipe)
            return result;

        std::array<char, 256> buffer{};
        while (::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            result.output += buffer.data();

        const int status = ::pclose(pipe);
        if (status != -1 && WIFEXITED(status))
            result.exitCode = WEXITSTATUS(status);
        return result;
    }

    RunResult runAnalyzer(const std::vector<std::string>& args, std::string_view envPrefix = {})
    {
        std::string command;
        if (!envPrefix.empty())
        {
            command += std::string(envPrefix);
            command += " ";
        }

        command += shellEscape(analyzerBinaryPath().string());
        for (const std::string& arg : args)
        {
            command += " ";
            command += shellEscape(arg);
        }
        return runCommand(command);
    }

    std::filesystem::path makeTempDir(std::string_view prefix)
    {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 256; ++attempt)
        {
            const std::filesystem::path path =
                base / (std::string(prefix) + "-" + std::to_string(::getpid()) + "-" +
                        std::to_string(attempt));
            std::error_code ec;
            if (std::filesystem::create_directory(path, ec))
                return path;
        }
        return {};
    }

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    bool assertContains(const std::string& text, std::string_view expected, std::string_view label)
    {
        return assertTrue(text.find(expected) != std::string::npos,
                          std::string(label) + " missing expected token: " + std::string(expected) +
                              "\noutput:\n" + text);
    }

    bool assertNotContains(const std::string& text, std::string_view unexpected,
                           std::string_view label)
    {
        return assertTrue(text.find(unexpected) == std::string::npos,
                          std::string(label) + " unexpectedly contains token: " +
                              std::string(unexpected) + "\noutput:\n" + text);
    }

    bool testHelpAndInputParsingErrors()
    {
        bool ok = true;

        {
            const RunResult result = runAnalyzer({"--help"});
            ok = assertTrue(result.exitCode == 0, "--help should exit with code 0") && ok;
            ok = assertContains(result.output, "Usage:", "--help output") && ok;
            ok = assertContains(result.output, "--analyze", "--help output") && ok;
            ok = assertContains(result.output, "--rules=<comma-separated>|all", "--help output") &&
                 ok;
            // Every rule the option accepts is named, and the default is stated.
            ok = assertContains(result.output, "unsafe-signal-handler", "--help output") && ok;
            ok = assertContains(result.output, "(default: every rule)", "--help output") && ok;
            ok = assertContains(result.output, "--format=human|json|sarif", "--help output") && ok;
            ok = assertContains(result.output, "--verbose", "--help output") && ok;
        }

        {
            const RunResult result = runAnalyzer({});
            ok = assertTrue(result.exitCode == 1, "no-args invocation should fail with code 1") &&
                 ok;
            ok = assertContains(result.output, "Usage:", "no-args output") && ok;
        }

        {
            const RunResult result = runAnalyzer({"--unknown"});
            ok = assertTrue(result.exitCode == 1, "unknown option should fail with code 1") && ok;
            ok = assertContains(result.output, "Unknown option", "unknown option output") && ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), "--ir-format=xyz"});
            ok = assertTrue(result.exitCode == 1, "invalid --ir-format should fail") && ok;
            ok = assertContains(result.output, "Unsupported --ir-format value",
                                "invalid --ir-format output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), "--analyze", "--format=xml"});
            ok = assertTrue(result.exitCode == 1, "invalid --format should fail") && ok;
            ok = assertContains(result.output, "Unsupported --format value",
                                "invalid --format output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), "--format=json"});
            ok = assertTrue(result.exitCode == 1, "--format without --analyze should fail") && ok;
            ok = assertContains(result.output, "--format requires --analyze",
                                "format without analyze output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer({fixturePath("hello.c").string(), "--rules=all"});
            ok = assertTrue(result.exitCode == 1, "--rules without --analyze should fail") && ok;
            ok = assertContains(result.output, "--rules requires --analyze",
                                "rules without analyze output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), "--analyze", "--rules=unknown"});
            ok = assertTrue(result.exitCode == 1, "invalid --rules should fail") && ok;
            ok = assertContains(result.output, "Unsupported --rules value",
                                "invalid --rules output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), fixturePath("hello.cpp").string()});
            ok = assertTrue(result.exitCode == 1, "extra positional arg should fail") && ok;
            ok = assertContains(result.output, "Unexpected positional argument",
                                "extra positional output") &&
                 ok;
        }

        return ok;
    }

    bool testSuccessfulCompilesAndVerboseMode()
    {
        bool ok = true;

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), "--ir-format=ll", "--verbose",
                             "--compile-arg=-DFOO=1", "--", "-Wall"});
            ok = assertTrue(result.exitCode == 0, "LL compile with verbose mode should succeed") &&
                 ok;
            ok = assertContains(result.output, "request.input-file:", "verbose output") && ok;
            ok = assertContains(result.output, "request.ir-format: ll", "verbose output") && ok;
            ok = assertContains(result.output, "request.analyze: false", "verbose output") && ok;
            ok =
                assertContains(result.output, "request.extra-arg: -DFOO=1", "verbose output") && ok;
            ok = assertContains(result.output, "request.extra-arg: -Wall", "verbose output") && ok;
            ok = assertContains(result.output, "IR compilation succeeded", "LL compile output") &&
                 ok;
            ok = assertContains(result.output, "format: ll", "LL compile output") && ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("hello.c").string(), "--ir-format=bc", "--instrument"});
            ok = assertTrue(result.exitCode == 0,
                            "BC compile with instrumentation should succeed") &&
                 ok;
            ok = assertContains(result.output, "IR compilation succeeded", "BC compile output") &&
                 ok;
            ok = assertContains(result.output, "format: bc", "BC compile output") && ok;
        }

        return ok;
    }

    bool testAnalyzeMode()
    {
        bool ok = true;

        {
            const RunResult result = runAnalyzer({fixturePath("hello.c").string(), "--analyze"});
            ok = assertTrue(result.exitCode == 0, "--analyze on hello.c should succeed") && ok;
            ok = assertContains(result.output, "Mode: IR", "hello analyze output") && ok;
            ok = assertContains(result.output, "Diagnostics summary: info=0, warning=0, error=0",
                                "hello analyze output") &&
                 ok;
            ok = assertNotContains(result.output, "IR compilation succeeded",
                                   "hello analyze output should be pure human report") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/data-race/data_race_basic.c").string(), "--analyze"});
            ok = assertTrue(result.exitCode == 0,
                            "--analyze on data_race_basic should not fail the CLI") &&
                 ok;
            ok = assertContains(result.output, "Function:", "race analyze output") && ok;
            ok = assertContains(result.output, "ruleId: DataRaceGlobal", "race analyze output") &&
                 ok;
            ok = assertContains(result.output, "symbol: shared_counter", "race analyze output") &&
                 ok;
            ok =
                assertContains(result.output, "at line 10, column 23", "race analyze output") && ok;
            ok = assertContains(result.output, "Diagnostics summary: info=0, warning=0, error=",
                                "race analyze output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/data-race/cpp_data_race_class.cpp").string(),
                             "--analyze"});
            ok = assertTrue(result.exitCode == 0,
                            "--analyze on cpp_data_race_class should not fail the CLI") &&
                 ok;
            ok = assertContains(result.output, "ruleId: DataRaceGlobal",
                                "cpp_data_race_class analyze output") &&
                 ok;
            ok = assertContains(result.output, "symbol: global_counter",
                                "cpp_data_race_class analyze output") &&
                 ok;
            ok = assertContains(result.output, "Function: increment",
                                "cpp_data_race_class analyze output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/data-race/data_race_mutex_protected.c").string(),
                 "--analyze"});
            ok = assertTrue(result.exitCode == 0,
                            "--analyze on mutex-protected fixture should succeed") &&
                 ok;
            ok = assertContains(result.output, "Diagnostics summary: info=0, warning=0, error=0",
                                "mutex-protected analyze output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/data-race/data_race_split_symbols.c").string(),
                 "--analyze"});
            ok = assertTrue(result.exitCode == 0,
                            "--analyze on split-symbol fixture should succeed") &&
                 ok;
            ok = assertContains(result.output, "symbol: racy_counter",
                                "split-symbol analyze output") &&
                 ok;
            ok = assertNotContains(result.output, "symbol: safe_counter",
                                   "split-symbol analyze output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/data-race/cpp_move_semantics_race.cpp").string(),
                 "--analyze"});
            ok = assertTrue(result.exitCode == 0,
                            "--analyze on cpp_move_semantics_race should succeed") &&
                 ok;
            ok = assertContains(result.output, "symbol: shared_resource",
                                "cpp_move_semantics_race analyze output") &&
                 ok;
            ok = assertNotContains(result.output, "symbol: _ZNSt3__14coutE",
                                   "cpp_move_semantics_race analyze output") &&
                 ok;
            ok = assertContains(result.output, "Function: producer",
                                "cpp_move_semantics_race analyze output") &&
                 ok;
            ok = assertContains(result.output, "related: Lowered first access ->",
                                "cpp_move_semantics_race analyze output") &&
                 ok;
            ok = assertContains(result.output, "cpp_move_semantics_race.cpp:18:21 in producer",
                                "cpp_move_semantics_race analyze output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/data-race/data_race_basic.c").string(),
                             "--analyze", "--format=json"});
            ok = assertTrue(result.exitCode == 0,
                            "--format=json on data_race_basic should succeed") &&
                 ok;
            ok = assertContains(result.output, "\"meta\"", "json analyze output") && ok;
            ok = assertContains(result.output, "\"functions\"", "json analyze output") && ok;
            ok = assertContains(result.output, "\"diagnostics\"", "json analyze output") && ok;
            ok = assertContains(result.output, "\"ruleId\": \"DataRaceGlobal\"",
                                "json analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"symbol\": \"shared_counter\"",
                                "json analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"confidence\": \"HIGH\"", "json analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"firstAliasProvenance\": \"direct\"",
                                "json analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"secondAliasProvenance\": \"direct\"",
                                "json analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"variableAliasing\": [", "json analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"startLine\": 10", "json analyze output") && ok;
            ok = assertContains(result.output, "\"startColumn\": 23", "json analyze output") && ok;
        }

        // What a JSON consumer is told about counts nobody took. Under a selection that reads
        // the shared accesses the counts are there, zeros included; under one that does not,
        // the fields are absent rather than zero, which would read as an answer.
        {
            const RunResult counted =
                runAnalyzer({fixturePath("concurrency/deadlock/deadlock_basic.c").string(),
                             "--analyze", "--rules=data-race", "--format=json"});
            const RunResult uncounted =
                runAnalyzer({fixturePath("concurrency/deadlock/deadlock_basic.c").string(),
                             "--analyze", "--rules=deadlock-lock-order", "--format=json"});

            ok = assertTrue(counted.exitCode == 0 && uncounted.exitCode == 0,
                            "both rule selections analyse deadlock_basic") &&
                 ok;
            ok = assertContains(counted.output, "\"sharedAccessCount\"",
                                "json output of a run that read the accesses") &&
                 ok;
            ok = assertContains(uncounted.output, "\"functions\"",
                                "json output of a run that did not read the accesses") &&
                 ok;
            ok = assertNotContains(uncounted.output, "\"sharedAccessCount\"",
                                   "json output of a run that did not read the accesses") &&
                 ok;
            ok = assertNotContains(uncounted.output, "\"protectedAccessCount\"",
                                   "json output of a run that did not read the accesses") &&
                 ok;
            ok = assertNotContains(uncounted.output, "\"writeAccessCount\"",
                                   "json output of a run that did not read the accesses") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/data-race/cpp_move_semantics_race.cpp").string(),
                 "--analyze", "--format=json"});
            ok = assertTrue(result.exitCode == 0,
                            "--format=json on cpp_move_semantics_race should succeed") &&
                 ok;
            ok = assertContains(result.output, "\"symbol\": \"shared_resource\"",
                                "cpp_move_semantics_race json output") &&
                 ok;
            ok = assertNotContains(result.output, "_ZNSt3__14coutE",
                                   "cpp_move_semantics_race json output") &&
                 ok;
            ok = assertContains(result.output, "\"relatedLocations\": [",
                                "cpp_move_semantics_race json output") &&
                 ok;
            ok = assertContains(result.output, "\"label\": \"Lowered first access\"",
                                "cpp_move_semantics_race json output") &&
                 ok;
            ok = assertContains(result.output, "cpp_move_semantics_race.cpp",
                                "cpp_move_semantics_race json output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/data-race/data_race_basic.c").string(),
                             "--analyze", "--format=sarif"});
            ok = assertTrue(result.exitCode == 0,
                            "--format=sarif on data_race_basic should succeed") &&
                 ok;
            ok = assertContains(result.output, "\"version\": \"2.1.0\"", "sarif analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"runs\"", "sarif analyze output") && ok;
            ok = assertContains(result.output, "\"ruleId\": \"DataRaceGlobal\"",
                                "sarif analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"partialFingerprints\"", "sarif analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"properties\"", "sarif analyze output") && ok;
            ok =
                assertContains(result.output, "\"confidence\": \"HIGH\"", "sarif analyze output") &&
                ok;
            ok = assertContains(result.output, "\"coretraceDiagnosticProperties\"",
                                "sarif analyze output") &&
                 ok;
            ok = assertContains(result.output, "\"startLine\": 10", "sarif analyze output") && ok;
            ok = assertContains(result.output, "\"startColumn\": 23", "sarif analyze output") && ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("invalid.c").string(), "--analyze", "--format=human"});
            ok = assertTrue(result.exitCode == 1,
                            "--format=human on invalid.c should fail with structured output") &&
                 ok;
            ok = assertContains(result.output, "Location:", "human compile-error output") && ok;
            ok = assertContains(result.output, "tests/fixtures/invalid.c:2:1",
                                "human compile-error output") &&
                 ok;
            ok = assertContains(result.output, "ruleId: CompilerDiagnostic",
                                "human compile-error output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("invalid.c").string(), "--analyze", "--format=json"});
            ok = assertTrue(result.exitCode == 1,
                            "--format=json on invalid.c should fail with structured output") &&
                 ok;
            ok = assertContains(result.output, "\"ruleId\": \"CompilerDiagnostic\"",
                                "json compile-error output") &&
                 ok;
            ok = assertContains(result.output, "\"file\": \"", "json compile-error output") && ok;
            ok = assertContains(result.output, "tests/fixtures/invalid.c",
                                "json compile-error output") &&
                 ok;
            ok = assertContains(result.output, "\"startLine\": 2", "json compile-error output") &&
                 ok;
            ok = assertContains(result.output, "\"startColumn\": 1", "json compile-error output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("invalid.c").string(), "--analyze", "--format=sarif"});
            ok = assertTrue(result.exitCode == 1,
                            "--format=sarif on invalid.c should fail with structured output") &&
                 ok;
            ok = assertContains(result.output, "\"ruleId\": \"CompilerDiagnostic\"",
                                "sarif compile-error output") &&
                 ok;
            ok = assertContains(result.output, "\"uri\": ", "sarif compile-error output") && ok;
            ok = assertContains(result.output, "tests/fixtures/invalid.c",
                                "sarif compile-error output") &&
                 ok;
            ok = assertContains(result.output, "\"startLine\": 2", "sarif compile-error output") &&
                 ok;
            ok =
                assertContains(result.output, "\"startColumn\": 1", "sarif compile-error output") &&
                ok;
        }

        return ok;
    }

    bool testRuleSelectionAndNewChecks()
    {
        bool ok = true;

        // The default runs every rule, including the two that were once left out of it (#53).
        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/signal/signal_handler_unsafe_call.c").string(),
                 "--analyze", "--fail-on=error"});
            ok = assertTrue(result.exitCode == kFindingsExitCode,
                            "a default run gated on errors should fail on the unsafe handler") &&
                 ok;
            ok = assertContains(result.output, "ruleId: UnsafeSignalHandler",
                                "default unsafe-signal-handler output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/thread-escape/thread_argument_stack_escape.c").string(),
                 "--analyze"});
            ok = assertTrue(result.exitCode == 0, "default thread-arg-escape run should succeed") &&
                 ok;
            ok = assertContains(result.output, "ruleId: ThreadArgumentEscapesFrame",
                                "default thread-arg-escape output") &&
                 ok;
        }

        // An explicit selection replaces the default rather than adding to it.
        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/signal/signal_handler_unsafe_call.c").string(),
                 "--analyze", "--rules=data-race", "--fail-on=error"});
            ok = assertTrue(result.exitCode == 0,
                            "--rules=data-race should not gate on the unsafe handler") &&
                 ok;
            ok = assertNotContains(result.output, "ruleId: UnsafeSignalHandler",
                                   "data-race-only signal output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/missing-join/missing_join_basic.c").string(),
                             "--analyze"});
            ok =
                assertTrue(result.exitCode == 0, "default --analyze should include missing-join") &&
                ok;
            ok = assertContains(result.output, "ruleId: MissingJoin",
                                "default missing-join output") &&
                 ok;
            ok = assertContains(result.output, "handle kind: pthread",
                                "default missing-join output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/data-race/data_race_basic.c").string(),
                             "--analyze", "--rules=data-race"});
            ok = assertTrue(result.exitCode == 0, "--rules=data-race should succeed") && ok;
            ok = assertContains(result.output, "ruleId: DataRaceGlobal", "data-race-only output") &&
                 ok;
            ok = assertNotContains(result.output, "ruleId: MissingJoin", "data-race-only output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/missing-join/missing_join_basic.c").string(),
                             "--analyze", "--rules=missing-join"});
            ok = assertTrue(result.exitCode == 0, "--rules=missing-join should succeed") && ok;
            ok = assertContains(result.output, "ruleId: MissingJoin", "missing-join-only output") &&
                 ok;
            ok = assertNotContains(result.output, "ruleId: DataRaceGlobal",
                                   "missing-join-only output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/missing-join/missing_join_multiple.c").string(),
                 "--analyze", "--rules=data-race,missing-join"});
            ok = assertTrue(result.exitCode == 0, "multi-rule selection should succeed") && ok;
            ok = assertContains(result.output, "ruleId: DataRaceGlobal",
                                "multi-rule selection output") &&
                 ok;
            ok = assertContains(result.output, "ruleId: MissingJoin",
                                "multi-rule selection output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/deadlock/deadlock_basic.c").string(), "--analyze"});
            ok = assertTrue(result.exitCode == 0,
                            "default --analyze should include deadlock detection") &&
                 ok;
            ok = assertContains(result.output, "ruleId: DeadlockLockOrder",
                                "default deadlock output") &&
                 ok;
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("concurrency/deadlock/recursive_deadlock.c").string(),
                             "--analyze", "--rules=deadlock-lock-order"});
            ok = assertTrue(result.exitCode == 0, "recursive deadlock analysis should succeed") &&
                 ok;
            ok = assertContains(result.output, "ruleId: DeadlockLockOrder",
                                "recursive deadlock output") &&
                 ok;
            ok = assertContains(result.output,
                                "potential deadlock caused by reacquiring a non-recursive lock",
                                "recursive deadlock output") &&
                 ok;
            ok =
                assertContains(result.output, "helper_function", "recursive deadlock output") && ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/missing-join/cpp_std_thread_missing_join.cpp").string(),
                 "--analyze", "--rules=missing-join"});
            ok =
                assertTrue(result.exitCode == 0, "std::thread missing-join analysis should pass") &&
                ok;
            ok = assertContains(result.output, "ruleId: MissingJoin",
                                "std::thread missing-join output") &&
                 ok;
            ok = assertContains(result.output, "handle kind: std::thread",
                                "std::thread missing-join output") &&
                 ok;
        }

        {
            const RunResult result = runAnalyzer(
                {fixturePath("concurrency/data-race/data_race_callsite_lock_protected.c").string(),
                 "--analyze", "--rules=data-race"});
            ok = assertTrue(result.exitCode == 0,
                            "callsite-protected helper analysis should succeed") &&
                 ok;
            ok = assertContains(result.output, "Diagnostics summary: info=0, warning=0, error=0",
                                "callsite-protected helper output") &&
                 ok;
            ok = assertNotContains(result.output, "ruleId: DataRaceGlobal",
                                   "callsite-protected helper output") &&
                 ok;
        }

        return ok;
    }

    bool testInputValidationFailuresAndBackendDiagnostics()
    {
        bool ok = true;

        {
            const RunResult result = runAnalyzer(
                {"/tmp/_coretrace_concurrency_missing_fixture_xyz.c", "--ir-format=ll"});
            ok = assertTrue(result.exitCode == 1, "missing input should fail") && ok;
            ok =
                assertContains(result.output, "input_file_does_not_exist", "missing file output") &&
                ok;
        }

        {
            const std::filesystem::path tempDir = makeTempDir("ctrace-cli-dir");
            ok =
                assertTrue(!tempDir.empty(), "temp dir creation failed for directory-input test") &&
                ok;
            const RunResult result = runAnalyzer({tempDir.string(), "--ir-format=ll"});
            ok = assertTrue(result.exitCode == 1, "directory input should fail") && ok;
            ok =
                assertContains(result.output, "input_file_not_regular", "directory input output") &&
                ok;
            std::error_code ec;
            std::filesystem::remove_all(tempDir, ec);
        }

        {
            const RunResult result =
                runAnalyzer({fixturePath("invalid.c").string(), "--ir-format=ll"});
            ok = assertTrue(result.exitCode == 1, "invalid C source should fail") && ok;
            ok = assertContains(result.output, "backend_compile:", "invalid source output") && ok;
        }

        {
            // Bitcode is compiled in memory: no temporary file is needed, so a TMPDIR nothing can
            // be created in does not stop it. The source includes no system header, because on
            // macOS the sysroot probe does use the temporary directory.
            const RunResult result = runAnalyzer(
                {fixturePath("empty.c").string(), "--ir-format=bc"}, "TMPDIR=/dev/null");
            ok = assertTrue(result.exitCode == 0,
                            "an unusable TMPDIR should not stop a bitcode compilation\noutput:\n" +
                                result.output) &&
                 ok;
        }

        return ok;
    }

    /// No mode of compilation writes a temporary bitcode file: with a TMPDIR nothing can be
    /// created in, every one of them still succeeds, textual or bitcode, instrumented or not, and
    /// a project analysis without the cache. This says nothing of other disk access: the compiler
    /// still reads sources and headers, and the macOS sysroot detection uses the temporary
    /// directory, which is why the source includes no system header: under an unusable TMPDIR on
    /// macOS, a source that does include one fails, while the next units, once the directory is
    /// usable again, detect the sysroot anew (coretrace-compiler#98).
    bool testCompilationNeedsNoTemporaryBitcodeFile()
    {
        bool ok = true;
        const std::string source = fixturePath("empty.c").string();
        for (const std::string format : {"--ir-format=ll", "--ir-format=bc"})
        {
            for (const bool instrument : {false, true})
            {
                std::vector<std::string> args = {source, format};
                if (instrument)
                    args.push_back("--instrument");
                const RunResult result = runAnalyzer(args, "TMPDIR=/dev/null");
                ok = assertTrue(result.exitCode == 0,
                                format + (instrument ? " --instrument" : "") +
                                    " should not need a temporary file\noutput:\n" +
                                    result.output) &&
                     ok;
            }
        }

        const std::filesystem::path projectDir = makeTempDir("ctrace-cli-no-temp");
        if (!assertTrue(!projectDir.empty(), "project directory should be created"))
            return false;
        const std::filesystem::path database = projectDir / "compile_commands.json";
        {
            std::ofstream out(database);
            out << "[{\"directory\": \"" << projectDir.string() << "\", \"file\": \"" << source
                << "\", \"arguments\": [\"clang\", \"-c\", \"" << source
                << "\", \"-o\", \"empty.o\"]}]\n";
        }
        const RunResult project = runAnalyzer(
            {"--compile-commands=" + database.string(), "--no-cache"}, "TMPDIR=/dev/null");
        ok = assertTrue(project.exitCode == 0,
                        "a project analysis without the cache should not need a temporary "
                        "file\noutput:\n" +
                            project.output) &&
             ok;

        std::error_code ec;
        std::filesystem::remove_all(projectDir, ec);
        return ok;
    }

    /// A project whose compile database sits in its own directory, with the cache beside it:
    /// the two units of cross-tu-data-race, which race only when read together, and a generated
    /// unit holding more than a megabyte of bitcode, so that `bitcode-mb` tells bytes kept in
    /// memory from none.
    std::filesystem::path writeCacheProject(const std::filesystem::path& directory)
    {
        const std::filesystem::path big = directory / "big.c";
        {
            std::ofstream out(big);
            out << "static const char blob[] = \"" << std::string(1500000, 'x') << "\";\n"
                << "int read_blob(int index) { return blob[index]; }\n";
        }

        const std::vector<std::filesystem::path> sources = {
            fixturePath("concurrency-project/cross-tu-data-race/main.c"),
            fixturePath("concurrency-project/cross-tu-data-race/worker.c"), big};
        const std::filesystem::path database = directory / "compile_commands.json";
        std::ofstream out(database);
        out << "[";
        for (std::size_t index = 0; index < sources.size(); ++index)
        {
            const std::string source = sources[index].string();
            out << (index == 0 ? "" : ",") << "{\"directory\": \"" << directory.string()
                << "\", \"file\": \"" << source << "\", \"arguments\": [\"clang\", \"-c\", \""
                << source << "\", \"-o\", \"unit" << index << ".o\"]}";
        }
        out << "]\n";
        return database;
    }

    /// The JSON report without its `meta` object, which records timings: what two runs must
    /// agree on.
    std::string reportWithoutMeta(const std::string& output)
    {
        const std::size_t begin = output.find("{\n  \"diagnostics\"");
        const std::size_t end = output.find("\"meta\"", begin);
        if (begin == std::string::npos || end == std::string::npos)
            return {};
        return output.substr(begin, end - begin);
    }

    /// The value `--verbose` printed for `key`, or -1 when it printed none.
    long verboseNumber(const std::string& output, std::string_view key)
    {
        const std::string label = std::string(key) + ": ";
        const std::size_t at = output.find(label);
        if (at == std::string::npos)
            return -1;
        return std::stol(output.substr(at + label.size()));
    }

    /// With the cache, a project unit's bitcode stays in its cache entry and is read from there
    /// when the unit is analysed, whether the run compiled it or reused it: none is kept in
    /// memory. Without the cache it is kept in memory. The reports are the same either way (#51).
    bool testCachedProjectKeepsNoBitcodeInMemory()
    {
        const std::filesystem::path directory = makeTempDir("ctrace-cli-cache-memory");
        if (!assertTrue(!directory.empty(), "project directory should be created"))
            return false;
        const std::string database = "--compile-commands=" + writeCacheProject(directory).string();

        const RunResult noCache =
            runAnalyzer({database, "--no-cache", "--verbose", "--format=json"});
        const RunResult first = runAnalyzer({database, "--verbose", "--format=json"});
        const RunResult second = runAnalyzer({database, "--verbose", "--format=json"});

        const std::string expected = reportWithoutMeta(noCache.output);
        bool ok = assertTrue(noCache.exitCode == 0 && first.exitCode == 0 && second.exitCode == 0,
                             "the three runs should succeed\noutput:\n" + noCache.output +
                                 first.output + second.output) &&
                  assertTrue(expected.find("shared_counter") != std::string::npos,
                             "the reference report should hold the cross-unit race");
        ok = assertTrue(verboseNumber(noCache.output, "bitcode-mb") >= 1,
                        "without the cache the bitcode is kept in memory") &&
             ok;
        ok = assertContains(first.output, "units: 3 compiled, 0 reused", "first cached run") &&
             assertTrue(verboseNumber(first.output, "bitcode-mb") == 0,
                        "units compiled and stored are read back from the cache") &&
             ok;
        ok = assertContains(second.output, "units: 0 compiled, 3 reused", "second cached run") &&
             assertTrue(verboseNumber(second.output, "bitcode-mb") == 0,
                        "units reused are read from the cache") &&
             ok;
        ok = assertTrue(reportWithoutMeta(first.output) == expected &&
                            reportWithoutMeta(second.output) == expected,
                        "cached runs report exactly what the run without the cache reports") &&
             ok;

        std::error_code ec;
        std::filesystem::remove_all(directory, ec);
        return ok;
    }

    /// Every file of a directory, by name, with its contents.
    std::vector<std::pair<std::string, std::string>> snapshot(const std::filesystem::path& root)
    {
        std::vector<std::pair<std::string, std::string>> files;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(root, ec))
        {
            std::ifstream in(entry.path(), std::ios::binary);
            files.emplace_back(
                entry.path().filename().string(),
                std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()));
        }
        std::sort(files.begin(), files.end());
        return files;
    }

    /// `--no-cache` neither writes the cache nor reads it. Not writing: a run creates no cache
    /// directory, and leaves an existing one byte for byte as it was. Not reading: an entry is
    /// poisoned with another unit's bitcode, which a cached run does pick up, and a run without
    /// the cache still reports what it reported before. This is a guarantee about the cache
    /// only; compiling still reads sources and headers (#51).
    bool testNoCacheNeitherReadsNorWritesTheCache()
    {
        const std::filesystem::path directory = makeTempDir("ctrace-cli-no-cache");
        if (!assertTrue(!directory.empty(), "project directory should be created"))
            return false;
        const std::string database = "--compile-commands=" + writeCacheProject(directory).string();
        const std::filesystem::path cacheDir = directory / ".coretrace-ir-cache";

        const RunResult before = runAnalyzer({database, "--no-cache", "--format=json"});
        const std::string expected = reportWithoutMeta(before.output);
        bool ok = assertTrue(before.exitCode == 0 && !expected.empty(),
                             "a run without the cache should succeed\noutput:\n" + before.output) &&
                  assertTrue(!std::filesystem::exists(cacheDir),
                             "a run without the cache should create no cache");

        const RunResult cached = runAnalyzer({database, "--format=json"});
        ok = assertTrue(reportWithoutMeta(cached.output) == expected,
                        "a cached run should fill the cache and report the same") &&
             ok;

        // main.c's entry now holds worker.c's bitcode; the dependency lists still vouch for it.
        std::filesystem::path mainEntry;
        std::filesystem::path workerEntry;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(cacheDir, ec))
        {
            if (entry.path().extension() != ".deps")
                continue;
            std::ifstream in(entry.path());
            const std::string deps((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
            std::filesystem::path bitcode = entry.path();
            bitcode.replace_extension(".bc");
            if (deps.find("cross-tu-data-race/main.c") != std::string::npos)
                mainEntry = bitcode;
            else if (deps.find("cross-tu-data-race/worker.c") != std::string::npos)
                workerEntry = bitcode;
        }
        if (!assertTrue(!mainEntry.empty() && !workerEntry.empty(),
                        "the cache should hold an entry for each unit"))
            return false;
        std::filesystem::copy_file(workerEntry, mainEntry,
                                   std::filesystem::copy_options::overwrite_existing, ec);

        const RunResult poisoned = runAnalyzer({database, "--format=json"});
        ok = assertTrue(reportWithoutMeta(poisoned.output) != expected,
                        "a cached run should read the poisoned entry") &&
             ok;

        const auto cacheBefore = snapshot(cacheDir);
        const RunResult after = runAnalyzer({database, "--no-cache", "--format=json"});
        ok = assertTrue(reportWithoutMeta(after.output) == expected,
                        "a run without the cache should not read the poisoned entry") &&
             assertTrue(snapshot(cacheDir) == cacheBefore,
                        "a run without the cache should leave the cache as it was") &&
             ok;

        std::filesystem::remove_all(directory, ec);
        return ok;
    }

    bool testPermissionRelatedInputFailures()
    {
        bool ok = true;

        {
            const std::filesystem::path tempDir = makeTempDir("ctrace-cli-unreadable");
            ok =
                assertTrue(!tempDir.empty(), "temp dir creation failed for unreadable-file test") &&
                ok;

            const std::filesystem::path unreadableFile = tempDir / "unreadable.c";
            std::error_code copyEc;
            std::filesystem::copy_file(fixturePath("hello.c"), unreadableFile, copyEc);
            ok = assertTrue(!copyEc, "failed to create unreadable file fixture") && ok;

            std::error_code permEc;
            std::filesystem::permissions(unreadableFile, std::filesystem::perms::none,
                                         std::filesystem::perm_options::replace, permEc);
            ok = assertTrue(!permEc, "failed to chmod unreadable fixture to 000") && ok;

            const RunResult result = runAnalyzer({unreadableFile.string(), "--ir-format=ll"});
            ok = assertTrue(result.exitCode == 1, "unreadable file should fail") && ok;
            ok = assertContains(result.output, "input_file_not_readable",
                                "unreadable file output") &&
                 ok;

            std::filesystem::permissions(unreadableFile,
                                         std::filesystem::perms::owner_read |
                                             std::filesystem::perms::owner_write,
                                         std::filesystem::perm_options::replace, permEc);
            std::filesystem::remove_all(tempDir, permEc);
        }

        {
            const std::filesystem::path tempDir = makeTempDir("ctrace-cli-locked");
            ok = assertTrue(!tempDir.empty(), "temp dir creation failed for access-failure test") &&
                 ok;

            const std::filesystem::path lockedDir = tempDir / "locked";
            const std::filesystem::path lockedFile = lockedDir / "source.c";
            std::error_code ec;
            std::filesystem::create_directory(lockedDir, ec);
            ok = assertTrue(!ec, "failed to create locked directory") && ok;

            std::ofstream out(lockedFile);
            out << "int main(){return 0;}\n";
            out.close();

            std::filesystem::permissions(
                lockedDir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                std::filesystem::perm_options::replace, ec);
            ok = assertTrue(!ec, "failed to chmod locked directory") && ok;

            const RunResult result = runAnalyzer({lockedFile.string(), "--ir-format=ll"});
            ok = assertTrue(result.exitCode == 1,
                            "locked directory path should fail with access error") &&
                 ok;
            ok = assertContains(result.output, "input_file_access_failed",
                                "locked directory output") &&
                 ok;

            std::filesystem::permissions(lockedDir, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, ec);
            std::filesystem::remove_all(tempDir, ec);
        }

        return ok;
    }
} // namespace

int main()
{
    bool ok = true;

    ok = assertTrue(std::filesystem::exists(analyzerBinaryPath()), "analyzer binary is missing") &&
         ok;
    ok = testHelpAndInputParsingErrors() && ok;
    ok = testSuccessfulCompilesAndVerboseMode() && ok;
    ok = testAnalyzeMode() && ok;
    ok = testRuleSelectionAndNewChecks() && ok;
    ok = testInputValidationFailuresAndBackendDiagnostics() && ok;
    ok = testPermissionRelatedInputFailures() && ok;
    ok = testCompilationNeedsNoTemporaryBitcodeFile() && ok;
    ok = testCachedProjectKeepsNoBitcodeInMemory() && ok;
    ok = testNoCacheNeitherReadsNorWritesTheCache() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] cli integration cpp tests\n";
    return 0;
}
