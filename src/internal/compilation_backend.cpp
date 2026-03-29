// SPDX-License-Identifier: Apache-2.0
#include "compilation_backend.hpp"

#include <compilerlib/compiler.h>

#include <utility>

namespace ctrace::concurrency::internal
{
    namespace
    {
        BackendCompileOutput toBackendOutput(compilerlib::CompileResult raw)
        {
            BackendCompileOutput output;
            output.success = raw.success;
            output.diagnostics = std::move(raw.diagnostics);
            output.llvmIR = std::move(raw.llvmIR);
            return output;
        }
    } // namespace

    BackendCompileOutput CompilerLibBackend::compileLLToMemory(const std::vector<std::string>& args,
                                                               bool instrument) const
    {
        compilerlib::CompileResult raw =
            compilerlib::compile(args, compilerlib::OutputMode::ToMemory, instrument);
        return toBackendOutput(std::move(raw));
    }

    BackendCompileOutput CompilerLibBackend::compileBCToFile(const std::vector<std::string>& args,
                                                             bool instrument) const
    {
        compilerlib::CompileResult raw =
            compilerlib::compile(args, compilerlib::OutputMode::ToFile, instrument);
        return toBackendOutput(std::move(raw));
    }
} // namespace ctrace::concurrency::internal
