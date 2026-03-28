#include "coretrace_concurrency_analyzer.hpp"

#include "internal/compile_command_builder.hpp"
#include "internal/compilation_backend.hpp"
#include "internal/ir_loader.hpp"
#include "internal/temporary_bitcode_file.hpp"

#include <llvm/IR/LLVMContext.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
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

        bool readBinaryFile(const std::filesystem::path& path, std::string& out)
        {
            out.clear();
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in)
                return false;

            in.seekg(0, std::ios::end);
            if (!in)
                return false;

            const std::streampos endPos = in.tellg();
            if (endPos < 0)
                return false;

            const std::size_t byteCount = static_cast<std::size_t>(endPos);
            if (byteCount > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
                return false;

            out.resize(byteCount);
            in.seekg(0, std::ios::beg);
            if (!in)
            {
                out.clear();
                return false;
            }

            const std::streamsize expected = static_cast<std::streamsize>(out.size());
            in.read(out.data(), expected);
            if (in.gcount() != expected || in.bad())
            {
                out.clear();
                return false;
            }

            return true;
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

        const std::optional<internal::TemporaryBitcodeFile> bitcodeFile =
            internal::TemporaryBitcodeFile::create(result.error);
        if (!bitcodeFile)
            return result;

        const std::vector<std::string> args =
            internal::CompileCommandBuilder::buildBC(request, bitcodeFile->path());
        internal::BackendCompileOutput backendResult =
            backend_->compileBCToFile(args, request.instrument);
        result.diagnostics = std::move(backendResult.diagnostics);

        if (!backendResult.success)
        {
            setCompileError(result, CompilePhase::BackendCompile,
                            CompileErrc::BackendCompilationFailed, "backend failed in bc mode");
            return result;
        }

        if (!readBinaryFile(bitcodeFile->path(), result.llvmBitcode))
        {
            setCompileError(result, CompilePhase::BackendCompile, CompileErrc::BitcodeReadFailed,
                            bitcodeFile->path().string());
            return result;
        }

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
