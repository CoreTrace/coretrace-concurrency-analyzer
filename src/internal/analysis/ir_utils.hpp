// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <optional>
#include <string>
#include <vector>

namespace llvm
{
    class GlobalVariable;
    class Function;
    class Argument;
    class Instruction;
    class Value;
    class AAResults;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    struct ResolvedSourceLocations
    {
        SourceLocation loweredLocation;
        SourceLocation userLocation;
    };

    [[nodiscard]] bool shouldTrackSharedGlobal(const llvm::GlobalVariable& global);

    struct FunctionBinding
    {
        const llvm::Function* function = nullptr;
        std::optional<unsigned> argumentIndex;
    };

    struct AliasResolvedGlobal
    {
        std::string symbol;
        AliasProvenance aliasProvenance = AliasProvenance::Direct;
    };

    [[nodiscard]] const llvm::GlobalVariable* resolveBaseGlobal(const llvm::Value& value);
    [[nodiscard]] std::optional<std::string> canonicalGlobalId(const llvm::Value& value);
    [[nodiscard]] std::optional<std::string> canonicalStorageGroupId(const llvm::Value& value);
    [[nodiscard]] std::optional<RootBinding> resolveTrackedRoot(const llvm::Value& value);
    [[nodiscard]] std::optional<AliasResolvedGlobal>
    resolveAliasGlobal(const llvm::Instruction& accessInstruction, llvm::AAResults& aaResults,
                       const std::vector<const llvm::GlobalVariable*>& candidateGlobals);
    [[nodiscard]] std::optional<FunctionBinding> resolveFunctionBinding(const llvm::Value& value);
    [[nodiscard]] const llvm::Function* resolveFunctionValue(const llvm::Value& value);
    [[nodiscard]] std::string functionId(const llvm::Function& function);
    [[nodiscard]] std::string functionDisplayName(const llvm::Function& function);
    [[nodiscard]] ResolvedSourceLocations
    resolveSourceLocations(const llvm::Instruction& instruction);
    [[nodiscard]] SourceLocation makeSourceLocation(const llvm::Instruction& instruction);
} // namespace ctrace::concurrency::internal::analysis
