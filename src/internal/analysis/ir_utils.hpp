// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace llvm
{
    class GlobalVariable;
    class Function;
    class Argument;
    class DataLayout;
    class Instruction;
    class Value;
    class Module;
    class AAResults;
} // namespace llvm

namespace ctrace::concurrency::internal::analysis
{
    struct ResolvedSourceLocations
    {
        SourceLocation loweredLocation;
        SourceLocation userLocation;
    };

    /// Global identifiers the program defines somewhere, or null when the analysis sees a
    /// single unit. A unit alone cannot tell an `extern` backed by real storage from an
    /// unresolved symbol, so it drops both; a whole-project run knows the difference.
    using ProgramDefinedGlobals = std::unordered_set<std::string>;

    [[nodiscard]] bool shouldTrackSharedGlobal(const llvm::GlobalVariable& global,
                                               const ProgramDefinedGlobals* programDefined);

    struct FunctionBinding
    {
        const llvm::Function* function = nullptr;
        std::optional<unsigned> argumentIndex;
    };

    struct AliasResolvedGlobal
    {
        std::string symbol;
        /// The global the access was attributed to, so callers can weigh how much the answer is
        /// a resolution and how much it is a guess.
        const llvm::GlobalVariable* global = nullptr;
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

    /// Directory of the translation unit's own source, from its first compile unit. Used to tell
    /// code the user wrote from code a standard library header brought in.
    [[nodiscard]] std::optional<std::filesystem::path>
    primarySourceRoot(const llvm::Module& module);
    [[nodiscard]] bool isLikelyUserLocation(const SourceLocation& location,
                                            const std::optional<std::filesystem::path>& sourceRoot);

    [[nodiscard]] const llvm::GlobalVariable* resolveBaseGlobal(const llvm::Value& value);
    [[nodiscard]] std::optional<std::string> canonicalGlobalId(const llvm::Value& value);
    /// Canonical identity of a lock object, field-sensitive so that two mutexes stored in the same
    /// global aggregate are not conflated. Without a data layout the offset cannot be folded and
    /// every member of an aggregate shares the base identity, as before.
    [[nodiscard]] std::optional<std::string> canonicalLockId(const llvm::Value& value,
                                                             const llvm::DataLayout* layout);
    [[nodiscard]] std::optional<std::string> canonicalStorageGroupId(const llvm::Value& value);

    /// The module-level global a handle group id designates, or null when the id names a stack
    /// slot or a parameter. Only a global can be the same object in two translation units.
    [[nodiscard]] const llvm::GlobalVariable*
    globalOfStorageGroupId(const llvm::Module& module, std::string_view handleGroupId);

    /// Identity of a lock a function receives as a parameter, as seen from inside that function.
    /// It stands for whatever the caller passed, and is only meaningful while summarising the
    /// callee: every consumer sees it substituted by the caller's own lock.
    [[nodiscard]] std::optional<std::string> parameterLockId(const llvm::Value& value);

    /// Identity of a lock that is a field of the object a parameter points at.
    ///
    /// A thread-safe class keeps its mutex beside the data it guards, so once the object has an
    /// identity the lock has one too: the same base, plus the offset of the field. Without this
    /// the data would be identifiable and the lock protecting it would not, which reports every
    /// correctly guarded access as a race.
    [[nodiscard]] std::optional<std::string> objectFieldLockId(const llvm::Value& value,
                                                               const llvm::DataLayout* layout,
                                                               unsigned argumentIndex,
                                                               const std::string& objectId);
    /// Resolves the tracked root of a pointer, together with the byte range it designates.
    /// `byteSize` is the extent of the access; zero means unknown and conservatively covers the
    /// whole object.
    [[nodiscard]] std::optional<RootBinding>
    resolveTrackedRoot(const llvm::Value& value, const llvm::DataLayout* layout,
                       std::uint64_t byteSize,
                       const ProgramDefinedGlobals* programDefined = nullptr);
    [[nodiscard]] std::optional<RootBinding>
    resolveTrackedRoot(const llvm::Value& value,
                       const ProgramDefinedGlobals* programDefined = nullptr);
    [[nodiscard]] std::optional<AliasResolvedGlobal>
    resolveAliasGlobal(const llvm::Instruction& accessInstruction, llvm::AAResults& aaResults,
                       const std::vector<const llvm::GlobalVariable*>& candidateGlobals,
                       const ProgramDefinedGlobals* programDefined = nullptr);
    [[nodiscard]] std::optional<FunctionBinding> resolveFunctionBinding(const llvm::Value& value);
    [[nodiscard]] const llvm::Function* resolveFunctionValue(const llvm::Value& value);
    [[nodiscard]] std::string functionId(const llvm::Function& function);
    [[nodiscard]] std::string functionDisplayName(const llvm::Function& function);
    [[nodiscard]] ResolvedSourceLocations
    resolveSourceLocations(const llvm::Instruction& instruction);
    [[nodiscard]] SourceLocation makeSourceLocation(const llvm::Instruction& instruction);
} // namespace ctrace::concurrency::internal::analysis
