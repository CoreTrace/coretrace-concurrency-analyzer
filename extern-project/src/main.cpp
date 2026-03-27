#include "coretrace_concurrency_analyzer.hpp"

#include <llvm/IR/LLVMContext.h>

#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: concurrency_consumer <file.c|cpp> [ll|bc] [extra compiler args...]\n";
        return 1;
    }

    ctrace::concurrency::CompileRequest request;
    request.inputFile = argv[1];

    int extraArgsStart = 2;
    if (argc >= 3)
    {
        const std::string format = argv[2];
        if (format == "ll")
        {
            request.format = ctrace::concurrency::IRFormat::LL;
            extraArgsStart = 3;
        }
        else if (format == "bc")
        {
            request.format = ctrace::concurrency::IRFormat::BC;
            extraArgsStart = 3;
        }
    }

    for (int i = extraArgsStart; i < argc; ++i)
        request.extraCompileArgs.emplace_back(argv[i]);

    llvm::LLVMContext context;
    ctrace::concurrency::InMemoryIRCompiler compiler;
    ctrace::concurrency::CompileResult result = compiler.compile(request, context);

    if (!result.diagnostics.empty())
        std::cerr << result.diagnostics;

    if (!result.success)
    {
        std::cerr << "Compilation failed: " << result.error << "\n";
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
