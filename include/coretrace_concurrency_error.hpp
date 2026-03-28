#pragma once

#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ctrace::concurrency
{
    enum class CompilePhase
    {
        None = 0,
        ValidateInput,
        BuildCommand,
        BackendCompile,
        IRParse,
    };

    enum class CompileErrc
    {
        Success = 0,
        InvalidRequest,
        InputFileAccessFailed,
        InputFileDoesNotExist,
        InputFileNotRegular,
        InputFileNotReadable,
        TemporaryBitcodeFileCreationFailed,
        BackendCompilationFailed,
        MissingIROutput,
        BitcodeReadFailed,
        IRParseFailed,
        BitcodeParseFailed,
    };

    struct CompileError
    {
        std::error_code code{};
        CompilePhase phase = CompilePhase::None;
        std::string message;

        [[nodiscard]] bool hasError() const noexcept
        {
            return static_cast<bool>(code);
        }
    };

    [[nodiscard]] const std::error_category& compileErrorCategory() noexcept;
    [[nodiscard]] std::error_code make_error_code(CompileErrc error) noexcept;
    [[nodiscard]] std::string_view toString(CompilePhase phase) noexcept;
    [[nodiscard]] std::string_view toString(CompileErrc error) noexcept;
    [[nodiscard]] std::string formatCompileError(const CompileError& error);
} // namespace ctrace::concurrency

namespace std
{
    template <> struct is_error_code_enum<ctrace::concurrency::CompileErrc> : true_type
    {
    };
} // namespace std
