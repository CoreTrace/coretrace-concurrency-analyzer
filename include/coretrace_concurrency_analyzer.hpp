#pragma once

#include <llvm/IR/Module.h>

#include <memory>
#include <string>
#include <vector>

namespace llvm
{
    class LLVMContext;
} // namespace llvm

namespace ctrace::concurrency
{
    enum class IRFormat
    {
        LL,
        BC,
    };

    struct CompileRequest
    {
        std::string inputFile;
        std::vector<std::string> extraCompileArgs;
        IRFormat format = IRFormat::BC;
        bool instrument = false;
    };

    struct CompileResult
    {
        bool success = false;
        std::string diagnostics;
        std::string error;
        std::string llvmIRText;
        std::string llvmBitcode;
        std::unique_ptr<llvm::Module> module;
    };

    class InMemoryIRCompiler
    {
      public:
        CompileResult compile(const CompileRequest& request, llvm::LLVMContext& context) const;
    };

    const char* toString(IRFormat format);
} // namespace ctrace::concurrency
