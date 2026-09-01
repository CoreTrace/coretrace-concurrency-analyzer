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
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/Support/ModRef.h>

#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::string rootBindingKey(const RootBinding& binding)
        {
            if (binding.kind == RootBindingKind::Global)
                return "global:" + binding.symbol + binding.region.suffix();

            return "argument:" + std::to_string(binding.argumentIndex) + binding.region.suffix();
        }

        /// Extent of a scalar access, so that writes to adjacent fields do not appear to overlap.
        std::uint64_t accessByteSize(const llvm::DataLayout& layout, const llvm::Type* accessedType)
        {
            if (accessedType == nullptr || !accessedType->isSized())
                return 0;

            return layout.getTypeStoreSize(const_cast<llvm::Type*>(accessedType)).getFixedValue();
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

        /// True when every memory operation a callee performs on memory it does not own is atomic,
        /// following direct calls. The coarse effect inferred for such a call summarizes atomic
        /// work only, so it must not be reported as a plain access: that is what turned a correct
        /// `std::atomic` wrapper into a race.
        ///
        /// Answering this from the callee body rather than from its interprocedural summary keeps
        /// the result identical whether or not the summary reaches the caller, which differs
        /// between standard library implementations.
        using AtomicOnlyCache = std::unordered_map<const llvm::Function*, bool>;

        bool accessesMemoryAtomicallyOnly(const llvm::Function& function,
                                          const ConcurrencySymbolClassifier& classifier,
                                          AtomicOnlyCache& cache,
                                          std::unordered_set<const llvm::Function*>& visiting)
        {
            if (const auto cached = cache.find(&function); cached != cache.end())
                return cached->second;

            if (function.isDeclaration())
                return false;

            // A cycle is assumed atomic-only; a genuine plain access elsewhere in it still
            // decides the answer.
            if (!visiting.insert(&function).second)
                return true;

            bool sawAtomic = false;
            bool atomicOnly = true;
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    if (llvm::isa<llvm::AtomicRMWInst>(instruction) ||
                        llvm::isa<llvm::AtomicCmpXchgInst>(instruction))
                    {
                        sawAtomic = true;
                        continue;
                    }

                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    {
                        if (llvm::isa<llvm::AllocaInst>(
                                load->getPointerOperand()->stripPointerCastsAndAliases()))
                            continue;

                        sawAtomic = sawAtomic || load->isAtomic();
                        atomicOnly = atomicOnly && load->isAtomic();
                        continue;
                    }

                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (llvm::isa<llvm::AllocaInst>(
                                store->getPointerOperand()->stripPointerCastsAndAliases()))
                            continue;

                        sawAtomic = sawAtomic || store->isAtomic();
                        atomicOnly = atomicOnly && store->isAtomic();
                        continue;
                    }

                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || llvm::isa<llvm::DbgInfoIntrinsic>(call))
                        continue;

                    const llvm::Function* callee = classifier.directCallee(*call);
                    if (callee == nullptr || callee->isDeclaration())
                    {
                        atomicOnly = false;
                        continue;
                    }

                    if (!accessesMemoryAtomicallyOnly(*callee, classifier, cache, visiting))
                        atomicOnly = false;
                    else
                        sawAtomic = true;
                }
            }

            visiting.erase(&function);
            const bool result = sawAtomic && atomicOnly;
            cache.emplace(&function, result);
            return result;
        }

        void appendAccess(std::vector<PendingAccess>& accesses, const llvm::Function& function,
                          const llvm::Instruction& instruction, const llvm::Value& pointerOperand,
                          AccessKind kind, AliasProvenance aliasProvenance,
                          const llvm::DataLayout& layout, std::uint64_t byteSize = 0,
                          bool isAtomic = false, bool coarseCallEffect = false)
        {
            const std::optional<RootBinding> root =
                resolveTrackedRoot(pointerOperand, &layout, byteSize);
            if (!root.has_value())
                return;

            PendingAccess access;
            access.function = &function;
            access.instruction = &instruction;
            access.root = *root;
            access.fact.functionId = functionId(function);
            access.fact.kind = kind;
            access.fact.aliasProvenance = aliasProvenance;
            access.fact.isAtomic = isAtomic;
            access.fact.coarseCallEffect = coarseCallEffect;
            access.fact.guessedIdentity = aliasProvenance != AliasProvenance::Direct;
            const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
            access.fact.loweredLocation = locations.loweredLocation;
            access.fact.userLocation = locations.userLocation;
            accesses.push_back(std::move(access));
        }

        /// Byte count of a memory intrinsic when it is a compile-time constant; zero otherwise,
        /// which keeps the region covering the whole object.
        std::uint64_t memoryIntrinsicLength(const llvm::MemIntrinsic& intrinsic)
        {
            const auto* length = llvm::dyn_cast<llvm::ConstantInt>(intrinsic.getLength());
            return length == nullptr ? 0 : length->getZExtValue();
        }

        void appendMemoryIntrinsicAccesses(std::vector<PendingAccess>& accesses,
                                           const llvm::Function& function,
                                           const llvm::MemIntrinsic& intrinsic,
                                           const llvm::DataLayout& layout)
        {
            const std::uint64_t length = memoryIntrinsicLength(intrinsic);
            if (const auto* transfer = llvm::dyn_cast<llvm::MemTransferInst>(&intrinsic))
            {
                appendAccess(accesses, function, intrinsic, *transfer->getRawDest(),
                             AccessKind::Write, AliasProvenance::Direct, layout, length);
                appendAccess(accesses, function, intrinsic, *transfer->getRawSource(),
                             AccessKind::Read, AliasProvenance::Direct, layout, length);
                return;
            }

            appendAccess(accesses, function, intrinsic, *intrinsic.getRawDest(), AccessKind::Write,
                         AliasProvenance::Direct, layout, length);
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
                                            const ConcurrencySymbolClassifier& classifier,
                                            const llvm::DataLayout& layout,
                                            AtomicOnlyCache& atomicOnlyCache)
        {
            if (!shouldInferCallMemoryEffects(call, classifier))
                return;

            const llvm::Function* callee = classifier.directCallee(call);
            std::unordered_set<const llvm::Function*> visiting;
            const bool atomicEffect =
                callee != nullptr &&
                accessesMemoryAtomicallyOnly(*callee, classifier, atomicOnlyCache, visiting);

            std::unordered_set<std::string> seenEffects;
            for (const llvm::Use& argument : call.args())
            {
                const llvm::Value* value = argument.get();
                if (value == nullptr || !value->getType()->isPointerTy())
                    continue;

                const std::optional<RootBinding> root = resolveTrackedRoot(*value, &layout, 0);
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

                appendAccess(accesses, function, call, *value, *kind, AliasProvenance::Direct,
                             layout, 0, atomicEffect, true);
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
        AtomicOnlyCache atomicOnlyCache;
        const llvm::DataLayout& layout = module.getDataLayout();

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
                    bool isAtomicAccess = false;
                    std::uint64_t byteSize = 0;

                    if (const auto* intrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(&instruction))
                    {
                        appendMemoryIntrinsicAccesses(accesses, function, *intrinsic, layout);
                        continue;
                    }

                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        appendCallMemoryEffectAccesses(accesses, function, *call, aaResults,
                                                       classifier, layout, atomicOnlyCache);
                        continue;
                    }

                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    {
                        pointerOperand = load->getPointerOperand();
                        kind = AccessKind::Read;
                        isAtomicAccess = load->isAtomic();
                        byteSize = accessByteSize(layout, load->getType());
                    }
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        pointerOperand = store->getPointerOperand();
                        kind = AccessKind::Write;
                        isAtomicAccess = store->isAtomic();
                        byteSize = accessByteSize(layout, store->getValueOperand()->getType());
                    }
                    else if (const auto* atomicRmw =
                                 llvm::dyn_cast<llvm::AtomicRMWInst>(&instruction))
                    {
                        pointerOperand = atomicRmw->getPointerOperand();
                        kind = AccessKind::Write;
                        isAtomicAccess = true;
                        byteSize = accessByteSize(layout, atomicRmw->getType());
                    }
                    else if (const auto* compareExchange =
                                 llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&instruction))
                    {
                        pointerOperand = compareExchange->getPointerOperand();
                        kind = AccessKind::Write;
                        isAtomicAccess = true;
                        byteSize =
                            accessByteSize(layout, compareExchange->getCompareOperand()->getType());
                    }
                    else
                    {
                        continue;
                    }

                    if (pointerOperand == nullptr)
                        continue;

                    std::optional<RootBinding> root =
                        resolveTrackedRoot(*pointerOperand, &layout, byteSize);
                    if (!root.has_value())
                    {
                        const std::optional<AliasResolvedGlobal> aliasResolvedGlobal =
                            resolveAliasGlobal(instruction, aaResults, trackedGlobals);
                        if (!aliasResolvedGlobal.has_value())
                            continue;

                        // The global identity is a guess here, not a resolution. Made inside a
                        // standard library header, about an object the user never named, it
                        // attributes the library's own bookkeeping to their data.
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
                    access.fact.isAtomic = isAtomicAccess;
                    access.fact.guessedIdentity = aliasProvenance != AliasProvenance::Direct;
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
