// SPDX-License-Identifier: Apache-2.0
#include "ir_utils.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <string_view>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::string normalizeValueName(llvm::StringRef name)
        {
            if (name.starts_with("\x01"))
                name = name.drop_front();
            return name.str();
        }

        bool canIgnoreLocalSlotUser(const llvm::User& user)
        {
            if (llvm::isa<llvm::DbgInfoIntrinsic>(user))
                return true;

            const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(&user);
            if (intrinsic == nullptr)
                return false;

            switch (intrinsic->getIntrinsicID())
            {
            case llvm::Intrinsic::lifetime_start:
            case llvm::Intrinsic::lifetime_end:
            case llvm::Intrinsic::dbg_declare:
            case llvm::Intrinsic::dbg_value:
            case llvm::Intrinsic::dbg_assign:
                return true;
            default:
                return false;
            }
        }

        const llvm::Value* followLocalPointerCopy(const llvm::LoadInst& load,
                                                  llvm::SmallPtrSetImpl<const llvm::Value*>& seen);
        const llvm::Value*
        followStoredPointerValue(const llvm::LoadInst& load,
                                 llvm::SmallPtrSetImpl<const llvm::Value*>& seen);

        std::string printValueOperand(const llvm::Value& value)
        {
            std::string rendered;
            llvm::raw_string_ostream stream(rendered);
            value.printAsOperand(stream, false);
            return stream.str();
        }

        template <typename GEPType> std::string printGepIndices(const GEPType& gep)
        {
            std::string rendered;
            for (const llvm::Value* index : gep.indices())
            {
                rendered += "[";
                if (const auto* constantIndex = llvm::dyn_cast<llvm::ConstantInt>(index))
                    rendered += std::to_string(constantIndex->getSExtValue());
                else
                    rendered += "*";
                rendered += "]";
            }
            return rendered;
        }

        SourceLocation sourceLocationFromDebugLocation(const llvm::DILocation& debugLocation,
                                                       std::string_view fallbackFunction)
        {
            SourceLocation location;
            location.function = std::string(fallbackFunction);

            if (const llvm::DISubprogram* subprogram = debugLocation.getScope()->getSubprogram())
            {
                if (!subprogram->getName().empty())
                    location.function = subprogram->getName().str();
            }

            location.line = debugLocation.getLine();
            location.column = debugLocation.getColumn();
            location.endLine = location.line;
            location.endColumn = location.column;

            const std::string filename = debugLocation.getFilename().str();
            const std::string directory = debugLocation.getDirectory().str();
            if (filename.empty())
                return location;

            std::filesystem::path filePath(filename);
            if (!directory.empty() && filePath.is_relative())
                filePath = std::filesystem::path(directory) / filePath;
            location.file = filePath.lexically_normal().string();
            return location;
        }

        /// Collected while walking from an access back to its root: the accumulated byte offset,
        /// and whether any traversed type is a synchronization primitive.
        struct AccessPathWalk
        {
            const llvm::DataLayout* layout = nullptr;
            bool touchesSyncPrimitive = false;
            bool touchesRecursiveLock = false;
            bool hasKnownOffset = true;
            std::int64_t byteOffset = 0;
            /// Size of the object the pointer designates, taken from the innermost indexing step.
            /// It bounds a coarse effect inferred for a call, which would otherwise cover the
            /// whole root and collide with every sibling field.
            std::uint64_t designatedSize = 0;

            [[nodiscard]] MemoryRegion region(std::uint64_t byteSize = 0) const
            {
                return MemoryRegion{
                    .hasKnownOffset = hasKnownOffset,
                    .byteOffset = byteOffset,
                    .byteSize = byteSize,
                };
            }
        };

        bool isSyncPrimitiveTypeName(llvm::StringRef name)
        {
            static constexpr std::string_view posixTypes[] = {
                "pthread_mutex_t",
                "pthread_rwlock_t",
                "pthread_cond_t",
                "pthread_spinlock_t",
                "pthread_barrier_t",
                "pthread_once_t",
                "pthread_mutexattr_t",
                "pthread_attr_t",
                "pthread_rwlockattr_t",
                "pthread_condattr_t",
                "sem_t",
            };
            for (const std::string_view posixType : posixTypes)
            {
                if (name.contains(posixType))
                    return true;
            }

            if (!name.contains("std::"))
                return false;

            static constexpr std::string_view stdTypes[] = {
                "::mutex",      "recursive_mutex",    "timed_mutex",
                "shared_mutex", "shared_timed_mutex", "condition_variable",
                "once_flag",    "counting_semaphore", "binary_semaphore",
                "::latch",      "::barrier",
            };
            for (const std::string_view stdType : stdTypes)
            {
                if (name.contains(stdType))
                    return true;
            }

            return false;
        }

        bool isRecursiveLockTypeName(llvm::StringRef name)
        {
            return name.contains("recursive_mutex") || name.contains("recursive_timed_mutex");
        }

        llvm::StringRef structuralTypeName(const llvm::Type* type)
        {
            const auto* structType = llvm::dyn_cast_or_null<llvm::StructType>(type);
            if (structType == nullptr || !structType->hasName())
                return {};

            return structType->getName();
        }

        /// Depth bound for looking through transparent wrappers. libstdc++ lowers its lock types
        /// to an unnamed struct holding nothing but the POSIX primitive, so the recognizable name
        /// sits below the type the pointer designates; libc++ names the outer type directly.
        constexpr unsigned kMaxTypeNestingDepth = 4;

        template <typename Predicate>
        bool matchesNestedTypeName(const llvm::Type* type, Predicate&& matches, unsigned depth = 0)
        {
            if (type == nullptr || depth > kMaxTypeNestingDepth)
                return false;

            if (const llvm::StringRef name = structuralTypeName(type);
                !name.empty() && matches(name))
            {
                return true;
            }

            if (const auto* arrayType = llvm::dyn_cast<llvm::ArrayType>(type))
                return matchesNestedTypeName(arrayType->getElementType(), matches, depth + 1);

            // Only a single-member struct is a wrapper. A user aggregate that merely *contains*
            // a mutex next to its data is not a lock: treating it as one would stop tracking the
            // data it guards.
            const auto* structType = llvm::dyn_cast<llvm::StructType>(type);
            if (structType == nullptr || structType->getNumElements() != 1)
                return false;

            return matchesNestedTypeName(structType->getElementType(0), matches, depth + 1);
        }

        bool isSynchronizationPrimitiveType(const llvm::Type* type)
        {
            return matchesNestedTypeName(type, isSyncPrimitiveTypeName);
        }

        bool isRecursiveLockType(const llvm::Type* type)
        {
            return matchesNestedTypeName(type, isRecursiveLockTypeName);
        }

        void noteTraversedType(AccessPathWalk& walk, const llvm::Type* type)
        {
            if (isSynchronizationPrimitiveType(type))
                walk.touchesSyncPrimitive = true;
            if (isRecursiveLockType(type))
                walk.touchesRecursiveLock = true;
        }

        std::uint64_t storeSizeOf(const AccessPathWalk& walk, llvm::Type* type)
        {
            if (walk.layout == nullptr || type == nullptr || !type->isSized())
                return 0;

            return walk.layout->getTypeStoreSize(type).getFixedValue();
        }

        template <typename GEPType> void noteGepTypes(AccessPathWalk& walk, const GEPType& gep)
        {
            noteTraversedType(walk, gep.getSourceElementType());
            noteTraversedType(walk, gep.getResultElementType());

            // The walk runs from the access towards the root, so the first step seen is the
            // innermost and the most precise.
            if (walk.designatedSize == 0)
                walk.designatedSize = storeSizeOf(walk, gep.getResultElementType());
        }

        /// Folds the GEP into the running byte offset. A variable index makes the offset unknown,
        /// which keeps the region conservatively overlapping every sibling.
        void accumulateGepOffset(AccessPathWalk& walk, const llvm::GEPOperator& gep)
        {
            if (!walk.hasKnownOffset)
                return;

            if (walk.layout == nullptr)
            {
                walk.hasKnownOffset = false;
                return;
            }

            llvm::APInt offset(walk.layout->getIndexTypeSizeInBits(gep.getType()), 0);
            if (!gep.accumulateConstantOffset(*walk.layout, offset))
            {
                walk.hasKnownOffset = false;
                return;
            }

            walk.byteOffset += offset.getSExtValue();
        }

        const llvm::Value* resolveCopiedValue(const llvm::Value& value,
                                              llvm::SmallPtrSetImpl<const llvm::Value*>& seen,
                                              AccessPathWalk* walk = nullptr)
        {
            const llvm::Value* current = value.stripPointerCastsAndAliases();
            while (current != nullptr)
            {
                if (!seen.insert(current).second)
                    return nullptr;

                if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                {
                    if (walk != nullptr)
                    {
                        noteTraversedType(*walk, global->getValueType());
                        if (walk->designatedSize == 0)
                            walk->designatedSize = storeSizeOf(*walk, global->getValueType());
                    }
                    return current;
                }

                if (llvm::isa<llvm::Argument>(current) || llvm::isa<llvm::Function>(current))
                    return current;

                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    if (walk != nullptr)
                    {
                        noteGepTypes(*walk, *gep);
                        accumulateGepOffset(*walk, *gep);
                    }
                    current = gep->getPointerOperand()->stripPointerCastsAndAliases();
                    continue;
                }

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(current))
                    return followLocalPointerCopy(*load, seen);

                return nullptr;
            }

            return nullptr;
        }

        const llvm::Value* followLocalPointerCopy(const llvm::LoadInst& load,
                                                  llvm::SmallPtrSetImpl<const llvm::Value*>& seen)
        {
            const llvm::Value* slot = load.getPointerOperand()->stripPointerCastsAndAliases();
            const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(slot);
            if (alloca == nullptr || !seen.insert(alloca).second)
                return nullptr;

            const llvm::Value* storedValue = nullptr;
            for (const llvm::User* user : alloca->users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand()->stripPointerCastsAndAliases() != alloca)
                        return nullptr;

                    const llvm::Value* candidate =
                        store->getValueOperand()->stripPointerCastsAndAliases();
                    if (storedValue == nullptr)
                        storedValue = candidate;
                    else if (storedValue != candidate)
                        return nullptr;
                    continue;
                }

                if (const auto* localLoad = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (localLoad->getPointerOperand()->stripPointerCastsAndAliases() != alloca)
                        return nullptr;
                    continue;
                }

                if (canIgnoreLocalSlotUser(*user))
                    continue;

                return nullptr;
            }

            if (storedValue == nullptr)
                return nullptr;

            return resolveCopiedValue(*storedValue, seen);
        }

        const llvm::Value* followStoredPointerValue(const llvm::LoadInst& load,
                                                    llvm::SmallPtrSetImpl<const llvm::Value*>& seen)
        {
            const llvm::Value* slot = load.getPointerOperand()->stripPointerCastsAndAliases();
            const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(slot);
            if (alloca == nullptr || !seen.insert(alloca).second)
                return nullptr;

            const llvm::Value* storedValue = nullptr;
            for (const llvm::User* user : alloca->users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand()->stripPointerCastsAndAliases() != alloca)
                        return nullptr;

                    const llvm::Value* candidate =
                        store->getValueOperand()->stripPointerCastsAndAliases();
                    if (storedValue == nullptr)
                        storedValue = candidate;
                    else if (storedValue != candidate)
                        return nullptr;
                    continue;
                }

                if (const auto* localLoad = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (localLoad->getPointerOperand()->stripPointerCastsAndAliases() != alloca)
                        return nullptr;
                    continue;
                }

                if (canIgnoreLocalSlotUser(*user))
                    continue;

                return nullptr;
            }

            return storedValue;
        }

        std::optional<AliasProvenance> aliasProvenanceFromResult(llvm::AliasResult aliasResult)
        {
            switch (aliasResult)
            {
            case llvm::AliasResult::NoAlias:
                return std::nullopt;
            case llvm::AliasResult::MustAlias:
                return AliasProvenance::MustAlias;
            case llvm::AliasResult::MayAlias:
            case llvm::AliasResult::PartialAlias:
                return AliasProvenance::MayAlias;
            }
            return std::nullopt;
        }
    } // namespace

    std::optional<std::filesystem::path> primarySourceRoot(const llvm::Module& module)
    {
        for (const llvm::DICompileUnit* compileUnit : module.debug_compile_units())
        {
            if (compileUnit == nullptr || compileUnit->getFile() == nullptr)
                continue;

            std::filesystem::path filePath(compileUnit->getFile()->getFilename().str());
            const std::string directory = compileUnit->getFile()->getDirectory().str();
            if (!directory.empty() && filePath.is_relative())
                filePath = std::filesystem::path(directory) / filePath;
            filePath = filePath.lexically_normal();
            if (!filePath.empty())
                return filePath.parent_path();
        }

        return std::nullopt;
    }

    bool isLikelyUserLocation(const SourceLocation& location,
                              const std::optional<std::filesystem::path>& sourceRoot)
    {
        if (location.file.empty() || !sourceRoot.has_value())
            return false;

        const std::filesystem::path filePath =
            std::filesystem::path(location.file).lexically_normal();
        const std::filesystem::path relativePath = filePath.lexically_relative(*sourceRoot);
        return !relativePath.empty() && *relativePath.begin() != "..";
    }

    bool shouldTrackSharedGlobal(const llvm::GlobalVariable& global,
                                 const ProgramDefinedGlobals* programDefined)
    {
        if (global.isConstant() || global.isThreadLocal())
            return false;

        // An `extern` with no definition in sight designates nothing this analysis can reason
        // about. Once the whole program is known, the same declaration may name real storage.
        if (global.isDeclaration() &&
            (programDefined == nullptr || !programDefined->contains(global.getGlobalIdentifier())))
        {
            return false;
        }

        return !isSynchronizationPrimitiveType(global.getValueType());
    }

    bool designatesSynchronizationPrimitive(const llvm::Value& pointerOperand)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        resolveCopiedValue(pointerOperand, seen, &walk);
        return walk.touchesSyncPrimitive;
    }

    bool designatesRecursiveLockType(const llvm::Value& pointerOperand)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        resolveCopiedValue(pointerOperand, seen, &walk);
        return walk.touchesRecursiveLock;
    }

    const llvm::GlobalVariable* resolveBaseGlobal(const llvm::Value& value)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        return llvm::dyn_cast_or_null<llvm::GlobalVariable>(resolveCopiedValue(value, seen));
    }

    std::optional<std::string> canonicalGlobalId(const llvm::Value& value)
    {
        const llvm::GlobalVariable* global = resolveBaseGlobal(value);
        if (global == nullptr)
            return std::nullopt;

        return normalizeValueName(global->getName());
    }

    std::optional<std::string> canonicalLockId(const llvm::Value& value,
                                               const llvm::DataLayout* layout)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        walk.layout = layout;
        const auto* global =
            llvm::dyn_cast_or_null<llvm::GlobalVariable>(resolveCopiedValue(value, seen, &walk));
        if (global == nullptr)
            return std::nullopt;

        return normalizeValueName(global->getName()) + walk.region().suffix();
    }

    std::optional<std::string> canonicalStorageGroupId(const llvm::Value& value)
    {
        const llvm::Value* current = &value;
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        std::vector<std::string> pathFragments;

        auto withPath = [&pathFragments](std::string baseId) -> std::string
        {
            for (auto it = pathFragments.rbegin(); it != pathFragments.rend(); ++it)
                baseId += *it;
            return baseId;
        };

        while (current != nullptr)
        {
            if (!seen.insert(current).second)
                return std::nullopt;

            if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                return withPath("global:" + normalizeValueName(global->getName()));

            if (const auto* argument = llvm::dyn_cast<llvm::Argument>(current))
            {
                return withPath("arg:" + functionId(*argument->getParent()) + ":" +
                                std::to_string(argument->getArgNo()));
            }

            if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(current))
            {
                return withPath("stack:" + functionId(*alloca->getFunction()) + ":" +
                                printValueOperand(*alloca));
            }

            if (const auto* gepInstruction = llvm::dyn_cast<llvm::GetElementPtrInst>(current))
            {
                pathFragments.push_back(printGepIndices(*gepInstruction));
                current = gepInstruction->getPointerOperand();
                continue;
            }

            if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
            {
                pathFragments.push_back(printGepIndices(*gep));
                current = gep->getPointerOperand();
                continue;
            }

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(current))
            {
                // Probe stored-pointer forwarding on a local copy of the visited set so a failed
                // probe does not poison the main traversal. This keeps the fallback path able to
                // resolve the load's storage slot itself, which is required for handles whose
                // value is produced by calls like pthread_create and later consumed by join/detach.
                llvm::SmallPtrSet<const llvm::Value*, 8> probeSeen = seen;
                if (const llvm::Value* copiedValue = followStoredPointerValue(*load, probeSeen))
                {
                    current = copiedValue;
                    continue;
                }

                current = load->getPointerOperand();
                continue;
            }

            const llvm::Value* stripped = current->stripPointerCastsAndAliases();
            if (stripped != current)
            {
                current = stripped;
                continue;
            }

            return std::nullopt;
        }

        return std::nullopt;
    }

    std::optional<RootBinding> resolveTrackedRoot(const llvm::Value& value,
                                                  const llvm::DataLayout* layout,
                                                  std::uint64_t byteSize,
                                                  const ProgramDefinedGlobals* programDefined)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        walk.layout = layout;
        const llvm::Value* root = resolveCopiedValue(value, seen, &walk);
        if (root == nullptr)
            return std::nullopt;

        // The runtime mutates lock and condition-variable objects under its own synchronization;
        // surfacing those mutations as shared-data conflicts reports the synchronization itself.
        if (walk.touchesSyncPrimitive)
            return std::nullopt;

        const MemoryRegion region = walk.region(byteSize != 0 ? byteSize : walk.designatedSize);
        if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(root))
        {
            if (!shouldTrackSharedGlobal(*global, programDefined))
                return std::nullopt;

            return RootBinding::global(normalizeValueName(global->getName()), region);
        }

        if (const auto* argument = llvm::dyn_cast<llvm::Argument>(root))
            return RootBinding::argument(argument->getArgNo(), region);

        return std::nullopt;
    }

    std::optional<RootBinding> resolveTrackedRoot(const llvm::Value& value,
                                                  const ProgramDefinedGlobals* programDefined)
    {
        return resolveTrackedRoot(value, nullptr, 0, programDefined);
    }

    std::optional<AliasResolvedGlobal>
    resolveAliasGlobal(const llvm::Instruction& accessInstruction, llvm::AAResults& aaResults,
                       const std::vector<const llvm::GlobalVariable*>& candidateGlobals,
                       const ProgramDefinedGlobals* programDefined)
    {
        const std::optional<llvm::MemoryLocation> accessLocation =
            llvm::MemoryLocation::getOrNone(&accessInstruction);
        if (!accessLocation.has_value())
            return std::nullopt;

        std::optional<std::string> mustAliasSymbol;
        std::optional<std::string> mayAliasSymbol;
        const llvm::GlobalVariable* mustAliasGlobal = nullptr;
        const llvm::GlobalVariable* mayAliasGlobal = nullptr;
        bool hasConflictingMustAlias = false;
        bool hasAmbiguousMayAlias = false;

        for (const llvm::GlobalVariable* global : candidateGlobals)
        {
            if (global == nullptr || !shouldTrackSharedGlobal(*global, programDefined))
                continue;

            const llvm::AliasResult aliasResult =
                aaResults.alias(*accessLocation, llvm::MemoryLocation::getBeforeOrAfter(global));
            const std::optional<AliasProvenance> aliasProvenance =
                aliasProvenanceFromResult(aliasResult);
            if (!aliasProvenance.has_value())
                continue;

            const std::string symbol = normalizeValueName(global->getName());
            if (*aliasProvenance == AliasProvenance::MustAlias)
            {
                if (!mustAliasSymbol.has_value())
                {
                    mustAliasSymbol = symbol;
                    mustAliasGlobal = global;
                }
                else if (*mustAliasSymbol != symbol)
                    hasConflictingMustAlias = true;

                continue;
            }

            if (!mayAliasSymbol.has_value())
            {
                mayAliasSymbol = symbol;
                mayAliasGlobal = global;
            }
            else if (*mayAliasSymbol != symbol)
                hasAmbiguousMayAlias = true;
        }

        if (!hasConflictingMustAlias && mustAliasSymbol.has_value())
        {
            return AliasResolvedGlobal{
                .symbol = *mustAliasSymbol,
                .global = mustAliasGlobal,
                .aliasProvenance = AliasProvenance::MustAlias,
            };
        }

        if (!hasAmbiguousMayAlias && mayAliasSymbol.has_value())
        {
            return AliasResolvedGlobal{
                .symbol = *mayAliasSymbol,
                .global = mayAliasGlobal,
                .aliasProvenance = AliasProvenance::MayAlias,
            };
        }

        return std::nullopt;
    }

    std::optional<FunctionBinding> resolveFunctionBinding(const llvm::Value& value)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        const llvm::Value* root = resolveCopiedValue(value, seen);
        if (root == nullptr)
            return std::nullopt;

        if (const auto* function = llvm::dyn_cast<llvm::Function>(root))
            return FunctionBinding{.function = function};

        if (const auto* argument = llvm::dyn_cast<llvm::Argument>(root))
            return FunctionBinding{.argumentIndex = argument->getArgNo()};

        return std::nullopt;
    }

    const llvm::Function* resolveFunctionValue(const llvm::Value& value)
    {
        const std::optional<FunctionBinding> binding = resolveFunctionBinding(value);
        if (!binding.has_value())
            return nullptr;

        return binding->function;
    }

    std::string functionId(const llvm::Function& function)
    {
        return normalizeValueName(function.getName());
    }

    std::string functionDisplayName(const llvm::Function& function)
    {
        if (const llvm::DISubprogram* subprogram = function.getSubprogram())
        {
            if (!subprogram->getName().empty())
                return subprogram->getName().str();
        }

        return functionId(function);
    }

    ResolvedSourceLocations resolveSourceLocations(const llvm::Instruction& instruction)
    {
        const std::string fallbackFunction = functionDisplayName(*instruction.getFunction());
        ResolvedSourceLocations locations;
        locations.loweredLocation.function = fallbackFunction;
        locations.userLocation.function = fallbackFunction;

        const llvm::DebugLoc debugLocation = instruction.getDebugLoc();
        if (!debugLocation)
            return locations;

        locations.loweredLocation =
            sourceLocationFromDebugLocation(*debugLocation, fallbackFunction);
        locations.userLocation = locations.loweredLocation;

        const llvm::DILocation* outermostInlineLocation = debugLocation.get();
        while (outermostInlineLocation != nullptr &&
               outermostInlineLocation->getInlinedAt() != nullptr)
            outermostInlineLocation = outermostInlineLocation->getInlinedAt();

        if (outermostInlineLocation != nullptr)
        {
            const SourceLocation candidate =
                sourceLocationFromDebugLocation(*outermostInlineLocation, fallbackFunction);
            if (candidate.line != 0 || candidate.column != 0 || !candidate.file.empty())
                locations.userLocation = candidate;
        }

        return locations;
    }

    SourceLocation makeSourceLocation(const llvm::Instruction& instruction)
    {
        return resolveSourceLocations(instruction).loweredLocation;
    }
} // namespace ctrace::concurrency::internal::analysis
