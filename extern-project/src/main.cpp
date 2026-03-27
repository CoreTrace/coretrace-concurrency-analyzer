#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/LLVMContext.h>

#include <iostream>
#include <string>
#include <string_view>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: concurrency_consumer <file.c|cpp> [options]\n";
        std::cerr << "options:\n";
        std::cerr << "  --ir-format=ll|bc       IR output format (default: bc)\n";
        std::cerr << "  --compile-arg=<arg>     Forward a compile argument (repeatable)\n";
        std::cerr << "  --instrument            Enable compilerlib instrumentation mode\n";
        std::cerr << "  --verbose               Print request details for debugging\n";
        std::cerr << "  --                      Forward all following args to compilerlib\n";
        std::cerr << "legacy compatibility: second positional arg ll|bc is still accepted\n";
        return 1;
    }

    ctrace::concurrency::CompileRequest request;
    request.inputFile = argv[1];

    bool passthroughMode = false;
    bool formatExplicitlySet = false;
    bool verbose = false;

    for (int i = 2; i < argc; ++i)
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

        constexpr std::string_view formatPrefix = "--ir-format=";
        if (arg.rfind(formatPrefix, 0) == 0)
        {
            const std::string_view value = arg.substr(formatPrefix.size());
            if (value == "ll")
            {
                request.format = ctrace::concurrency::IRFormat::LL;
                formatExplicitlySet = true;
                continue;
            }
            if (value == "bc")
            {
                request.format = ctrace::concurrency::IRFormat::BC;
                formatExplicitlySet = true;
                continue;
            }
            std::cerr << "Unsupported --ir-format value: " << std::string(arg) << "\n";
            return 1;
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

        if (arg == "--verbose")
        {
            verbose = true;
            continue;
        }

        // Legacy compatibility mode: keep supporting positional ll|bc.
        if (!formatExplicitlySet && (arg == "ll" || arg == "bc"))
        {
            request.format = (arg == "ll") ? ctrace::concurrency::IRFormat::LL
                                           : ctrace::concurrency::IRFormat::BC;
            formatExplicitlySet = true;
            continue;
        }

        request.extraCompileArgs.emplace_back(arg);
    }

    if (verbose)
    {
        std::cerr << "request.input-file: " << request.inputFile << "\n";
        std::cerr << "request.ir-format: " << ctrace::concurrency::toString(request.format) << "\n";
        std::cerr << "request.instrument: " << (request.instrument ? "true" : "false") << "\n";
        std::cerr << "request.extra-args-count: " << request.extraCompileArgs.size() << "\n";
        for (const std::string& arg : request.extraCompileArgs)
            std::cerr << "request.extra-arg: " << arg << "\n";
    }

    llvm::LLVMContext context;
    ctrace::concurrency::InMemoryIRCompiler compiler;
    ctrace::concurrency::CompileResult result = compiler.compile(request, context);

    if (!result.diagnostics.empty())
        std::cerr << result.diagnostics;

    if (!result.success)
    {
        const std::string renderedError = formatCompileError(result.error);
        std::cerr << "Compilation failed: "
                  << (renderedError.empty() ? "unknown_compile_failure" : renderedError) << "\n";
        return 1;
    }

    const std::size_t payloadBytes = (request.format == ctrace::concurrency::IRFormat::BC)
                                         ? result.llvmBitcode.size()
                                         : result.llvmIRText.size();

    std::cout << "Consumer compilation succeeded in "
              << ctrace::concurrency::toString(request.format) << " mode, payload=" << payloadBytes
              << " bytes\n";

    return 0;
}
