// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_error.hpp"

namespace ctrace::concurrency
{
    namespace
    {
        class CompileErrorCategory final : public std::error_category
        {
          public:
            const char* name() const noexcept override
            {
                return "coretrace.concurrency.compile";
            }

            std::string message(int condition) const override
            {
                switch (static_cast<CompileErrc>(condition))
                {
                case CompileErrc::Success:
                    return "success";
                case CompileErrc::InvalidRequest:
                    return "invalid compile request";
                case CompileErrc::InputFileAccessFailed:
                    return "failed to access input file";
                case CompileErrc::InputFileDoesNotExist:
                    return "input file does not exist";
                case CompileErrc::InputFileNotRegular:
                    return "input path is not a regular file";
                case CompileErrc::InputFileNotReadable:
                    return "input file is not readable";
                case CompileErrc::TemporaryBitcodeFileCreationFailed:
                    return "failed to create temporary bitcode file";
                case CompileErrc::BackendCompilationFailed:
                    return "compiler backend failed";
                case CompileErrc::MissingIROutput:
                    return "compiler backend did not produce expected LLVM IR output";
                case CompileErrc::BitcodeReadFailed:
                    return "failed to read bitcode output";
                case CompileErrc::IRParseFailed:
                    return "failed to parse LLVM IR";
                case CompileErrc::BitcodeParseFailed:
                    return "failed to parse LLVM bitcode";
                }
                return "unknown compile error";
            }
        };
    } // namespace

    const std::error_category& compileErrorCategory() noexcept
    {
        static CompileErrorCategory category;
        return category;
    }

    std::error_code make_error_code(CompileErrc error) noexcept
    {
        return {static_cast<int>(error), compileErrorCategory()};
    }

    std::string_view toString(CompilePhase phase) noexcept
    {
        switch (phase)
        {
        case CompilePhase::None:
            return "none";
        case CompilePhase::ValidateInput:
            return "validate_input";
        case CompilePhase::BuildCommand:
            return "build_command";
        case CompilePhase::BackendCompile:
            return "backend_compile";
        case CompilePhase::IRParse:
            return "ir_parse";
        }
        return "unknown_phase";
    }

    std::string_view toString(CompileErrc error) noexcept
    {
        switch (error)
        {
        case CompileErrc::Success:
            return "success";
        case CompileErrc::InvalidRequest:
            return "invalid_request";
        case CompileErrc::InputFileAccessFailed:
            return "input_file_access_failed";
        case CompileErrc::InputFileDoesNotExist:
            return "input_file_does_not_exist";
        case CompileErrc::InputFileNotRegular:
            return "input_file_not_regular";
        case CompileErrc::InputFileNotReadable:
            return "input_file_not_readable";
        case CompileErrc::TemporaryBitcodeFileCreationFailed:
            return "temporary_bitcode_file_creation_failed";
        case CompileErrc::BackendCompilationFailed:
            return "backend_compilation_failed";
        case CompileErrc::MissingIROutput:
            return "missing_ir_output";
        case CompileErrc::BitcodeReadFailed:
            return "bitcode_read_failed";
        case CompileErrc::IRParseFailed:
            return "ir_parse_failed";
        case CompileErrc::BitcodeParseFailed:
            return "bitcode_parse_failed";
        }
        return "unknown_compile_error";
    }

    std::string formatCompileError(const CompileError& error)
    {
        if (!error.code)
            return {};

        std::string codeName;
        if (error.code.category() == compileErrorCategory())
            codeName = std::string(toString(static_cast<CompileErrc>(error.code.value())));
        else
            codeName = error.code.category().name();

        std::string formatted;
        if (error.phase != CompilePhase::None)
            formatted = std::string(toString(error.phase)) + ": ";
        formatted += codeName + ": " + error.code.message();
        if (!error.message.empty())
            formatted += " (" + error.message + ")";
        return formatted;
    }
} // namespace ctrace::concurrency
