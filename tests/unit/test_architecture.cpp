// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analyzer.hpp"
#include "coretrace_concurrency_error.hpp"
#include "internal/compilation_backend.hpp"
#include "internal/compile_command_builder.hpp"
#include "internal/ir_loader.hpp"
#include "internal/temporary_bitcode_file.hpp"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

namespace
{
    using ctrace::concurrency::CompileErrc;
    using ctrace::concurrency::CompileError;
    using ctrace::concurrency::CompilePhase;
    using ctrace::concurrency::CompileRequest;
    using ctrace::concurrency::InMemoryIRCompiler;
    using ctrace::concurrency::IRFormat;
    using ctrace::concurrency::make_error_code;
    using ctrace::concurrency::internal::BackendCompileOutput;
    using ctrace::concurrency::internal::CompileCommandBuilder;
    using ctrace::concurrency::internal::ICompilationBackend;
    using ctrace::concurrency::internal::IIRLoader;
    using ctrace::concurrency::internal::LLVMIRLoader;
    using ctrace::concurrency::internal::TemporaryBitcodeFile;

    constexpr std::string_view kValidLL = R"(
; ModuleID = 'fake'
source_filename = "fake.c"
define i32 @main() {
entry:
  ret i32 0
}
)";

    std::size_t countToken(const std::vector<std::string>& args, std::string_view token)
    {
        return static_cast<std::size_t>(std::count_if(
            args.begin(), args.end(), [token](const std::string& arg) { return arg == token; }));
    }

    bool hasToken(const std::vector<std::string>& args, std::string_view token)
    {
        return std::find_if(args.begin(), args.end(),
                            [token](const std::string& arg) { return arg == token; }) != args.end();
    }

    bool hasOutputPair(const std::vector<std::string>& args, const std::string& outputPath)
    {
        for (std::size_t index = 1; index < args.size(); ++index)
        {
            if (args[index - 1] == "-o" && args[index] == outputPath)
                return true;
        }
        return false;
    }

    bool isOutputFlagVariant(const std::string& arg)
    {
        if (arg == "-o")
            return true;
        if (arg.rfind("-o=", 0) == 0)
            return true;
        return arg.size() > 2 && arg.rfind("-o", 0) == 0;
    }

    bool hasOutputFlagVariant(const std::vector<std::string>& args)
    {
        return std::find_if(args.begin(), args.end(),
                            [](const std::string& arg)
                            { return isOutputFlagVariant(arg); }) != args.end();
    }

    std::optional<std::filesystem::path> findOutputPath(const std::vector<std::string>& args)
    {
        for (std::size_t index = 1; index < args.size(); ++index)
        {
            if (args[index - 1] == "-o")
                return std::filesystem::path(args[index]);
        }
        return std::nullopt;
    }

    class ScopedEnvVar final
    {
      public:
        ScopedEnvVar(std::string name, std::string value) : name_(std::move(name))
        {
            const char* oldValue = std::getenv(name_.c_str());
            if (oldValue != nullptr)
                oldValue_ = oldValue;

            setenv(name_.c_str(), value.c_str(), 1);
        }

        ~ScopedEnvVar()
        {
            if (oldValue_.has_value())
                setenv(name_.c_str(), oldValue_->c_str(), 1);
            else
                unsetenv(name_.c_str());
        }

        ScopedEnvVar(const ScopedEnvVar&) = delete;
        ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

      private:
        std::string name_;
        std::optional<std::string> oldValue_;
    };

    class FakeBackend final : public ICompilationBackend
    {
      public:
        using Handler = std::function<BackendCompileOutput(const std::vector<std::string>&, bool)>;

        FakeBackend(Handler llHandler, Handler bcHandler)
            : llHandler_(std::move(llHandler)), bcHandler_(std::move(bcHandler))
        {
        }

        BackendCompileOutput compileLLToMemory(const std::vector<std::string>& args,
                                               bool instrument) const override
        {
            return llHandler_(args, instrument);
        }

        BackendCompileOutput compileBCToFile(const std::vector<std::string>& args,
                                             bool instrument) const override
        {
            return bcHandler_(args, instrument);
        }

      private:
        Handler llHandler_;
        Handler bcHandler_;
    };

    class FakeLoader final : public IIRLoader
    {
      public:
        using LLHandler = std::function<std::unique_ptr<llvm::Module>(
            std::string_view, llvm::LLVMContext&, CompileError&)>;
        using BCHandler = std::function<std::unique_ptr<llvm::Module>(
            std::string_view, llvm::LLVMContext&, CompileError&)>;

        FakeLoader(LLHandler llHandler, BCHandler bcHandler)
            : llHandler_(std::move(llHandler)), bcHandler_(std::move(bcHandler))
        {
        }

        std::unique_ptr<llvm::Module> parseLL(std::string_view llvmIR, llvm::LLVMContext& context,
                                              CompileError& error) const override
        {
            return llHandler_(llvmIR, context, error);
        }

        std::unique_ptr<llvm::Module> parseBC(std::string_view llvmBitcode,
                                              llvm::LLVMContext& context,
                                              CompileError& error) const override
        {
            return bcHandler_(llvmBitcode, context, error);
        }

      private:
        LLHandler llHandler_;
        BCHandler bcHandler_;
    };

    std::filesystem::path fixturePath(std::string_view relativePath)
    {
        return std::filesystem::path(CORETRACE_PROJECT_SOURCE_DIR) / relativePath;
    }

    std::filesystem::path makeTempDir(std::string_view prefix)
    {
        const std::filesystem::path base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 256; ++attempt)
        {
            const std::filesystem::path path =
                base / (std::string(prefix) + "-" + std::to_string(getpid()) + "-" +
                        std::to_string(attempt));
            std::error_code ec;
            if (std::filesystem::create_directory(path, ec))
                return path;
        }
        return {};
    }

    std::unique_ptr<llvm::Module> makeValidModule(llvm::LLVMContext& context)
    {
        CompileError error;
        LLVMIRLoader loader;
        return loader.parseLL(kValidLL, context, error);
    }

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    std::shared_ptr<FakeBackend> makeNoopBackend()
    {
        return std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool) { return BackendCompileOutput{}; },
            [](const std::vector<std::string>&, bool) { return BackendCompileOutput{}; });
    }

    bool testInMemoryCompilerWithValidLLFromFakeBackend()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = true;
                output.diagnostics = "fake ll success\n";
                output.llvmIR = std::string(kValidLL);
                return output;
            },
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                output.diagnostics = "bc path should not be used\n";
                return output;
            });

        auto loader = std::make_shared<LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(result.success, "LL compile should succeed") &&
               assertTrue(result.module != nullptr, "LL compile should return a module") &&
               assertTrue(!result.error.hasError(), "LL compile should not report an error") &&
               assertTrue(result.diagnostics == "fake ll success\n",
                          "LL diagnostics should propagate from backend");
    }

    bool testInMemoryCompilerWithInvalidLLFromFakeBackend()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = true;
                output.llvmIR = "this is not valid llvm ir";
                return output;
            },
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                return output;
            });

        auto loader = std::make_shared<LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "invalid LL should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::IRParseFailed),
                          "invalid LL should map to IRParseFailed") &&
               assertTrue(result.error.phase == CompilePhase::IRParse,
                          "invalid LL should be tagged with IRParse phase");
    }

    bool testInMemoryCompilerWithLLBackendFailure()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                output.diagnostics = "fake ll backend failure\n";
                return output;
            },
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                return output;
            });

        InMemoryIRCompiler compiler(backend, std::make_shared<LLVMIRLoader>());

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "LL backend failure should fail") &&
               assertTrue(result.error.code ==
                              make_error_code(CompileErrc::BackendCompilationFailed),
                          "LL backend failure should map to BackendCompilationFailed") &&
               assertTrue(result.error.phase == CompilePhase::BackendCompile,
                          "LL backend failure should be tagged with BackendCompile phase") &&
               assertTrue(result.diagnostics == "fake ll backend failure\n",
                          "LL backend diagnostics should be preserved");
    }

    bool testInMemoryCompilerWithMissingLLOutput()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = true;
                output.diagnostics = "fake ll success without payload\n";
                output.llvmIR.clear();
                return output;
            },
            [](const std::vector<std::string>&, bool) { return BackendCompileOutput{}; });

        InMemoryIRCompiler compiler(backend, std::make_shared<LLVMIRLoader>());

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "LL compile should fail when IR payload is empty") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::MissingIROutput),
                          "empty LL payload should map to MissingIROutput") &&
               assertTrue(result.error.phase == CompilePhase::BackendCompile,
                          "empty LL payload should be tagged with BackendCompile phase");
    }

    bool testInMemoryCompilerUsesFallbackErrorWhenLLLoaderReturnsNoError()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = true;
                output.llvmIR = std::string(kValidLL);
                return output;
            },
            [](const std::vector<std::string>&, bool) { return BackendCompileOutput{}; });

        auto loader =
            std::make_shared<FakeLoader>([](std::string_view, llvm::LLVMContext&, CompileError&)
                                         { return std::unique_ptr<llvm::Module>{}; },
                                         [](std::string_view, llvm::LLVMContext& context,
                                            CompileError&) { return makeValidModule(context); });

        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "LL loader null module should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::IRParseFailed),
                          "LL fallback error should map to IRParseFailed") &&
               assertTrue(result.error.phase == CompilePhase::IRParse,
                          "LL fallback error should use IRParse phase") &&
               assertTrue(result.error.message == "failed to parse LLVM IR in ll mode",
                          "LL fallback error should use stable message");
    }

    bool testInMemoryCompilerWithBCBackendFailure()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                return output;
            },
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                output.diagnostics = "fake bc backend failure\n";
                return output;
            });

        auto loader = std::make_shared<LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::BC;

        llvm::LLVMContext context;
        ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "BC backend failure should fail") &&
               assertTrue(result.error.code ==
                              make_error_code(CompileErrc::BackendCompilationFailed),
                          "BC backend failure should map to BackendCompilationFailed") &&
               assertTrue(result.error.phase == CompilePhase::BackendCompile,
                          "BC backend failure should be tagged with BackendCompile phase");
    }

    bool testInMemoryCompilerReportsBitcodeReadFailureWhenOutputMissing()
    {
        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool) { return BackendCompileOutput{}; },
            [](const std::vector<std::string>& args, bool)
            {
                const std::optional<std::filesystem::path> outputPath = findOutputPath(args);
                if (outputPath.has_value())
                {
                    std::error_code ec;
                    std::filesystem::remove(*outputPath, ec);
                }

                BackendCompileOutput output;
                output.success = true;
                return output;
            });

        auto loader = std::make_shared<LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::BC;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "missing bitcode file should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::BitcodeReadFailed),
                          "missing bitcode file should map to BitcodeReadFailed") &&
               assertTrue(result.error.phase == CompilePhase::BackendCompile,
                          "missing bitcode file should use BackendCompile phase");
    }

    bool testInMemoryCompilerUsesFallbackErrorWhenBCLoaderReturnsNoError()
    {
        auto backend = std::make_shared<FakeBackend>([](const std::vector<std::string>&, bool)
                                                     { return BackendCompileOutput{}; },
                                                     [](const std::vector<std::string>&, bool)
                                                     {
                                                         BackendCompileOutput output;
                                                         output.success = true;
                                                         return output;
                                                     });

        auto loader =
            std::make_shared<FakeLoader>([](std::string_view, llvm::LLVMContext& context,
                                            CompileError&) { return makeValidModule(context); },
                                         [](std::string_view, llvm::LLVMContext&, CompileError&)
                                         { return std::unique_ptr<llvm::Module>{}; });

        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::BC;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "BC loader null module should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::BitcodeParseFailed),
                          "BC fallback error should map to BitcodeParseFailed") &&
               assertTrue(result.error.phase == CompilePhase::IRParse,
                          "BC fallback error should use IRParse phase") &&
               assertTrue(result.error.message == "failed to parse LLVM bitcode in bc mode",
                          "BC fallback error should use stable message");
    }

    bool testInMemoryCompilerPreservesBCLoaderError()
    {
        auto backend = std::make_shared<FakeBackend>([](const std::vector<std::string>&, bool)
                                                     { return BackendCompileOutput{}; },
                                                     [](const std::vector<std::string>&, bool)
                                                     {
                                                         BackendCompileOutput output;
                                                         output.success = true;
                                                         return output;
                                                     });

        auto loader = std::make_shared<FakeLoader>(
            [](std::string_view, llvm::LLVMContext& context, CompileError&)
            { return makeValidModule(context); },
            [](std::string_view, llvm::LLVMContext&, CompileError& error)
            {
                error.code = make_error_code(CompileErrc::IRParseFailed);
                error.phase = CompilePhase::IRParse;
                error.message = "custom bc parser error";
                return std::unique_ptr<llvm::Module>{};
            });

        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::BC;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "custom BC loader error should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::IRParseFailed),
                          "custom BC loader code should be preserved") &&
               assertTrue(result.error.message == "custom bc parser error",
                          "custom BC loader message should be preserved");
    }

    bool testInMemoryCompilerFailsWhenTemporaryBitcodeCreationFails()
    {
        ScopedEnvVar tmpdirOverride("TMPDIR", "/dev/null");
        InMemoryIRCompiler compiler;

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::BC;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "invalid TMPDIR should fail BC compile") &&
               assertTrue(result.error.code ==
                              make_error_code(CompileErrc::TemporaryBitcodeFileCreationFailed),
                          "TMPDIR failure should map to TemporaryBitcodeFileCreationFailed") &&
               assertTrue(result.error.phase == CompilePhase::BuildCommand,
                          "TMPDIR failure should be reported as BuildCommand phase");
    }

    bool testInMemoryCompilerWithRealBackendAndDefaultCtor()
    {
        InMemoryIRCompiler compiler;
        llvm::LLVMContext context;

        CompileRequest llRequest;
        llRequest.inputFile = fixturePath("tests/fixtures/hello.c").string();
        llRequest.format = IRFormat::LL;

        ctrace::concurrency::CompileResult llResult = compiler.compile(llRequest, context);
        const bool llOk = assertTrue(llResult.success, "default ctor LL compile should succeed") &&
                          assertTrue(llResult.module != nullptr,
                                     "default ctor LL compile should return module") &&
                          assertTrue(!llResult.llvmIRText.empty(),
                                     "default ctor LL compile should return textual IR");

        llvm::LLVMContext bcContext;
        CompileRequest bcRequest;
        bcRequest.inputFile = fixturePath("tests/fixtures/hello.c").string();
        bcRequest.format = IRFormat::BC;

        ctrace::concurrency::CompileResult bcResult = compiler.compile(bcRequest, bcContext);
        const bool bcOk = assertTrue(bcResult.success, "default ctor BC compile should succeed") &&
                          assertTrue(bcResult.module != nullptr,
                                     "default ctor BC compile should return module") &&
                          assertTrue(!bcResult.llvmBitcode.empty(),
                                     "default ctor BC compile should return bitcode payload");

        return llOk && bcOk;
    }

    bool testInMemoryCompilerWithNullDependencyFallbackCtor()
    {
        InMemoryIRCompiler compiler(nullptr, nullptr);

        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(result.success, "nullptr dependency ctor should fallback to defaults") &&
               assertTrue(result.module != nullptr,
                          "nullptr dependency ctor should still produce module");
    }

    bool testInMemoryCompilerRejectsEmptyInputRequest()
    {
        InMemoryIRCompiler compiler(makeNoopBackend(), std::make_shared<LLVMIRLoader>());
        CompileRequest request;
        request.inputFile.clear();

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "empty input request should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::InvalidRequest),
                          "empty input request should map to InvalidRequest") &&
               assertTrue(result.error.phase == CompilePhase::ValidateInput,
                          "empty input request should be validation error");
    }

    bool testInMemoryCompilerRejectsMissingInputFile()
    {
        InMemoryIRCompiler compiler(makeNoopBackend(), std::make_shared<LLVMIRLoader>());
        CompileRequest request;
        request.inputFile = "/tmp/_coretrace_missing_file_xyz.c";
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "missing input file should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::InputFileDoesNotExist),
                          "missing input file should map to InputFileDoesNotExist") &&
               assertTrue(result.error.phase == CompilePhase::ValidateInput,
                          "missing input file should be validation error");
    }

    bool testInMemoryCompilerRejectsDirectoryInput()
    {
        InMemoryIRCompiler compiler(makeNoopBackend(), std::make_shared<LLVMIRLoader>());
        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "directory input should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::InputFileNotRegular),
                          "directory input should map to InputFileNotRegular") &&
               assertTrue(result.error.phase == CompilePhase::ValidateInput,
                          "directory input should be validation error");
    }

    bool testInMemoryCompilerRejectsUnreadableInputFile()
    {
        const std::filesystem::path tempDir = makeTempDir("ctrace-arch-unreadable");
        if (!assertTrue(!tempDir.empty(), "temp dir creation failed"))
            return false;

        const std::filesystem::path unreadableFile = tempDir / "unreadable.c";
        std::error_code ec;
        std::filesystem::copy_file(fixturePath("tests/fixtures/hello.c"), unreadableFile, ec);
        if (!assertTrue(!ec, "failed to create unreadable fixture file"))
            return false;

        std::filesystem::permissions(unreadableFile, std::filesystem::perms::none,
                                     std::filesystem::perm_options::replace, ec);
        if (!assertTrue(!ec, "failed to chmod unreadable fixture"))
            return false;

        InMemoryIRCompiler compiler(makeNoopBackend(), std::make_shared<LLVMIRLoader>());
        CompileRequest request;
        request.inputFile = unreadableFile.string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        std::filesystem::permissions(unreadableFile,
                                     std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::remove_all(tempDir, ec);

        return assertTrue(!result.success, "unreadable input should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::InputFileNotReadable),
                          "unreadable input should map to InputFileNotReadable") &&
               assertTrue(result.error.phase == CompilePhase::ValidateInput,
                          "unreadable input should be validation error");
    }

    bool testInMemoryCompilerReportsAccessFailureForLockedDirectory()
    {
        const std::filesystem::path tempDir = makeTempDir("ctrace-arch-locked");
        if (!assertTrue(!tempDir.empty(), "temp dir creation failed"))
            return false;

        const std::filesystem::path lockedDir = tempDir / "locked";
        const std::filesystem::path lockedFile = lockedDir / "source.c";
        std::error_code ec;
        std::filesystem::create_directory(lockedDir, ec);
        if (!assertTrue(!ec, "failed to create locked directory"))
            return false;

        {
            std::ofstream out(lockedFile);
            out << "int main(){return 0;}\n";
        }

        std::filesystem::permissions(
            lockedDir, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
            std::filesystem::perm_options::replace, ec);
        if (!assertTrue(!ec, "failed to remove execute bit on locked directory"))
            return false;

        InMemoryIRCompiler compiler(makeNoopBackend(), std::make_shared<LLVMIRLoader>());
        CompileRequest request;
        request.inputFile = lockedFile.string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        const ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        std::filesystem::permissions(lockedDir, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
        std::filesystem::remove_all(tempDir, ec);

        return assertTrue(!result.success, "locked path should fail") &&
               assertTrue(result.error.code == make_error_code(CompileErrc::InputFileAccessFailed),
                          "locked path should map to InputFileAccessFailed") &&
               assertTrue(result.error.phase == CompilePhase::ValidateInput,
                          "locked path should be validation error");
    }

    bool testIRLoaderErrorMapping()
    {
        LLVMIRLoader loader;
        llvm::LLVMContext context;

        CompileError llError;
        std::unique_ptr<llvm::Module> llModule =
            loader.parseLL("definitely invalid llvm ir", context, llError);

        const bool llOk = assertTrue(!llModule, "invalid LL parse should fail") &&
                          assertTrue(llError.code == make_error_code(CompileErrc::IRParseFailed),
                                     "invalid LL should map to IRParseFailed") &&
                          assertTrue(llError.phase == CompilePhase::IRParse,
                                     "invalid LL should use IRParse phase");

        CompileError bcError;
        const std::string invalidBC = "not-a-bitcode-stream";
        std::unique_ptr<llvm::Module> bcModule = loader.parseBC(invalidBC, context, bcError);

        const bool bcOk =
            assertTrue(!bcModule, "invalid BC parse should fail") &&
            assertTrue(bcError.code == make_error_code(CompileErrc::BitcodeParseFailed),
                       "invalid BC should map to BitcodeParseFailed") &&
            assertTrue(bcError.phase == CompilePhase::IRParse,
                       "invalid BC should use IRParse phase");

        return llOk && bcOk;
    }

    bool testCompileCommandBuilderAvoidsDuplicateInputFile()
    {
        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.extraCompileArgs = {"-Wall", request.inputFile, "-S", "-o", "stale_output.bc"};

        const std::vector<std::string> llArgs = CompileCommandBuilder::buildLL(request);
        const bool llOk =
            assertTrue(countToken(llArgs, request.inputFile) == 1,
                       "LL args should include input file only once") &&
            assertTrue(hasToken(llArgs, "-emit-llvm"), "LL args should include -emit-llvm") &&
            assertTrue(hasToken(llArgs, "-S"), "LL args should include -S") &&
            assertTrue(hasToken(llArgs, "-g"), "LL args should include -g");

        const std::filesystem::path outputPath = fixturePath("build/test-output.bc");
        const std::vector<std::string> bcArgs = CompileCommandBuilder::buildBC(request, outputPath);
        const bool bcOk =
            assertTrue(countToken(bcArgs, request.inputFile) == 1,
                       "BC args should include input file only once") &&
            assertTrue(hasToken(bcArgs, "-emit-llvm"), "BC args should include -emit-llvm") &&
            assertTrue(hasToken(bcArgs, "-c"), "BC args should include -c") &&
            assertTrue(!hasToken(bcArgs, "-S"), "BC args should not include -S") &&
            assertTrue(hasOutputPair(bcArgs, outputPath.string()),
                       "BC args should include -o <outputPath>");

        return llOk && bcOk;
    }

    bool testCompileCommandBuilderStripsAttachedOutputFlags()
    {
        CompileRequest request;
        request.inputFile = fixturePath("tests/fixtures/hello.c").string();
        request.extraCompileArgs = {"-o=old.bc", "-Wall",   "-oold2.bc", "-Winvalid",
                                    "-o",        "old3.bc", "-DVALUE=1", "-o"};

        const std::vector<std::string> llArgs = CompileCommandBuilder::buildLL(request);
        const bool llOk =
            assertTrue(!hasOutputFlagVariant(llArgs),
                       "LL args should strip all -o* output flags") &&
            assertTrue(hasToken(llArgs, "-Wall"), "LL args should preserve non-output flags") &&
            assertTrue(hasToken(llArgs, "-Winvalid"), "LL args should preserve -Winvalid") &&
            assertTrue(hasToken(llArgs, "-DVALUE=1"), "LL args should preserve defines");

        const std::filesystem::path outputPath = fixturePath("build/output-attached.bc");
        const std::vector<std::string> bcArgs = CompileCommandBuilder::buildBC(request, outputPath);
        const bool hasAttachedOutputFlag = std::any_of(
            bcArgs.begin(), bcArgs.end(),
            [](const std::string& arg)
            { return arg.rfind("-o=", 0) == 0 || (arg.size() > 2 && arg.rfind("-o", 0) == 0); });
        const bool bcOk =
            assertTrue(countToken(bcArgs, "-o") == 1, "BC args should contain exactly one -o") &&
            assertTrue(hasOutputPair(bcArgs, outputPath.string()),
                       "BC args should target requested output path") &&
            assertTrue(
                !hasAttachedOutputFlag,
                "BC args should strip stale attached -o variants before appending final pair");

        return llOk && bcOk;
    }

    bool testTemporaryBitcodeFileMoveAssignmentAndSelfAssignment()
    {
        CompileError firstError;
        CompileError secondError;
        std::filesystem::path movedPath;
        std::filesystem::path oldSecondPath;

        {
            std::optional<TemporaryBitcodeFile> first = TemporaryBitcodeFile::create(firstError);
            std::optional<TemporaryBitcodeFile> second = TemporaryBitcodeFile::create(secondError);

            const bool created =
                assertTrue(first.has_value() && second.has_value(),
                           "temporary bitcode files should be created for move test") &&
                assertTrue(std::filesystem::exists(first->path()),
                           "first temp bitcode file should exist") &&
                assertTrue(std::filesystem::exists(second->path()),
                           "second temp bitcode file should exist");
            if (!created)
                return false;

            const std::filesystem::path firstPath = first->path();
            oldSecondPath = second->path();

            *second = std::move(*first);
            const bool moveAssignOk =
                assertTrue(second->path() == firstPath,
                           "move assignment should transfer path from source to destination") &&
                assertTrue(first->path().empty(),
                           "moved-from temporary file path should be empty") &&
                assertTrue(!std::filesystem::exists(oldSecondPath),
                           "move assignment should cleanup destination's old file");
            if (!moveAssignOk)
                return false;

            movedPath = second->path();
            *second = std::move(*second);
            const bool selfAssignOk = assertTrue(second->path() == movedPath,
                                                 "self move-assignment should preserve path");
            if (!selfAssignOk)
                return false;
        }

        return assertTrue(!std::filesystem::exists(movedPath),
                          "temporary bitcode file destructor should cleanup moved path");
    }
} // namespace

int main()
{
    bool ok = true;

    ok = testInMemoryCompilerWithValidLLFromFakeBackend() && ok;
    ok = testInMemoryCompilerWithInvalidLLFromFakeBackend() && ok;
    ok = testInMemoryCompilerWithLLBackendFailure() && ok;
    ok = testInMemoryCompilerWithMissingLLOutput() && ok;
    ok = testInMemoryCompilerUsesFallbackErrorWhenLLLoaderReturnsNoError() && ok;
    ok = testInMemoryCompilerWithBCBackendFailure() && ok;
    ok = testInMemoryCompilerReportsBitcodeReadFailureWhenOutputMissing() && ok;
    ok = testInMemoryCompilerUsesFallbackErrorWhenBCLoaderReturnsNoError() && ok;
    ok = testInMemoryCompilerPreservesBCLoaderError() && ok;
    ok = testInMemoryCompilerFailsWhenTemporaryBitcodeCreationFails() && ok;
    ok = testInMemoryCompilerWithRealBackendAndDefaultCtor() && ok;
    ok = testInMemoryCompilerWithNullDependencyFallbackCtor() && ok;
    ok = testInMemoryCompilerRejectsEmptyInputRequest() && ok;
    ok = testInMemoryCompilerRejectsMissingInputFile() && ok;
    ok = testInMemoryCompilerRejectsDirectoryInput() && ok;
    ok = testInMemoryCompilerRejectsUnreadableInputFile() && ok;
    ok = testInMemoryCompilerReportsAccessFailureForLockedDirectory() && ok;
    ok = testIRLoaderErrorMapping() && ok;
    ok = testCompileCommandBuilderAvoidsDuplicateInputFile() && ok;
    ok = testCompileCommandBuilderStripsAttachedOutputFlags() && ok;
    ok = testTemporaryBitcodeFileMoveAssignmentAndSelfAssignment() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] architecture tests\n";
    return 0;
}
