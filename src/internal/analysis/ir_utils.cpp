// SPDX-License-Identifier: Apache-2.0
#include "ir_utils.hpp"

#include <llvm/Analysis/ValueTracking.h>

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
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
        /// Prefix of a lock named after a parameter of the enclosing function.
        constexpr std::string_view kParameterLockPrefix = "%arg";

        /// A local object is named after its storage, marked apart from the same storage read
        /// as a pointer variable: the variable and what it points at are different objects.
        std::string localObjectName(const llvm::AllocaInst& storage)
        {
            const std::optional<std::string> storageId = canonicalStorageGroupId(storage);
            return storageId.has_value() ? "object:" + *storageId : std::string();
        }

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

        struct AccessPathWalk;
        const llvm::Value* followLocalPointerCopy(const llvm::LoadInst& load,
                                                  llvm::SmallPtrSetImpl<const llvm::Value*>& seen,
                                                  AccessPathWalk* walk);
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

        // An unnamed alloca has no textual name, and printAsOperand would supply one -- at the
        // cost of building a module-wide SlotTracker on every call, which walks every metadata
        // node in the module. All this identifier has to do is separate one stack slot from
        // another within a function and stay the same across runs, which its position does.
        std::string allocaSlotId(const llvm::AllocaInst& alloca)
        {
            if (alloca.hasName())
                return std::string(alloca.getName());

            std::size_t index = 0;
            for (const llvm::BasicBlock& block : *alloca.getFunction())
            {
                for (const llvm::Instruction& instruction : block)
                {
                    if (&instruction == &alloca)
                        return "%" + std::to_string(index);
                    if (llvm::isa<llvm::AllocaInst>(instruction))
                        ++index;
                }
            }
            return "%?";
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
            /// Size of the object the pointer designates, settled by the innermost indexing step
            /// that names one: a field, or the array an element belongs to. It bounds a coarse
            /// effect inferred for a call, which would otherwise cover the whole root and collide
            /// with every sibling field. Unset while only pointer arithmetic has been seen; zero
            /// once settled means unknown.
            std::optional<std::uint64_t> designatedSize;
            /// How far into the designated object the pointer sits. Non-zero for a pointer to an
            /// array element, which whoever receives it may move to any other element, on either
            /// side.
            std::int64_t designatedDisplacement = 0;
            /// Whether the walk may end at a local variable. Only the analyses naming a local
            /// object handed to a thread ask for that; everyone else stops short of locals.
            bool acceptsLocalStorage = false;

            [[nodiscard]] MemoryRegion region(std::uint64_t byteSize = 0) const
            {
                return MemoryRegion{
                    .hasKnownOffset = hasKnownOffset,
                    .byteOffset = byteOffset,
                    .byteSize = byteSize,
                };
            }

            [[nodiscard]] MemoryRegion designatedRegion() const
            {
                return MemoryRegion{
                    .hasKnownOffset = hasKnownOffset,
                    .byteOffset = byteOffset - designatedDisplacement,
                    .byteSize = designatedSize.value_or(0),
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

        /// Settles the designated object at `size` bytes. A pointer moved outside it by pointer
        /// arithmetic no longer points into it, so what it designates becomes unknown.
        void settleDesignatedSize(AccessPathWalk& walk, std::uint64_t size)
        {
            const std::int64_t displacement = walk.designatedDisplacement;
            const bool staysInside =
                displacement >= 0 && static_cast<std::uint64_t>(displacement) < size;
            walk.designatedSize = displacement == 0 || staysInside ? size : 0;
        }

        /// Settles what the pointer designates, from one indexing step of the walk.
        ///
        /// A step ending on a field index designates that field. A step ending on a run of array
        /// indices designates the array the run starts in, and the pointer sits that far into
        /// it. A run starting at the first index is pointer arithmetic: it moves a pointer that
        /// already designated an object, which a step further out, or the root, names.
        template <typename GEPType>
        void noteDesignatedObject(AccessPathWalk& walk, const GEPType& gep)
        {
            bool endsInArrayRun = false;
            bool runIsPointerArithmetic = false;
            llvm::Type* runArray = nullptr;
            llvm::Type* selectedSoFar = nullptr;
            std::int64_t runDisplacement = 0;
            bool isFirstIndex = true;
            for (auto step = llvm::gep_type_begin(gep); step != llvm::gep_type_end(gep); ++step)
            {
                if (step.isStruct())
                {
                    endsInArrayRun = false;
                }
                else
                {
                    if (!endsInArrayRun)
                    {
                        endsInArrayRun = true;
                        runIsPointerArithmetic = isFirstIndex;
                        runArray = selectedSoFar;
                        runDisplacement = 0;
                    }

                    // A variable index already makes the offset, and so the region, unknown.
                    const auto* index = llvm::dyn_cast<llvm::ConstantInt>(step.getOperand());
                    if (index != nullptr && walk.layout != nullptr)
                    {
                        runDisplacement +=
                            index->getSExtValue() *
                            static_cast<std::int64_t>(
                                step.getSequentialElementStride(*walk.layout).getFixedValue());
                    }
                }

                selectedSoFar = step.getIndexedType();
                isFirstIndex = false;
            }

            if (!endsInArrayRun)
            {
                settleDesignatedSize(walk, storeSizeOf(walk, gep.getResultElementType()));
                return;
            }

            walk.designatedDisplacement += runDisplacement;
            if (!runIsPointerArithmetic)
                settleDesignatedSize(walk, storeSizeOf(walk, runArray));
        }

        template <typename GEPType> void noteGepTypes(AccessPathWalk& walk, const GEPType& gep)
        {
            noteTraversedType(walk, gep.getSourceElementType());
            noteTraversedType(walk, gep.getResultElementType());

            // The walk runs from the access towards the root, so the first step that names an
            // object is the innermost and the most precise.
            if (!walk.designatedSize.has_value())
                noteDesignatedObject(walk, gep);
        }

        /// Folds the GEP into the running byte offset. A variable index makes the offset unknown,
        /// which keeps the region conservatively overlapping every sibling.
        /// Strips casts and aliases while leaving indexing steps in place.
        ///
        /// `stripPointerCastsAndAliases` also removes a GEP whose indices are all zero, because
        /// it computes the same address. It does not describe the same thing: the first field of
        /// an aggregate sits at offset zero, so that GEP is where its type and offset appear.
        const llvm::Value* stripCastsKeepingIndexing(const llvm::Value* value)
        {
            while (value != nullptr)
            {
                if (llvm::isa<llvm::GEPOperator>(value))
                    return value;

                const llvm::Value* stripped = value->stripPointerCastsAndAliases();
                if (stripped == value)
                    return value;

                value = stripped;
            }

            return value;
        }

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

        /// Resolves a call result only when every normal return names the same function.
        /// This includes lambda conversion operators without depending on their ABI names.
        /// Arguments and other computed returns stay unresolved: they belong to the callee's
        /// context and cannot be treated as values in the caller without substitution.
        const llvm::Function* constantReturnedFunction(const llvm::CallBase& call)
        {
            const auto* callee = llvm::dyn_cast<llvm::Function>(
                call.getCalledOperand()->stripPointerCastsAndAliases());
            if (callee == nullptr || callee->isDeclaration() || callee->isInterposable())
                return nullptr;

            const llvm::Function* result = nullptr;
            for (const llvm::BasicBlock& block : *callee)
            {
                const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
                if (ret == nullptr)
                    continue;

                const llvm::Value* returnedValue = ret->getReturnValue();
                if (returnedValue == nullptr)
                    return nullptr;

                const auto* candidate =
                    llvm::dyn_cast<llvm::Function>(returnedValue->stripPointerCastsAndAliases());
                if (candidate == nullptr || (result != nullptr && result != candidate))
                    return nullptr;

                result = candidate;
            }

            return result;
        }

        const llvm::Value* resolveCopiedValue(const llvm::Value& value,
                                              llvm::SmallPtrSetImpl<const llvm::Value*>& seen,
                                              AccessPathWalk* walk = nullptr)
        {
            const llvm::Value* current = stripCastsKeepingIndexing(&value);
            while (current != nullptr)
            {
                if (!seen.insert(current).second)
                    return nullptr;

                if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(current))
                {
                    if (walk != nullptr)
                    {
                        noteTraversedType(*walk, global->getValueType());
                        if (!walk->designatedSize.has_value())
                            settleDesignatedSize(*walk, storeSizeOf(*walk, global->getValueType()));
                    }
                    return current;
                }

                if (llvm::isa<llvm::Argument>(current) || llvm::isa<llvm::Function>(current))
                    return current;

                if (const auto* storage = llvm::dyn_cast<llvm::AllocaInst>(current))
                {
                    if (walk == nullptr || !walk->acceptsLocalStorage)
                        return nullptr;

                    noteTraversedType(*walk, storage->getAllocatedType());
                    if (!walk->designatedSize.has_value())
                    {
                        settleDesignatedSize(*walk,
                                             storeSizeOf(*walk, storage->getAllocatedType()));
                    }
                    return current;
                }

                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
                    if (walk != nullptr)
                    {
                        noteGepTypes(*walk, *gep);
                        accumulateGepOffset(*walk, *gep);
                    }
                    current = stripCastsKeepingIndexing(gep->getPointerOperand());
                    continue;
                }

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(current))
                    return followLocalPointerCopy(*load, seen, walk);

                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(current))
                    return constantReturnedFunction(*call);

                return nullptr;
            }

            return nullptr;
        }

        /// Follows a pointer read back from the local variable it was stored in. The indexing that
        /// produced the stored pointer belongs to the same access path as the indexing applied
        /// after the read, so the walk goes on through it.
        const llvm::Value* followLocalPointerCopy(const llvm::LoadInst& load,
                                                  llvm::SmallPtrSetImpl<const llvm::Value*>& seen,
                                                  AccessPathWalk* walk)
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
                        stripCastsKeepingIndexing(store->getValueOperand());
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

            return resolveCopiedValue(*storedValue, seen, walk);
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
                        stripCastsKeepingIndexing(store->getValueOperand());
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

    bool canBeSharedState(const llvm::GlobalVariable& global)
    {
        return !global.isConstant() && !global.isThreadLocal() &&
               !isSynchronizationPrimitiveType(global.getValueType());
    }

    bool shouldTrackSharedGlobal(const llvm::GlobalVariable& global,
                                 const ProgramDefinedGlobals* programDefined)
    {
        if (!canBeSharedState(global))
            return false;

        // An `extern` with no definition in sight designates nothing this analysis can reason
        // about. Once the whole program is known, the same declaration may name real storage.
        return !global.isDeclaration() || (programDefined != nullptr &&
                                           programDefined->contains(global.getGlobalIdentifier()));
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

    const llvm::GlobalVariable* globalOfStorageGroupId(const llvm::Module& module,
                                                       std::string_view handleGroupId)
    {
        constexpr std::string_view kGlobalPrefix = "global:";
        if (!handleGroupId.starts_with(kGlobalPrefix))
            return nullptr;

        // The id may carry an access path after the name; the object is the part before it.
        std::string_view name = handleGroupId.substr(kGlobalPrefix.size());
        const std::size_t end = name.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.$");
        if (end != std::string_view::npos)
            name = name.substr(0, end);

        if (name.empty())
            return nullptr;

        return module.getGlobalVariable(name, /*AllowInternal=*/true);
    }

    std::optional<RootBinding>
    resolveSharedObjectRoot(const llvm::Value& value, const llvm::DataLayout* layout,
                            std::uint64_t byteSize,
                            const std::unordered_set<std::string>& sharedObjectIds)
    {
        if (sharedObjectIds.empty())
            return std::nullopt;

        AccessPathWalk walk;
        walk.layout = layout;

        const llvm::Value* current = stripCastsKeepingIndexing(&value);
        while (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
        {
            noteGepTypes(walk, *gep);
            accumulateGepOffset(walk, *gep);
            current = stripCastsKeepingIndexing(gep->getPointerOperand());
        }

        // The load is where the pointer was read, and its operand is the slot holding it. That
        // slot is what the spawn site named, so the walk stops here rather than chasing the
        // value to an allocation that has no name.
        const auto* load = llvm::dyn_cast<llvm::LoadInst>(current);
        if (load == nullptr || walk.touchesSyncPrimitive)
            return std::nullopt;

        const std::optional<std::string> slotId =
            canonicalStorageGroupId(*load->getPointerOperand());
        if (!slotId.has_value() || !sharedObjectIds.contains(*slotId))
            return std::nullopt;

        RootBinding binding = RootBinding::global(*slotId, walk.region(byteSize));
        binding.designated = walk.designatedRegion();
        return binding;
    }

    std::optional<RootBinding> resolveLocalStorageRoot(const llvm::Value& value,
                                                       const llvm::DataLayout* layout,
                                                       std::uint64_t byteSize)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        walk.layout = layout;
        walk.acceptsLocalStorage = true;
        const auto* storage =
            llvm::dyn_cast_or_null<llvm::AllocaInst>(resolveCopiedValue(value, seen, &walk));
        if (storage == nullptr || walk.touchesSyncPrimitive)
            return std::nullopt;

        std::string name = localObjectName(*storage);
        if (name.empty())
            return std::nullopt;

        RootBinding binding = RootBinding::global(std::move(name), walk.region(byteSize));
        binding.designated = walk.designatedRegion();
        return binding;
    }

    std::optional<RootBinding>
    resolveLocalObjectRoot(const llvm::Value& value, const llvm::DataLayout* layout,
                           std::uint64_t byteSize,
                           const std::unordered_set<std::string>& localObjectIds)
    {
        if (localObjectIds.empty())
            return std::nullopt;

        std::optional<RootBinding> binding = resolveLocalStorageRoot(value, layout, byteSize);
        if (!binding.has_value() || !localObjectIds.contains(binding->symbol))
            return std::nullopt;
        return binding;
    }

    std::optional<std::string>
    localObjectLockId(const llvm::Value& value, const llvm::DataLayout* layout,
                      const std::unordered_set<std::string>& localObjectIds)
    {
        if (localObjectIds.empty())
            return std::nullopt;

        // The lock itself is a synchronization object, which is exactly what is being named
        // here, so the walk is not stopped by it the way a data access is.
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        walk.layout = layout;
        walk.acceptsLocalStorage = true;
        const auto* storage =
            llvm::dyn_cast_or_null<llvm::AllocaInst>(resolveCopiedValue(value, seen, &walk));
        if (storage == nullptr)
            return std::nullopt;

        const std::string name = localObjectName(*storage);
        if (name.empty() || !localObjectIds.contains(name))
            return std::nullopt;

        return name + walk.region().suffix();
    }

    std::optional<std::string> objectFieldLockId(const llvm::Value& value,
                                                 const llvm::DataLayout* layout,
                                                 unsigned argumentIndex, const RootBinding& object)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        walk.layout = layout;
        const auto* argument =
            llvm::dyn_cast_or_null<llvm::Argument>(resolveCopiedValue(value, seen, &walk));
        if (argument == nullptr || argument->getArgNo() != argumentIndex)
            return std::nullopt;

        return lockIdAt(object, walk.region());
    }

    std::optional<std::string> parameterFieldLockId(const llvm::Value& value,
                                                    const llvm::DataLayout* layout)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        AccessPathWalk walk;
        walk.layout = layout;
        const auto* argument =
            llvm::dyn_cast_or_null<llvm::Argument>(resolveCopiedValue(value, seen, &walk));
        if (argument == nullptr)
            return std::nullopt;

        return lockIdAt(RootBinding::argument(argument->getArgNo()), walk.region());
    }

    std::optional<ParameterLockPlace> parameterLockPlace(std::string_view lockId)
    {
        if (!lockId.starts_with(kParameterLockPrefix))
            return std::nullopt;

        std::string_view rest = lockId.substr(kParameterLockPrefix.size());
        const std::size_t digits = rest.find_first_not_of("0123456789");
        const std::string_view index = rest.substr(0, digits);
        if (index.empty())
            return std::nullopt;

        ParameterLockPlace result;
        result.argumentIndex = static_cast<unsigned>(std::stoul(std::string(index)));
        const std::string_view suffix =
            digits == std::string_view::npos ? std::string_view{} : rest.substr(digits);
        if (suffix == "[*]")
            result.place.hasKnownOffset = false;
        else if (suffix.starts_with("+"))
            result.place.byteOffset = std::stoll(std::string(suffix.substr(1)));
        else if (!suffix.empty())
            return std::nullopt;
        return result;
    }

    std::string lockIdAt(const RootBinding& object, const MemoryRegion& place)
    {
        const MemoryRegion position{
            .hasKnownOffset = object.region.hasKnownOffset && place.hasKnownOffset,
            .byteOffset = object.region.byteOffset + place.byteOffset,
        };
        if (object.kind == RootBindingKind::Argument)
        {
            return std::string(kParameterLockPrefix) + std::to_string(object.argumentIndex) +
                   position.suffix();
        }
        return object.symbol + position.suffix();
    }

    std::optional<std::string> parameterLockId(const llvm::Value& value)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        const auto* argument =
            llvm::dyn_cast_or_null<llvm::Argument>(resolveCopiedValue(value, seen, nullptr));
        if (argument == nullptr)
            return std::nullopt;

        return std::string(kParameterLockPrefix) + std::to_string(argument->getArgNo());
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
                                allocaSlotId(*alloca));
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

        std::optional<RootBinding> binding;
        if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(root))
        {
            if (!shouldTrackSharedGlobal(*global, programDefined))
                return std::nullopt;

            binding =
                RootBinding::global(normalizeValueName(global->getName()), walk.region(byteSize));
        }
        else if (const auto* argument = llvm::dyn_cast<llvm::Argument>(root))
        {
            binding = RootBinding::argument(argument->getArgNo(), walk.region(byteSize));
        }

        if (binding.has_value())
            binding->designated = walk.designatedRegion();
        return binding;
    }

    namespace
    {
        const llvm::Function* calledFunction(const llvm::CallBase& call)
        {
            return llvm::dyn_cast<llvm::Function>(
                call.getCalledOperand()->stripPointerCastsAndAliases());
        }

        /// Whether a pointer may have come from somewhere the module cannot see: an indirect
        /// call, or a call whose body is not in the module.
        ///
        /// Alias analysis reports may-alias for every pointer it cannot identify, so against a
        /// global the answer only says something when the pointer could have been handed in
        /// from outside. A pointer loaded out of a container the module itself built is
        /// unidentified too, yet it cannot reach a global; accepting the answer for it named
        /// whichever global the module happened to define.
        ///
        /// Calls to defined functions are followed through what they return: nothing was
        /// inlined at the level analysed, so an accessor is a call rather than the load it
        /// wraps. A callee returning one of its own parameters is read as the argument it was
        /// given. A parameter of the function being analysed is not opaque: what it holds is
        /// decided at its call sites, which is the argument binding's job, not this one's.
        bool pointerProvenanceIsOpaque(const llvm::Value& pointer,
                                       llvm::SmallPtrSetImpl<const llvm::Value*>& seen,
                                       const llvm::CallBase* boundCall)
        {
            const llvm::Value* object = llvm::getUnderlyingObject(&pointer);
            if (object == nullptr || !seen.insert(object).second)
                return false;

            if (const auto* argument = llvm::dyn_cast<llvm::Argument>(object))
            {
                const bool isBoundParameter = boundCall != nullptr &&
                                              argument->getParent() == calledFunction(*boundCall) &&
                                              argument->getArgNo() < boundCall->arg_size();
                if (!isBoundParameter)
                    return false;

                return pointerProvenanceIsOpaque(*boundCall->getArgOperand(argument->getArgNo()),
                                                 seen, nullptr);
            }

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(object))
            {
                // A local pointer variable is whatever was stored into it. A load from any
                // other memory is a pointer the module wrote there itself.
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                    llvm::getUnderlyingObject(load->getPointerOperand()));
                if (slot == nullptr)
                    return false;

                for (const llvm::User* user : slot->users())
                {
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (store == nullptr || store->getPointerOperand() != slot)
                        continue;
                    if (pointerProvenanceIsOpaque(*store->getValueOperand(), seen, boundCall))
                        return true;
                }
                return false;
            }

            if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(object))
            {
                for (const llvm::Value* incoming : phi->incoming_values())
                    if (pointerProvenanceIsOpaque(*incoming, seen, boundCall))
                        return true;
                return false;
            }

            if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(object))
            {
                return pointerProvenanceIsOpaque(*select->getTrueValue(), seen, boundCall) ||
                       pointerProvenanceIsOpaque(*select->getFalseValue(), seen, boundCall);
            }

            const auto* call = llvm::dyn_cast<llvm::CallBase>(object);
            if (call == nullptr)
                return false;

            const llvm::Function* callee = calledFunction(*call);
            if (callee == nullptr)
                return true;
            if (callee->isIntrinsic())
                return false;
            if (callee->isDeclaration())
                return true;

            for (const llvm::BasicBlock& block : *callee)
            {
                const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
                if (ret == nullptr || ret->getReturnValue() == nullptr)
                    continue;
                if (pointerProvenanceIsOpaque(*ret->getReturnValue(), seen, call))
                    return true;
            }
            return false;
        }
    } // namespace

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
            // With a lone candidate the ambiguity check above cannot fail, so the may-alias
            // answer must be qualified by where the pointer came from.
            llvm::SmallPtrSet<const llvm::Value*, 8> seen;
            if (!pointerProvenanceIsOpaque(*accessLocation->Ptr, seen, nullptr))
                return std::nullopt;

            return AliasResolvedGlobal{
                .symbol = *mayAliasSymbol,
                .global = mayAliasGlobal,
                .aliasProvenance = AliasProvenance::MayAlias,
            };
        }

        return std::nullopt;
    }

    namespace
    {
        /// The function a pointer-to-member denotes, when it denotes one at all.
        ///
        /// The Itanium ABI represents `&Service::run` as a pair: the function address and the
        /// adjustment to apply to `this`. It is a struct, not a pointer, so the copy walk that
        /// resolves ordinary callables never reaches it — which is why a thread started from a
        /// member function had no entry at all.
        ///
        /// Only the plain case is accepted. A non-zero adjustment means multiple inheritance and
        /// the callee still depends on which base the object is viewed as; an odd first field
        /// encodes a vtable offset, and the target is then whatever the object turns out to be.
        /// Neither is a single function this analysis could name.
        const llvm::Function* memberFunctionBehind(const llvm::Value& value)
        {
            const auto* storage =
                llvm::dyn_cast<llvm::AllocaInst>(llvm::getUnderlyingObject(&value));
            if (storage == nullptr)
                return nullptr;

            for (const llvm::User* user : storage->users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr || store->getPointerOperand() != storage)
                    continue;

                const auto* pair = llvm::dyn_cast<llvm::ConstantStruct>(store->getValueOperand());
                if (pair == nullptr || pair->getNumOperands() != 2)
                    continue;

                const auto* adjustment = llvm::dyn_cast<llvm::ConstantInt>(pair->getOperand(1));
                if (adjustment == nullptr || !adjustment->isZero())
                    continue;

                const auto* address = llvm::dyn_cast<llvm::ConstantExpr>(pair->getOperand(0));
                if (address == nullptr || address->getOpcode() != llvm::Instruction::PtrToInt)
                    continue;

                if (const auto* function =
                        llvm::dyn_cast<llvm::Function>(address->getOperand(0)->stripPointerCasts()))
                {
                    return function;
                }
            }

            return nullptr;
        }
    } // namespace

    std::optional<FunctionBinding> resolveFunctionBinding(const llvm::Value& value)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        if (const llvm::Value* root = resolveCopiedValue(value, seen); root != nullptr)
        {
            if (const auto* function = llvm::dyn_cast<llvm::Function>(root))
                return FunctionBinding{.function = function};

            if (const auto* argument = llvm::dyn_cast<llvm::Argument>(root))
                return FunctionBinding{.argumentIndex = argument->getArgNo()};
        }

        // The copy walk resolves pointers, and a pointer-to-member is a struct: it never
        // reaches one, so this is tried after rather than instead.
        if (const llvm::Function* member = memberFunctionBehind(value))
            return FunctionBinding{.function = member};

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
