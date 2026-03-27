#include "coretrace_concurrency_analyzer.hpp"

#include <compilerlib/compiler.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ctrace::concurrency
{
    namespace
    {
        void appendIfMissing(std::vector<std::string>& args, const std::string& flag)
        {
            if (std::find(args.begin(), args.end(), flag) == args.end())
                args.push_back(flag);
        }

        void removeOutputPathArgs(std::vector<std::string>& args)
        {
            std::vector<std::string> filtered;
            filtered.reserve(args.size());

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const std::string& arg = args[i];

                if (arg == "-o")
                {
                    if (i + 1 < args.size())
                        ++i;
                    continue;
                }
                if (arg.rfind("-o=", 0) == 0)
                    continue;
                if (arg.size() > 2 && arg.rfind("-o", 0) == 0)
                    continue;

                filtered.push_back(arg);
            }

            args.swap(filtered);
        }

        std::vector<std::string> buildLLCompileArgs(const CompileRequest& request)
        {
            std::vector<std::string> args = request.extraCompileArgs;
            removeOutputPathArgs(args);
            appendIfMissing(args, "-emit-llvm");
            appendIfMissing(args, "-S");
            appendIfMissing(args, "-g");
            args.push_back(request.inputFile);
            return args;
        }

        std::vector<std::string> buildBCCompileArgs(const CompileRequest& request,
                                                    const std::filesystem::path& outputPath)
        {
            std::vector<std::string> args = request.extraCompileArgs;
            args.erase(std::remove(args.begin(), args.end(), "-S"), args.end());
            removeOutputPathArgs(args);
            appendIfMissing(args, "-emit-llvm");
            appendIfMissing(args, "-c");
            args.push_back("-o");
            args.push_back(outputPath.string());
            args.push_back(request.inputFile);
            return args;
        }

        class ScopedFileCleanup
        {
          public:
            explicit ScopedFileCleanup(std::filesystem::path path) : path_(std::move(path)) {}

            ~ScopedFileCleanup()
            {
                if (path_.empty())
                    return;
                std::error_code ec;
                std::filesystem::remove(path_, ec);
            }

            ScopedFileCleanup(const ScopedFileCleanup&) = delete;
            ScopedFileCleanup& operator=(const ScopedFileCleanup&) = delete;

          private:
            std::filesystem::path path_;
        };

        void setCompileError(CompileResult& result, CompileErrc code, std::string message = {})
        {
            result.error.code = make_error_code(code);
            result.error.message = std::move(message);
        }

        bool makeTempBitcodePath(std::filesystem::path& outputPath, CompileError& error)
        {
            llvm::SmallString<256> tempPath;
            if (std::error_code ec =
                    llvm::sys::fs::createTemporaryFile("coretrace-concurrency-ir", "bc", tempPath))
            {
                error.code = make_error_code(CompileErrc::TemporaryBitcodeFileCreationFailed);
                error.message = ec.message();
                return false;
            }

            outputPath = std::filesystem::path(std::string(tempPath.str()));
            return true;
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
            if (endPos <= 0)
                return false;

            const std::size_t byteCount = static_cast<std::size_t>(endPos);
            if (byteCount > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max()))
            {
                return false;
            }

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
            std::filesystem::path inputPath(inputFile);
            std::error_code ec;

            if (!std::filesystem::exists(inputPath, ec))
            {
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
                error.message = inputFile;
                return false;
            }

            return true;
        }
    } // namespace

    CompileResult InMemoryIRCompiler::compile(const CompileRequest& request,
                                              llvm::LLVMContext& context) const
    {
        CompileResult result;

        if (request.inputFile.empty())
        {
            setCompileError(result, CompileErrc::InvalidRequest, "input file is required");
            return result;
        }

        if (!validateInputFile(request.inputFile, result.error))
            return result;

        if (request.format == IRFormat::LL)
        {
            const std::vector<std::string> args = buildLLCompileArgs(request);
            compilerlib::CompileResult raw =
                compilerlib::compile(args, compilerlib::OutputMode::ToMemory, request.instrument);
            result.diagnostics = raw.diagnostics;

            if (!raw.success)
            {
                setCompileError(result, CompileErrc::BackendCompilationFailed,
                                "backend failed in ll mode");
                return result;
            }

            if (raw.llvmIR.empty())
            {
                setCompileError(result, CompileErrc::MissingIROutput,
                                "compilerlib returned an empty IR payload in ll mode");
                return result;
            }

            result.llvmIRText = raw.llvmIR;

            auto buffer = llvm::MemoryBuffer::getMemBuffer(result.llvmIRText, "in_memory_ll");
            llvm::SMDiagnostic diag;
            result.module = llvm::parseIR(buffer->getMemBufferRef(), diag, context);
            if (!result.module)
            {
                std::string message;
                llvm::raw_string_ostream os(message);
                diag.print("in_memory_ll", os);
                setCompileError(result, CompileErrc::IRParseFailed, os.str());
                return result;
            }

            result.success = true;
            return result;
        }

        std::filesystem::path bitcodeOutputPath;
        if (!makeTempBitcodePath(bitcodeOutputPath, result.error))
        {
            return result;
        }
        const ScopedFileCleanup bitcodeCleanup(bitcodeOutputPath);

        const std::vector<std::string> args = buildBCCompileArgs(request, bitcodeOutputPath);
        compilerlib::CompileResult raw =
            compilerlib::compile(args, compilerlib::OutputMode::ToFile, request.instrument);
        result.diagnostics = raw.diagnostics;

        if (!raw.success)
        {
            setCompileError(result, CompileErrc::BackendCompilationFailed,
                            "backend failed in bc mode");
            return result;
        }

        if (!readBinaryFile(bitcodeOutputPath, result.llvmBitcode) || result.llvmBitcode.empty())
        {
            setCompileError(result, CompileErrc::BitcodeReadFailed, bitcodeOutputPath.string());
            return result;
        }

        auto buffer = llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef(result.llvmBitcode.data(), result.llvmBitcode.size()), "in_memory_bc");
        auto parsedBitcode = llvm::parseBitcodeFile(buffer->getMemBufferRef(), context);
        if (!parsedBitcode)
        {
            setCompileError(result, CompileErrc::BitcodeParseFailed,
                            llvm::toString(parsedBitcode.takeError()));
            return result;
        }

        result.module = std::move(*parsedBitcode);
        result.success = true;
        return result;
    }
} // namespace ctrace::concurrency
