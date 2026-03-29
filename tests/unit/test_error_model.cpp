// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_error.hpp"

#include <cerrno>
#include <iostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
    using ctrace::concurrency::CompileErrc;
    using ctrace::concurrency::CompileError;
    using ctrace::concurrency::compileErrorCategory;
    using ctrace::concurrency::CompilePhase;
    using ctrace::concurrency::formatCompileError;
    using ctrace::concurrency::make_error_code;
    using ctrace::concurrency::toString;

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    bool testCompilePhaseToStringCoverage()
    {
        const std::vector<std::pair<CompilePhase, std::string>> expected = {
            {CompilePhase::None, "none"},
            {CompilePhase::ValidateInput, "validate_input"},
            {CompilePhase::BuildCommand, "build_command"},
            {CompilePhase::BackendCompile, "backend_compile"},
            {CompilePhase::IRParse, "ir_parse"},
        };

        bool ok = true;
        for (const auto& [phase, text] : expected)
            ok = assertTrue(toString(phase) == text, "unexpected CompilePhase string") && ok;

        ok = assertTrue(toString(static_cast<CompilePhase>(9999)) == "unknown_phase",
                        "unknown CompilePhase should map to unknown_phase") &&
             ok;
        return ok;
    }

    bool testCompileErrcToStringAndCategoryMessages()
    {
        const std::vector<std::pair<CompileErrc, std::string>> expected = {
            {CompileErrc::Success, "success"},
            {CompileErrc::InvalidRequest, "invalid_request"},
            {CompileErrc::InputFileAccessFailed, "input_file_access_failed"},
            {CompileErrc::InputFileDoesNotExist, "input_file_does_not_exist"},
            {CompileErrc::InputFileNotRegular, "input_file_not_regular"},
            {CompileErrc::InputFileNotReadable, "input_file_not_readable"},
            {CompileErrc::TemporaryBitcodeFileCreationFailed,
             "temporary_bitcode_file_creation_failed"},
            {CompileErrc::BackendCompilationFailed, "backend_compilation_failed"},
            {CompileErrc::MissingIROutput, "missing_ir_output"},
            {CompileErrc::BitcodeReadFailed, "bitcode_read_failed"},
            {CompileErrc::IRParseFailed, "ir_parse_failed"},
            {CompileErrc::BitcodeParseFailed, "bitcode_parse_failed"},
        };

        bool ok = true;
        for (const auto& [errc, text] : expected)
        {
            ok = assertTrue(toString(errc) == text, "unexpected CompileErrc string") && ok;

            const std::error_code ec = make_error_code(errc);
            ok = assertTrue(ec.category() == compileErrorCategory(),
                            "CompileErrc should use compileErrorCategory") &&
                 ok;
            ok = assertTrue(!ec.message().empty(), "CompileErrc message should not be empty") && ok;
        }

        ok = assertTrue(toString(static_cast<CompileErrc>(7777)) == "unknown_compile_error",
                        "unknown CompileErrc should map to unknown_compile_error") &&
             ok;

        const std::error_code unknownEc = make_error_code(static_cast<CompileErrc>(7777));
        ok = assertTrue(unknownEc.message() == "unknown compile error",
                        "unknown CompileErrc message should be stable") &&
             ok;

        ok = assertTrue(std::string(compileErrorCategory().name()) ==
                            "coretrace.concurrency.compile",
                        "compileErrorCategory name should be stable") &&
             ok;
        return ok;
    }

    bool testFormatCompileErrorVariants()
    {
        bool ok = true;

        {
            CompileError noError;
            ok = assertTrue(formatCompileError(noError).empty(),
                            "formatCompileError should return empty string when no error is set") &&
                 ok;
        }

        {
            CompileError error;
            error.code = make_error_code(CompileErrc::InputFileDoesNotExist);
            error.phase = CompilePhase::ValidateInput;
            error.message = "/tmp/missing.c";

            const std::string formatted = formatCompileError(error);
            ok = assertTrue(formatted.find("validate_input:") != std::string::npos,
                            "formatted error should include phase prefix") &&
                 ok;
            ok = assertTrue(formatted.find("input_file_does_not_exist:") != std::string::npos,
                            "formatted error should include compile error code name") &&
                 ok;
            ok = assertTrue(formatted.find("/tmp/missing.c") != std::string::npos,
                            "formatted error should include contextual message") &&
                 ok;
        }

        {
            CompileError error;
            error.code = make_error_code(CompileErrc::BackendCompilationFailed);
            error.phase = CompilePhase::None;
            const std::string formatted = formatCompileError(error);
            ok = assertTrue(formatted.find("backend_compile") == std::string::npos,
                            "formatted error should not include phase prefix when phase is None") &&
                 ok;
            ok = assertTrue(formatted.find("backend_compilation_failed:") != std::string::npos,
                            "formatted error should include compile error code name") &&
                 ok;
        }

        {
            CompileError externalCategoryError;
            externalCategoryError.code = std::make_error_code(std::errc::invalid_argument);
            externalCategoryError.phase = CompilePhase::BuildCommand;
            externalCategoryError.message = "bad argument";

            const std::string formatted = formatCompileError(externalCategoryError);
            const std::string categoryName = externalCategoryError.code.category().name();

            ok = assertTrue(formatted.find("build_command:") != std::string::npos,
                            "formatted external error should include phase prefix") &&
                 ok;
            ok = assertTrue(formatted.find(categoryName) != std::string::npos,
                            "formatted external error should include category name") &&
                 ok;
            ok = assertTrue(formatted.find("bad argument") != std::string::npos,
                            "formatted external error should include contextual message") &&
                 ok;
        }

        return ok;
    }
} // namespace

int main()
{
    bool ok = true;

    ok = testCompilePhaseToStringCoverage() && ok;
    ok = testCompileErrcToStringAndCategoryMessages() && ok;
    ok = testFormatCompileErrorVariants() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] error model tests\n";
    return 0;
}
