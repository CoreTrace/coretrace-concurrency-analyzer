// SPDX-License-Identifier: Apache-2.0
#include "shared_access_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/Support/ModRef.h>

#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::string rootBindingKey(const RootBinding& binding)
        {
            if (binding.kind == RootBindingKind::Global)
                return "global:" + binding.symbol;

            return "argument:" + std::to_string(binding.argumentIndex);
        }

        std::string callEffectKey(const RootBinding& binding, AccessKind kind)
        {
            std::ostringstream stream;
            stream << rootBindingKey(binding) << "|" << toString(kind);
            return stream.str();
        }

        bool shouldInferCallMemoryEffects(const llvm::CallBase& call,
                                          const ConcurrencySymbolClassifier& classifier)
        {
            if (llvm::isa<llvm::DbgInfoIntrinsic>(call))
                return false;

            if (llvm::isa<llvm::MemIntrinsic>(call))
                return false;

            return classifier.classify(call) == CallKind::Unknown;
        }

        void appendAccess(std::vector<PendingAccess>& accesses, const llvm::Function& function,
                          const llvm::Instruction& instruction, const llvm::Value& pointerOperand,
                          AccessKind kind, AliasProvenance aliasProvenance)
        {
            const std::optional<RootBinding> root = resolveTrackedRoot(pointerOperand);
            if (!root.has_value())
                return;

            PendingAccess access;
            access.function = &function;
            access.instruction = &instruction;
            access.root = *root;
            access.fact.functionId = functionId(function);
            access.fact.kind = kind;
            access.fact.aliasProvenance = aliasProvenance;
            const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
            access.fact.loweredLocation = locations.loweredLocation;
            access.fact.userLocation = locations.userLocation;
            accesses.push_back(std::move(access));
        }

        void appendMemoryIntrinsicAccesses(std::vector<PendingAccess>& accesses,
                                           const llvm::Function& function,
                                           const llvm::MemIntrinsic& intrinsic)
        {
            if (const auto* transfer = llvm::dyn_cast<llvm::MemTransferInst>(&intrinsic))
            {
                appendAccess(accesses, function, intrinsic, *transfer->getRawDest(),
                             AccessKind::Write, AliasProvenance::Direct);
                appendAccess(accesses, function, intrinsic, *transfer->getRawSource(),
                             AccessKind::Read, AliasProvenance::Direct);
                return;
            }

            appendAccess(accesses, function, intrinsic, *intrinsic.getRawDest(), AccessKind::Write,
                         AliasProvenance::Direct);
        }

        std::optional<AccessKind> accessKindFromModRef(llvm::ModRefInfo modRefInfo)
        {
            if (llvm::isModSet(modRefInfo))
                return AccessKind::Write;

            if (llvm::isRefSet(modRefInfo))
                return AccessKind::Read;

            return std::nullopt;
        }

        void appendCallMemoryEffectAccesses(std::vector<PendingAccess>& accesses,
                                            const llvm::Function& function,
                                            const llvm::CallBase& call, llvm::AAResults& aaResults,
                                            const ConcurrencySymbolClassifier& classifier)
        {
            if (!shouldInferCallMemoryEffects(call, classifier))
                return;

            std::unordered_set<std::string> seenEffects;
            for (const llvm::Use& argument : call.args())
            {
                const llvm::Value* value = argument.get();
                if (value == nullptr || !value->getType()->isPointerTy())
                    continue;

                const std::optional<RootBinding> root = resolveTrackedRoot(*value);
                if (!root.has_value())
                    continue;

                const llvm::MemoryLocation location = llvm::MemoryLocation::getBeforeOrAfter(value);
                const std::optional<AccessKind> kind =
                    accessKindFromModRef(aaResults.getModRefInfo(&call, location));
                if (!kind.has_value())
                    continue;

                const std::string key = callEffectKey(*root, *kind);
                if (!seenEffects.insert(key).second)
                    continue;

                appendAccess(accesses, function, call, *value, *kind, AliasProvenance::Direct);
            }
        }
    } // namespace

    std::vector<PendingAccess> SharedAccessCollector::collect(const llvm::Module& module) const
    {
        std::vector<PendingAccess> accesses;
        std::vector<const llvm::GlobalVariable*> trackedGlobals;
        trackedGlobals.reserve(module.global_size());
        for (const llvm::GlobalVariable& global : module.globals())
        {
            if (shouldTrackSharedGlobal(global))
                trackedGlobals.push_back(&global);
        }

        LlvmFunctionAnalysisProvider analysisProvider;
        ConcurrencySymbolClassifier classifier;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::AAResults& aaResults = analysisProvider.getAAResults(function);

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const llvm::Value* pointerOperand = nullptr;
                    AccessKind kind = AccessKind::Read;
                    AliasProvenance aliasProvenance = AliasProvenance::Direct;

                    if (const auto* intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(&instruction))
                    {
                        appendMemoryIntrinsicAccesses(accesses, function, *intrinsic);
                        continue;
                    }

                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        appendCallMemoryEffectAccesses(accesses, function, *call, aaResults,
                                                       classifier);
                        continue;
                    }

                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    {
                        pointerOperand = load->getPointerOperand();
                        kind = AccessKind::Read;
                    }
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        pointerOperand = store->getPointerOperand();
                        kind = AccessKind::Write;
                    }
                    else
                    {
                        continue;
                    }

                    if (pointerOperand == nullptr)
                        continue;

                    std::optional<RootBinding> root = resolveTrackedRoot(*pointerOperand);
                    if (!root.has_value())
                    {
                        const std::optional<AliasResolvedGlobal> aliasResolvedGlobal =
                            resolveAliasGlobal(instruction, aaResults, trackedGlobals);
                        if (!aliasResolvedGlobal.has_value())
                            continue;

                        root = RootBinding::global(aliasResolvedGlobal->symbol);
                        aliasProvenance = aliasResolvedGlobal->aliasProvenance;
                    }

                    PendingAccess access;
                    access.function = &function;
                    access.instruction = &instruction;
                    access.root = *root;
                    access.fact.functionId = functionId(function);
                    access.fact.kind = kind;
                    access.fact.aliasProvenance = aliasProvenance;
                    const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
                    access.fact.loweredLocation = locations.loweredLocation;
                    access.fact.userLocation = locations.userLocation;
                    accesses.push_back(std::move(access));
                }
            }
        }

        return accesses;
    }
} // namespace ctrace::concurrency::internal::analysis
