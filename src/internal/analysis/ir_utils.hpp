// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llvm
{
    class GlobalVariable;
    class Function;
    class Argument;
    class DataLayout;
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

    /// True when `pointerOperand` designates a synchronization primitive (a POSIX mutex/rwlock/
    /// condition variable, or their C++ standard library counterparts) rather than user data.
    /// Such objects are mutated by the runtime under its own synchronization, so treating them as
    /// shared data reports the synchronization itself as a race.
    [[nodiscard]] bool designatesSynchronizationPrimitive(const llvm::Value& pointerOperand);

    /// True when `pointerOperand` designates a lock type that may legally be reacquired by the
    /// thread already holding it (`std::recursive_mutex`, `std::recursive_timed_mutex`).
    [[nodiscard]] bool designatesRecursiveLockType(const llvm::Value& pointerOperand);

    [[nodiscard]] const llvm::GlobalVariable* resolveBaseGlobal(const llvm::Value& value);
    [[nodiscard]] std::optional<std::string> canonicalGlobalId(const llvm::Value& value);
    /// Canonical identity of a lock object, field-sensitive so that two mutexes stored in the same
    /// global aggregate are not conflated. Without a data layout the offset cannot be folded and
    /// every member of an aggregate shares the base identity, as before.
    [[nodiscard]] std::optional<std::string> canonicalLockId(const llvm::Value& value,
                                                             const llvm::DataLayout* layout);
    [[nodiscard]] std::optional<std::string> canonicalStorageGroupId(const llvm::Value& value);
    /// Resolves the tracked root of a pointer, together with the byte range it designates.
    /// `byteSize` is the extent of the access; zero means unknown and conservatively covers the
    /// whole object.
    [[nodiscard]] std::optional<RootBinding> resolveTrackedRoot(const llvm::Value& value,
                                                                const llvm::DataLayout* layout,
                                                                std::uint64_t byteSize);
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
