// SPDX-License-Identifier: Apache-2.0
#include "ir_utils.hpp"

#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Value.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
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

        const llvm::Value* resolveCopiedValue(const llvm::Value& value,
                                              llvm::SmallPtrSetImpl<const llvm::Value*>& seen)
        {
            const llvm::Value* current = value.stripPointerCastsAndAliases();
            while (current != nullptr)
            {
                if (!seen.insert(current).second)
                    return nullptr;

                if (llvm::isa<llvm::GlobalVariable>(current) ||
                    llvm::isa<llvm::Argument>(current) || llvm::isa<llvm::Function>(current))
                    return current;

                if (const auto* gepInstruction = llvm::dyn_cast<llvm::GetElementPtrInst>(current))
                {
                    current = gepInstruction->getPointerOperand()->stripPointerCastsAndAliases();
                    continue;
                }

                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(current))
                {
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

    bool shouldTrackSharedGlobal(const llvm::GlobalVariable& global)
    {
        return !global.isDeclaration() && !global.isConstant() && !global.isThreadLocal();
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

    std::optional<RootBinding> resolveTrackedRoot(const llvm::Value& value)
    {
        llvm::SmallPtrSet<const llvm::Value*, 8> seen;
        const llvm::Value* root = resolveCopiedValue(value, seen);
        if (root == nullptr)
            return std::nullopt;

        if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(root))
        {
            if (!shouldTrackSharedGlobal(*global))
                return std::nullopt;

            return RootBinding::global(normalizeValueName(global->getName()));
        }

        if (const auto* argument = llvm::dyn_cast<llvm::Argument>(root))
            return RootBinding::argument(argument->getArgNo());

        return std::nullopt;
    }

    std::optional<AliasResolvedGlobal> resolveAliasGlobal(
        const llvm::Instruction& accessInstruction, llvm::AAResults& aaResults,
        const std::vector<const llvm::GlobalVariable*>& candidateGlobals)
    {
        const std::optional<llvm::MemoryLocation> accessLocation =
            llvm::MemoryLocation::getOrNone(&accessInstruction);
        if (!accessLocation.has_value())
            return std::nullopt;

        std::optional<std::string> mustAliasSymbol;
        std::optional<std::string> mayAliasSymbol;
        bool hasConflictingMustAlias = false;
        bool hasAmbiguousMayAlias = false;

        for (const llvm::GlobalVariable* global : candidateGlobals)
        {
            if (global == nullptr || !shouldTrackSharedGlobal(*global))
                continue;

            const llvm::AliasResult aliasResult = aaResults.alias(
                *accessLocation, llvm::MemoryLocation::getBeforeOrAfter(global));
            const std::optional<AliasProvenance> aliasProvenance =
                aliasProvenanceFromResult(aliasResult);
            if (!aliasProvenance.has_value())
                continue;

            const std::string symbol = normalizeValueName(global->getName());
            if (*aliasProvenance == AliasProvenance::MustAlias)
            {
                if (!mustAliasSymbol.has_value())
                    mustAliasSymbol = symbol;
                else if (*mustAliasSymbol != symbol)
                    hasConflictingMustAlias = true;

                continue;
            }

            if (!mayAliasSymbol.has_value())
                mayAliasSymbol = symbol;
            else if (*mayAliasSymbol != symbol)
                hasAmbiguousMayAlias = true;
        }

        if (!hasConflictingMustAlias && mustAliasSymbol.has_value())
        {
            return AliasResolvedGlobal{
                .symbol = *mustAliasSymbol,
                .aliasProvenance = AliasProvenance::MustAlias,
            };
        }

        if (!hasAmbiguousMayAlias && mayAliasSymbol.has_value())
        {
            return AliasResolvedGlobal{
                .symbol = *mayAliasSymbol,
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
