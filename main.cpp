#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace
{
    void printHelp()
    {
        llvm::outs()
            << "CoreTrace Concurrency Analyzer bootstrap CLI\n\n"
            << "Usage:\n"
            << "  coretrace_concurrency_analyzer <file.c|cpp> [options]\n\n"
            << "Options:\n"
            << "  --ir-format=ll|bc        Compilation output mode (default: bc)\n"
            << "  --compile-arg=<arg>      Forward a compile argument (repeatable)\n"
            << "  --instrument             Enable compilerlib instrumentation mode\n"
            << "  --verbose                Print request details for debugging\n"
            << "  --                       Forward all following args to compilerlib\n"
            << "  -h, --help               Show this help message\n\n"
            << "Examples:\n"
            << "  coretrace_concurrency_analyzer test.c --ir-format=ll\n"
            << "  coretrace_concurrency_analyzer test.c --ir-format=bc --compile-arg=-Iinclude\n"
            << "  coretrace_concurrency_analyzer test.c -- --std=gnu11 -Wall\n";
    }

    void printRequestSummary(const ctrace::concurrency::CompileRequest& request)
    {
        llvm::outs() << "request.input-file: " << request.inputFile << "\n";
        llvm::outs() << "request.ir-format: " << ctrace::concurrency::toString(request.format)
                     << "\n";
        llvm::outs() << "request.instrument: " << (request.instrument ? "true" : "false") << "\n";
        llvm::outs() << "request.extra-args-count: " << request.extraCompileArgs.size() << "\n";
        for (const std::string& arg : request.extraCompileArgs)
            llvm::outs() << "request.extra-arg: " << arg << "\n";
    }

    std::size_t countDefinedFunctions(const llvm::Module& module)
    {
        std::size_t count = 0;
        for (const llvm::Function& function : module)
        {
            if (!function.isDeclaration())
                ++count;
        }
        return count;
    }

    bool parseFormat(std::string_view value, ctrace::concurrency::IRFormat& out)
    {
        if (value == "ll")
        {
            out = ctrace::concurrency::IRFormat::LL;
            return true;
        }
        if (value == "bc")
        {
            out = ctrace::concurrency::IRFormat::BC;
            return true;
        }
        return false;
    }
} // namespace

int main(int argc, char** argv)
{
    ctrace::concurrency::CompileRequest request;
    bool passthroughMode = false;
    bool verbose = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];

        if (passthroughMode)
        {
            request.extraCompileArgs.emplace_back(arg);
            continue;
        }

        if (arg == "--")
        {
            passthroughMode = true;
            continue;
        }

        if (arg == "-h" || arg == "--help")
        {
            printHelp();
            return 0;
        }

        if (arg == "--verbose")
        {
            verbose = true;
            continue;
        }

        constexpr std::string_view formatPrefix = "--ir-format=";
        if (arg.rfind(formatPrefix, 0) == 0)
        {
            if (!parseFormat(arg.substr(formatPrefix.size()), request.format))
            {
                llvm::errs() << "Unsupported --ir-format value: " << std::string(arg) << "\n";
                return 1;
            }
            continue;
        }

        constexpr std::string_view compileArgPrefix = "--compile-arg=";
        if (arg.rfind(compileArgPrefix, 0) == 0)
        {
            request.extraCompileArgs.emplace_back(arg.substr(compileArgPrefix.size()));
            continue;
        }

        if (arg == "--instrument")
        {
            request.instrument = true;
            continue;
        }

        if (!arg.empty() && arg.front() == '-')
        {
            llvm::errs() << "Unknown option: " << std::string(arg) << "\n";
            return 1;
        }

        if (request.inputFile.empty())
        {
            request.inputFile = std::string(arg);
            continue;
        }

        llvm::errs() << "Unexpected positional argument: " << std::string(arg) << "\n";
        return 1;
    }

    if (request.inputFile.empty())
    {
        printHelp();
        return 1;
    }

    if (verbose)
        printRequestSummary(request);

    llvm::LLVMContext context;
    ctrace::concurrency::InMemoryIRCompiler compiler;
    ctrace::concurrency::CompileResult result = compiler.compile(request, context);

    if (!result.diagnostics.empty())
        llvm::errs() << result.diagnostics;

    if (!result.success)
    {
        const std::string renderedError = formatCompileError(result.error);
        if (!renderedError.empty())
            llvm::errs() << renderedError << "\n";
        else
            llvm::errs() << "unknown_compile_failure\n";
        return 1;
    }

    const std::size_t payloadBytes = (request.format == ctrace::concurrency::IRFormat::BC)
                                         ? result.llvmBitcode.size()
                                         : result.llvmIRText.size();

    llvm::outs() << "IR compilation succeeded\n";
    llvm::outs() << "format: " << ctrace::concurrency::toString(request.format) << "\n";
    llvm::outs() << "payload-bytes: " << payloadBytes << "\n";
    llvm::outs() << "defined-functions: " << countDefinedFunctions(*result.module) << "\n";

    return 0;
}
