// SPDX-License-Identifier: Apache-2.0
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
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

    bool testHelpAndInputParsingErrors()
    {
        bool ok = true;

        {
            const RunResult result = runAnalyzer({"--help"});
            ok = assertTrue(result.exitCode == 0, "--help should exit with code 0") && ok;
            ok = assertContains(result.output, "Usage:", "--help output") && ok;
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
            const RunResult result = runAnalyzer(
                {fixturePath("hello.c").string(), "--ir-format=bc"}, "TMPDIR=/dev/null");
            ok = assertTrue(result.exitCode == 1,
                            "invalid TMPDIR should fail temporary bitcode creation") &&
                 ok;
            ok = assertContains(result.output, "temporary_bitcode_file_creation_failed",
                                "TMPDIR failure output") &&
                 ok;
        }

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
    ok = testInputValidationFailuresAndBackendDiagnostics() && ok;
    ok = testPermissionRelatedInputFailures() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] cli integration cpp tests\n";
    return 0;
}
