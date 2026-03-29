// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>
#include <vector>

namespace ctrace::concurrency::internal
{
    struct BackendCompileOutput
    {
        bool success = false;
        std::string diagnostics;
        std::string llvmIR;
    };

    class ICompilationBackend
    {
      public:
        virtual ~ICompilationBackend() = default;

        [[nodiscard]] virtual BackendCompileOutput
        compileLLToMemory(const std::vector<std::string>& args, bool instrument) const = 0;

        [[nodiscard]] virtual BackendCompileOutput
        compileBCToFile(const std::vector<std::string>& args, bool instrument) const = 0;
    };

    class CompilerLibBackend final : public ICompilationBackend
    {
      public:
        [[nodiscard]] BackendCompileOutput compileLLToMemory(const std::vector<std::string>& args,
                                                             bool instrument) const override;

        [[nodiscard]] BackendCompileOutput compileBCToFile(const std::vector<std::string>& args,
                                                           bool instrument) const override;
    };
} // namespace ctrace::concurrency::internal
