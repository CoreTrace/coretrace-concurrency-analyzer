// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <optional>
#include <string>

namespace llvm
{
    class Function;
    class GlobalVariable;
    class Instruction;
    class Value;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    [[nodiscard]] const llvm::GlobalVariable* resolveBaseGlobal(const llvm::Value& value);
    [[nodiscard]] std::optional<std::string> canonicalGlobalId(const llvm::Value& value);
    [[nodiscard]] std::string functionId(const llvm::Function& function);
    [[nodiscard]] std::string functionDisplayName(const llvm::Function& function);
    [[nodiscard]] SourceLocation makeSourceLocation(const llvm::Instruction& instruction);
} // namespace ctrace::concurrency::internal::analysis
