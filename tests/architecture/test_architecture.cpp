#include "coretrace_concurrency_analyzer.hpp"
#include "coretrace_concurrency_error.hpp"
#include "internal/compile_command_builder.hpp"
#include "internal/compilation_backend.hpp"
#include "internal/ir_loader.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace
{
    using ctrace::concurrency::CompileErrc;
    using ctrace::concurrency::CompileError;
    using ctrace::concurrency::CompilePhase;
    using ctrace::concurrency::CompileRequest;
    using ctrace::concurrency::InMemoryIRCompiler;
    using ctrace::concurrency::IRFormat;
    using ctrace::concurrency::internal::BackendCompileOutput;
    using ctrace::concurrency::internal::CompileCommandBuilder;

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

    class FakeBackend final : public ctrace::concurrency::internal::ICompilationBackend
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

    std::filesystem::path fixturePath(std::string_view relativePath)
    {
        return std::filesystem::path(CORETRACE_PROJECT_SOURCE_DIR) / relativePath;
    }

    bool assertTrue(bool condition, const std::string& message)
    {
        if (condition)
            return true;

        std::cerr << "[FAIL] " << message << "\n";
        return false;
    }

    bool testInMemoryCompilerWithValidLLFromFakeBackend()
    {
        static constexpr const char* kValidLL = R"(
; ModuleID = 'fake'
source_filename = "fake.c"
define i32 @main() {
entry:
  ret i32 0
}
)";

        auto backend = std::make_shared<FakeBackend>(
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = true;
                output.diagnostics = "fake ll success\n";
                output.llvmIR = kValidLL;
                return output;
            },
            [](const std::vector<std::string>&, bool)
            {
                BackendCompileOutput output;
                output.success = false;
                output.diagnostics = "bc path should not be used\n";
                return output;
            });

        auto loader = std::make_shared<ctrace::concurrency::internal::LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("test/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(result.success, "LL compile should succeed") &&
               assertTrue(result.module != nullptr, "LL compile should return a module") &&
               assertTrue(!result.error.hasError(), "LL compile should not report an error");
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

        auto loader = std::make_shared<ctrace::concurrency::internal::LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("test/fixtures/hello.c").string();
        request.format = IRFormat::LL;

        llvm::LLVMContext context;
        ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "invalid LL should fail") &&
               assertTrue(result.error.code ==
                              ctrace::concurrency::make_error_code(CompileErrc::IRParseFailed),
                          "invalid LL should map to IRParseFailed") &&
               assertTrue(result.error.phase == CompilePhase::IRParse,
                          "invalid LL should be tagged with IRParse phase");
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

        auto loader = std::make_shared<ctrace::concurrency::internal::LLVMIRLoader>();
        InMemoryIRCompiler compiler(backend, loader);

        CompileRequest request;
        request.inputFile = fixturePath("test/fixtures/hello.c").string();
        request.format = IRFormat::BC;

        llvm::LLVMContext context;
        ctrace::concurrency::CompileResult result = compiler.compile(request, context);

        return assertTrue(!result.success, "BC backend failure should fail") &&
               assertTrue(result.error.code == ctrace::concurrency::make_error_code(
                                                   CompileErrc::BackendCompilationFailed),
                          "BC backend failure should map to BackendCompilationFailed") &&
               assertTrue(result.error.phase == CompilePhase::BackendCompile,
                          "BC backend failure should be tagged with BackendCompile phase");
    }

    bool testIRLoaderErrorMapping()
    {
        ctrace::concurrency::internal::LLVMIRLoader loader;
        llvm::LLVMContext context;

        CompileError llError;
        std::unique_ptr<llvm::Module> llModule =
            loader.parseLL("definitely invalid llvm ir", context, llError);

        const bool llOk = assertTrue(!llModule, "invalid LL parse should fail") &&
                          assertTrue(llError.code == ctrace::concurrency::make_error_code(
                                                         CompileErrc::IRParseFailed),
                                     "invalid LL should map to IRParseFailed") &&
                          assertTrue(llError.phase == CompilePhase::IRParse,
                                     "invalid LL should use IRParse phase");

        CompileError bcError;
        const std::string invalidBC = "not-a-bitcode-stream";
        std::unique_ptr<llvm::Module> bcModule = loader.parseBC(invalidBC, context, bcError);

        const bool bcOk = assertTrue(!bcModule, "invalid BC parse should fail") &&
                          assertTrue(bcError.code == ctrace::concurrency::make_error_code(
                                                         CompileErrc::BitcodeParseFailed),
                                     "invalid BC should map to BitcodeParseFailed") &&
                          assertTrue(bcError.phase == CompilePhase::IRParse,
                                     "invalid BC should use IRParse phase");

        return llOk && bcOk;
    }

    bool testCompileCommandBuilderAvoidsDuplicateInputFile()
    {
        CompileRequest request;
        request.inputFile = fixturePath("test/fixtures/hello.c").string();
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
} // namespace

int main()
{
    bool ok = true;

    ok = testInMemoryCompilerWithValidLLFromFakeBackend() && ok;
    ok = testInMemoryCompilerWithInvalidLLFromFakeBackend() && ok;
    ok = testInMemoryCompilerWithBCBackendFailure() && ok;
    ok = testIRLoaderErrorMapping() && ok;
    ok = testCompileCommandBuilderAvoidsDuplicateInputFile() && ok;

    if (!ok)
        return 1;

    std::cout << "[PASS] architecture tests\n";
    return 0;
}
