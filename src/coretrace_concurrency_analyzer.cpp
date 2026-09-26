// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analyzer.hpp"

#include "internal/compile_command_builder.hpp"
#include "internal/compilation_backend.hpp"
#include "internal/ir_loader.hpp"

#include <llvm/IR/LLVMContext.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace ctrace::concurrency
{
    namespace
    {
        void setCompileError(CompileResult& result, CompilePhase phase, CompileErrc code,
                             std::string message = {})
        {
            result.error.code = make_error_code(code);
            result.error.phase = phase;
            result.error.message = std::move(message);
        }

        bool validateInputFile(const std::string& inputFile, CompileError& error)
        {
            const std::filesystem::path inputPath(inputFile);
            std::error_code ec;

            if (!std::filesystem::exists(inputPath, ec))
            {
                error.phase = CompilePhase::ValidateInput;
                if (ec)
                {
                    error.code = make_error_code(CompileErrc::InputFileAccessFailed);
                    error.message = "'" + inputFile + "': " + ec.message();
                }
                else
                {
                    error.code = make_error_code(CompileErrc::InputFileDoesNotExist);
                    error.message = inputFile;
                }
                return false;
            }

            if (!std::filesystem::is_regular_file(inputPath, ec))
            {
                error.phase = CompilePhase::ValidateInput;
                if (ec)
                {
                    error.code = make_error_code(CompileErrc::InputFileAccessFailed);
                    error.message = "'" + inputFile + "': " + ec.message();
                }
                else
                {
                    error.code = make_error_code(CompileErrc::InputFileNotRegular);
                    error.message = inputFile;
                }
                return false;
            }

            std::ifstream probe(inputPath, std::ios::in | std::ios::binary);
            if (!probe.good())
            {
                error.code = make_error_code(CompileErrc::InputFileNotReadable);
                error.phase = CompilePhase::ValidateInput;
                error.message = inputFile;
                return false;
            }

            return true;
        }
    } // namespace

    InMemoryIRCompiler::InMemoryIRCompiler()
        : backend_(std::make_shared<internal::CompilerLibBackend>()),
          irLoader_(std::make_shared<internal::LLVMIRLoader>())
    {
    }

    InMemoryIRCompiler::InMemoryIRCompiler(std::shared_ptr<internal::ICompilationBackend> backend,
                                           std::shared_ptr<internal::IIRLoader> irLoader)
        : backend_(std::move(backend)), irLoader_(std::move(irLoader))
    {
        if (!backend_)
            backend_ = std::make_shared<internal::CompilerLibBackend>();
        if (!irLoader_)
            irLoader_ = std::make_shared<internal::LLVMIRLoader>();
    }

    CompileResult InMemoryIRCompiler::compile(const CompileRequest& request,
                                              llvm::LLVMContext& context) const
    {
        CompileResult result;

        if (request.inputFile.empty())
        {
            setCompileError(result, CompilePhase::ValidateInput, CompileErrc::InvalidRequest,
                            "input file is required");
            return result;
        }

        if (!validateInputFile(request.inputFile, result.error))
            return result;

        if (request.format == IRFormat::LL)
        {
            const std::vector<std::string> args = internal::CompileCommandBuilder::buildLL(request);
            internal::BackendCompileOutput backendResult =
                backend_->compileLLToMemory(args, request.instrument);
            result.diagnostics = std::move(backendResult.diagnostics);

            if (!backendResult.success)
            {
                setCompileError(result, CompilePhase::BackendCompile,
                                CompileErrc::BackendCompilationFailed, "backend failed in ll mode");
                return result;
            }

            if (backendResult.llvmIR.empty())
            {
                setCompileError(result, CompilePhase::BackendCompile, CompileErrc::MissingIROutput,
                                "compiler backend returned an empty IR payload in ll mode");
                return result;
            }

            result.llvmIRText = std::move(backendResult.llvmIR);
            // parseLL wraps llvmIRText with a non-owning memory buffer.
            // Keep llvmIRText alive for the full duration of this call.
            result.module = irLoader_->parseLL(result.llvmIRText, context, result.error);
            if (!result.module)
            {
                if (!result.error.hasError())
                {
                    setCompileError(result, CompilePhase::IRParse, CompileErrc::IRParseFailed,
                                    "failed to parse LLVM IR in ll mode");
                }
                return result;
            }

            result.success = true;
            return result;
        }

        // No file is written for the bitcode here, temporary or not: the backend returns the bytes
        // an output file would hold. Keeping them afterwards, as the IR cache does, is the
        // caller's choice.
        const std::vector<std::string> args = internal::CompileCommandBuilder::buildBC(request);
        internal::BackendCompileOutput backendResult =
            backend_->compileBCToMemory(args, request.instrument);
        result.diagnostics = std::move(backendResult.diagnostics);

        if (!backendResult.success)
        {
            setCompileError(result, CompilePhase::BackendCompile,
                            CompileErrc::BackendCompilationFailed, "backend failed in bc mode");
            return result;
        }

        if (backendResult.llvmBitcode.empty())
        {
            setCompileError(result, CompilePhase::BackendCompile, CompileErrc::MissingIROutput,
                            "compiler backend returned an empty bitcode payload in bc mode");
            return result;
        }

        result.llvmBitcode = std::move(backendResult.llvmBitcode);

        result.module = irLoader_->parseBC(result.llvmBitcode, context, result.error);
        if (!result.module)
        {
            if (!result.error.hasError())
            {
                setCompileError(result, CompilePhase::IRParse, CompileErrc::BitcodeParseFailed,
                                "failed to parse LLVM bitcode in bc mode");
            }
            return result;
        }

        result.success = true;
        return result;
    }
} // namespace ctrace::concurrency
