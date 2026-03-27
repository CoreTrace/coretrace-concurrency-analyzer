#include "coretrace_concurrency_analyzer.hpp"

#include <compilerlib/compiler.h>

#include <llvm/Bitcode/BitcodeReader.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
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

        std::filesystem::path makeTempBitcodePath()
        {
            std::error_code ec;
            std::filesystem::path tempDir = std::filesystem::temp_directory_path(ec);
            if (ec)
                tempDir = std::filesystem::path(".");

            const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
            return tempDir / ("coretrace-concurrency-ir-" + std::to_string(nonce) + ".bc");
        }

        bool readBinaryFile(const std::filesystem::path& path, std::string& out)
        {
            out.clear();
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in)
                return false;

            in.seekg(0, std::ios::end);
            const std::streampos endPos = in.tellg();
            if (endPos < 0)
                return false;
            out.resize(static_cast<std::size_t>(endPos));
            in.seekg(0, std::ios::beg);
            if (!out.empty())
                in.read(out.data(), static_cast<std::streamsize>(out.size()));
            return in.good() || in.eof();
        }
    } // namespace

    const char* toString(IRFormat format)
    {
        switch (format)
        {
        case IRFormat::LL:
            return "ll";
        case IRFormat::BC:
            return "bc";
        }
        return "bc";
    }

    CompileResult InMemoryIRCompiler::compile(const CompileRequest& request,
                                              llvm::LLVMContext& context) const
    {
        CompileResult result;

        if (request.inputFile.empty())
        {
            result.error = "Input file is required";
            return result;
        }

        if (request.format == IRFormat::LL)
        {
            const std::vector<std::string> args = buildLLCompileArgs(request);
            compilerlib::CompileResult raw =
                compilerlib::compile(args, compilerlib::OutputMode::ToMemory, request.instrument);
            result.diagnostics = raw.diagnostics;

            if (!raw.success)
            {
                result.error = "Compilation failed";
                return result;
            }

            if (raw.llvmIR.empty())
            {
                result.error = "No LLVM IR produced by compilerlib::compile";
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
                result.error = "Failed to parse in-memory LLVM IR:\n" + os.str();
                return result;
            }

            result.success = true;
            return result;
        }

        const std::filesystem::path bitcodeOutputPath = makeTempBitcodePath();
        const ScopedFileCleanup bitcodeCleanup(bitcodeOutputPath);

        const std::vector<std::string> args = buildBCCompileArgs(request, bitcodeOutputPath);
        compilerlib::CompileResult raw =
            compilerlib::compile(args, compilerlib::OutputMode::ToFile, request.instrument);
        result.diagnostics = raw.diagnostics;

        if (!raw.success)
        {
            result.error = "Compilation failed";
            return result;
        }

        if (!readBinaryFile(bitcodeOutputPath, result.llvmBitcode) || result.llvmBitcode.empty())
        {
            result.error = "Bitcode output file is missing or empty";
            return result;
        }

        auto buffer = llvm::MemoryBuffer::getMemBufferCopy(
            llvm::StringRef(result.llvmBitcode.data(), result.llvmBitcode.size()), "in_memory_bc");
        auto parsedBitcode = llvm::parseBitcodeFile(buffer->getMemBufferRef(), context);
        if (!parsedBitcode)
        {
            result.error = "Failed to parse in-memory LLVM bitcode: " +
                           llvm::toString(parsedBitcode.takeError());
            return result;
        }

        result.module = std::move(*parsedBitcode);
        result.success = true;
        return result;
    }
} // namespace ctrace::concurrency
