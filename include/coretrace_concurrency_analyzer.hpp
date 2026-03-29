// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_error.hpp"

#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llvm
{
    class LLVMContext;
} // namespace llvm

namespace ctrace::concurrency
{
    namespace internal
    {
        class ICompilationBackend;
        class IIRLoader;
    } // namespace internal

    enum class IRFormat
    {
        LL,
        BC,
    };

    struct CompileRequest
    {
        std::string inputFile;
        // Forwarded directly to compilerlib (no shell interpolation).
        // Treat these arguments as trusted input.
        std::vector<std::string> extraCompileArgs;
        IRFormat format = IRFormat::BC;
        bool instrument = false;
    };

    struct CompileResult
    {
        bool success = false;
        std::string diagnostics;
        CompileError error;
        std::string llvmIRText;
        std::string llvmBitcode;
        std::unique_ptr<llvm::Module> module;
    };

    class InMemoryIRCompiler
    {
      public:
        InMemoryIRCompiler();
        InMemoryIRCompiler(std::shared_ptr<internal::ICompilationBackend> backend,
                           std::shared_ptr<internal::IIRLoader> irLoader);

        [[nodiscard]] CompileResult compile(const CompileRequest& request,
                                            llvm::LLVMContext& context) const;

      private:
        std::shared_ptr<internal::ICompilationBackend> backend_;
        std::shared_ptr<internal::IIRLoader> irLoader_;
    };

    constexpr std::string_view toString(IRFormat format)
    {
        switch (format)
        {
        case IRFormat::LL:
            return "ll";
        case IRFormat::BC:
            return "bc";
        }
        llvm_unreachable("Unhandled IRFormat");
    }
} // namespace ctrace::concurrency
