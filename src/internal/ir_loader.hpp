// SPDX-License-Identifier: Apache-2.0
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

        /// Same, naming the buffer, which becomes the module identifier seen in reports.
        [[nodiscard]] std::unique_ptr<llvm::Module> parseBC(std::string_view llvmBitcode,
                                                            std::string_view bufferName,
                                                            llvm::LLVMContext& context,
                                                            CompileError& error) const;
    };
} // namespace ctrace::concurrency::internal
