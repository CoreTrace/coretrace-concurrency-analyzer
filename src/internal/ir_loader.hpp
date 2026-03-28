#pragma once

#include "coretrace_concurrency_error.hpp"

#include <memory>
#include <string_view>

namespace llvm
{
    class LLVMContext;
    class Module;
} // namespace llvm

namespace ctrace::concurrency::internal
{
    class IIRLoader
    {
      public:
        virtual ~IIRLoader() = default;

        [[nodiscard]] virtual std::unique_ptr<llvm::Module>
        parseLL(std::string_view llvmIR, llvm::LLVMContext& context, CompileError& error) const = 0;

        [[nodiscard]] virtual std::unique_ptr<llvm::Module> parseBC(std::string_view llvmBitcode,
                                                                    llvm::LLVMContext& context,
                                                                    CompileError& error) const = 0;
    };

    class LLVMIRLoader final : public IIRLoader
    {
      public:
        [[nodiscard]] std::unique_ptr<llvm::Module> parseLL(std::string_view llvmIR,
                                                            llvm::LLVMContext& context,
                                                            CompileError& error) const override;

        [[nodiscard]] std::unique_ptr<llvm::Module> parseBC(std::string_view llvmBitcode,
                                                            llvm::LLVMContext& context,
                                                            CompileError& error) const override;
    };
} // namespace ctrace::concurrency::internal
