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

    /// True when a global is storage threads can share, whether or not this unit defines it:
    /// not constant, not thread-local, and not a synchronization object the runtime owns.
    [[nodiscard]] bool canBeSharedState(const llvm::GlobalVariable& global);

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
    /// The global, or the parameter, a pointer is based on, following copies through local
    /// variables; null for anything else. A parameter stands for whatever object its caller passes.
    [[nodiscard]] const llvm::Value* resolveBaseObject(const llvm::Value& value);
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

    /// Identity of a lock that is, or is a field of, the object a parameter points at, as seen
    /// from inside the function: the parameter's placeholder and the field's offset. Like
    /// `parameterLockId`, it only means something once a call site substitutes what it passes.
    [[nodiscard]] std::optional<std::string> parameterFieldLockId(const llvm::Value& value,
                                                                  const llvm::DataLayout* layout);

    /// Where a parameter lock id points: the parameter, and the place in the object it passes.
    struct ParameterLockPlace
    {
        unsigned argumentIndex = 0;
        MemoryRegion place;
    };

    /// Reads back a parameter lock id; nothing for any other lock id.
    [[nodiscard]] std::optional<ParameterLockPlace> parameterLockPlace(std::string_view lockId);

    /// The id of the lock at `place` within `object`: the object's name and the combined offset
    /// for a named object, the caller's own parameter placeholder for one it received itself.
    [[nodiscard]] std::string lockIdAt(const RootBinding& object, const MemoryRegion& place);

    /// Reads back the object a lock id names and the place in it. Every lock id is its object's
    /// name followed by the suffix of that place, so `lockIdAt(lockIdRoot(id), {})` is `id`.
    [[nodiscard]] RootBinding lockIdRoot(std::string_view lockId);

    /// Identity of an access made through a pointer the program holds in a named slot, when
    /// that slot is one the spawn sites identified as shared.
    ///
    /// The thread that hands an object over usually keeps using it, and its own accesses reach
    /// the object by loading the same variable the spawn read. Following that load to the
    /// allocation gives nothing nameable; stopping at the slot gives the identity both sides
    /// already agree on.
    [[nodiscard]] std::optional<RootBinding>
    resolveSharedObjectRoot(const llvm::Value& value, const llvm::DataLayout* layout,
                            std::uint64_t byteSize,
                            const std::unordered_set<std::string>& sharedObjectIds);

    /// The local variable a pointer leads into, named after its storage, with where the pointer
    /// points in it and what it designates. Whether that local is anything more than a local is
    /// for the caller to decide.
    [[nodiscard]] std::optional<RootBinding> resolveLocalStorageRoot(const llvm::Value& value,
                                                                     const llvm::DataLayout* layout,
                                                                     std::uint64_t byteSize);

    /// Identity of an access to a local object the spawn sites handed to a thread, under the
    /// same name the thread's own accesses to it receive. Any other local is left out.
    [[nodiscard]] std::optional<RootBinding>
    resolveLocalObjectRoot(const llvm::Value& value, const llvm::DataLayout* layout,
                           std::uint64_t byteSize,
                           const std::unordered_set<std::string>& localObjectIds);

    /// Identity of a lock that is a field of a local object handed to a thread.
    [[nodiscard]] std::optional<std::string>
    localObjectLockId(const llvm::Value& value, const llvm::DataLayout* layout,
                      const std::unordered_set<std::string>& localObjectIds);

    /// Identity of a lock that is a field of the object a parameter points at.
    ///
    /// A thread-safe class keeps its mutex beside the data it guards, so once the object has an
    /// identity the lock has one too: the same base, plus the offset of the field. Without this
    /// the data would be identifiable and the lock protecting it would not, which reports every
    /// correctly guarded access as a race.
    [[nodiscard]] std::optional<std::string> objectFieldLockId(const llvm::Value& value,
                                                               const llvm::DataLayout* layout,
                                                               unsigned argumentIndex,
                                                               const RootBinding& object);
    /// A pointer into the element of an array that one variable index picks, every other
    /// indexing step being constant: `&base[c][index].field`.
    struct VariablyIndexedPointer
    {
        /// The object indexed, before any indexing step.
        const llvm::Value* base = nullptr;
        /// The variable index, without the integer extension the lowering adds.
        const llvm::Value* index = nullptr;
        /// Where element zero starts in `base`, and the distance between two elements.
        std::int64_t elementStart = 0;
        std::uint64_t elementSize = 0;
        /// Where the pointer sits inside its element.
        std::int64_t position = 0;
    };

    /// Reads a pointer as a chain of indexing steps with exactly one variable index; nothing for
    /// any other pointer. Only casts and indexing steps are followed: a pointer read back from
    /// memory may have been computed in an earlier iteration, with another index.
    [[nodiscard]] std::optional<VariablyIndexedPointer>
    variablyIndexedPointer(const llvm::Value& pointer, const llvm::DataLayout& layout);

    /// Resolves the tracked root of a pointer: where in it the pointer points, with `byteSize` as
    /// the extent (zero when unknown), and the object the pointer designates. When an integer
    /// parameter of the function picks the place, that is recorded as its `index`.
    [[nodiscard]] std::optional<RootBinding>
    resolveTrackedRoot(const llvm::Value& value, const llvm::DataLayout* layout,
                       std::uint64_t byteSize,
                       const ProgramDefinedGlobals* programDefined = nullptr);
    /// An integer as the enclosing function receives or fixes it: a non-negative constant, or
    /// one of its integer parameters, including through the local variable unoptimized code keeps
    /// it in and a sign or zero extension. Anything the function computes from it is left unknown.
    [[nodiscard]] std::optional<LinearIndex> resolveLinearIndex(const llvm::Value& value);
    /// An integer parameter of the enclosing function an integer is computed from, through local
    /// variables, conversions and arithmetic; empty when it comes from none.
    [[nodiscard]] std::optional<unsigned> integerSourceParameter(const llvm::Value& value);
    /// `outer` with its parameter replaced by `inner`; empty when the arithmetic overflows.
    /// Widened as soon as either is.
    [[nodiscard]] std::optional<LinearIndex> substituteLinearIndex(const LinearIndex& outer,
                                                                   const LinearIndex& inner);
    [[nodiscard]] std::optional<AliasResolvedGlobal>
    resolveAliasGlobal(const llvm::Instruction& accessInstruction, llvm::AAResults& aaResults,
                       const std::vector<const llvm::GlobalVariable*>& candidateGlobals,
                       const ProgramDefinedGlobals* programDefined = nullptr);
    [[nodiscard]] std::optional<FunctionBinding> resolveFunctionBinding(const llvm::Value& value);
    [[nodiscard]] const llvm::Function* resolveFunctionValue(const llvm::Value& value);
    [[nodiscard]] std::string functionId(const llvm::Function& function);
    [[nodiscard]] std::string functionDisplayName(const llvm::Function& function);

    /// A function's name read from its mangled one: the scope it is declared in and its own name,
    /// without template arguments, parameters, return type or libc++'s ABI tags.
    struct DemangledName
    {
        std::string context;
        std::string base;
        /// The function constructs an object of its class, handed as its first parameter.
        bool constructor = false;
    };

    /// Nothing when `mangled` names no function.
    [[nodiscard]] std::optional<DemangledName> demangleFunction(std::string_view mangled);
    [[nodiscard]] ResolvedSourceLocations
    resolveSourceLocations(const llvm::Instruction& instruction);
    [[nodiscard]] SourceLocation makeSourceLocation(const llvm::Instruction& instruction);
} // namespace ctrace::concurrency::internal::analysis
