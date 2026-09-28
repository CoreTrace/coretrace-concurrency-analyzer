// SPDX-License-Identifier: Apache-2.0
#include "shared_access_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"
#include "parameter_footprints.hpp"

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

#include <algorithm>
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
        /// What a pointer designates depends on the target layout and on which globals the
        /// program defines. The two questions travel together, so they travel as one value.
        struct MemoryScope
        {
            const llvm::DataLayout& layout;
            const ProgramDefinedGlobals* programDefined = nullptr;
            /// Local objects handed to threads: their owner's accesses name them by storage.
            const std::unordered_set<std::string>& localObjects;
        };

        /// The object a pointer leads into, when it is one the analysis follows: a global, or a
        /// local object its owner handed to a thread.
        std::optional<RootBinding> resolveObjectRoot(const llvm::Value& pointer,
                                                     std::uint64_t byteSize,
                                                     const MemoryScope& scope)
        {
            if (std::optional<RootBinding> root =
                    resolveTrackedRoot(pointer, &scope.layout, byteSize, scope.programDefined))
            {
                return root;
            }
            return resolveLocalObjectRoot(pointer, &scope.layout, byteSize, scope.localObjects);
        }

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

        /// Records an access to `root`, whose region is the bytes the access touches.
        void appendAccessTo(std::vector<PendingAccess>& accesses, const llvm::Function& function,
                            const llvm::Instruction& instruction, RootBinding root, AccessKind kind,
                            AliasProvenance aliasProvenance, bool isAtomic, bool coarseCallEffect)
        {
            PendingAccess access;
            access.function = &function;
            access.instruction = &instruction;
            access.root = std::move(root);
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

        void appendAccess(std::vector<PendingAccess>& accesses, const llvm::Function& function,
                          const llvm::Instruction& instruction, const llvm::Value& pointerOperand,
                          AccessKind kind, AliasProvenance aliasProvenance,
                          const MemoryScope& scope, std::uint64_t byteSize = 0)
        {
            std::optional<RootBinding> root = resolveObjectRoot(pointerOperand, byteSize, scope);
            if (!root.has_value())
                return;

            // An access of unknown extent may reach the whole object the pointer designates.
            if (byteSize == 0)
                root->region = root->designated;
            appendAccessTo(accesses, function, instruction, std::move(*root), kind, aliasProvenance,
                           false, false);
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
                                           const MemoryScope& scope)
        {
            const std::uint64_t length = memoryIntrinsicLength(intrinsic);
            if (const auto* transfer = llvm::dyn_cast<llvm::MemTransferInst>(&intrinsic))
            {
                appendAccess(accesses, function, intrinsic, *transfer->getRawDest(),
                             AccessKind::Write, AliasProvenance::Direct, scope, length);
                appendAccess(accesses, function, intrinsic, *transfer->getRawSource(),
                             AccessKind::Read, AliasProvenance::Direct, scope, length);
                return;
            }

            appendAccess(accesses, function, intrinsic, *intrinsic.getRawDest(), AccessKind::Write,
                         AliasProvenance::Direct, scope, length);
        }

        std::optional<AccessKind> accessKindFromModRef(llvm::ModRefInfo modRefInfo)
        {
            if (llvm::isModSet(modRefInfo))
                return AccessKind::Write;

            if (llvm::isRefSet(modRefInfo))
                return AccessKind::Read;

            return std::nullopt;
        }

        /// The effect a call may have on what each pointer argument designates.
        ///
        /// A callee defined here is read for what it does to that argument: the bytes it reads
        /// and writes, apart, and atomically or not. Most of that is its own accesses, which
        /// reach the call anyway, with the locks held around them; that part is marked as
        /// restating them. The rest is what no access shows: whatever it reaches through pointers
        /// it loads or once the argument escapes, and the thread handles it starts, joins or
        /// detaches. A callee without a body may do anything the declaration allows to the whole
        /// object the argument designates.
        void appendCallMemoryEffectAccesses(std::vector<PendingAccess>& accesses,
                                            const llvm::Function& function,
                                            const llvm::CallBase& call, llvm::AAResults& aaResults,
                                            const ConcurrencySymbolClassifier& classifier,
                                            const MemoryScope& scope,
                                            ParameterFootprints& footprints)
        {
            if (!shouldInferCallMemoryEffects(call, classifier))
                return;

            const llvm::Function* callee = classifier.directCallee(call);
            const bool calleeIsDefined = callee != nullptr && !callee->isDeclaration();

            std::unordered_set<std::string> seenEffects;
            auto appendEffect = [&](const RootBinding& root, const MemoryRegion& region,
                                    AccessKind kind, bool isAtomic, bool restates)
            {
                RootBinding effectRoot = root;
                effectRoot.region = region;
                const std::string key = callEffectKey(effectRoot, kind) + (isAtomic ? "|a" : "");
                if (!seenEffects.insert(key).second)
                    return;

                appendAccessTo(accesses, function, call, std::move(effectRoot), kind,
                               AliasProvenance::Direct, isAtomic, true);
                accesses.back().restatesCalleeAccesses = restates;
            };

            for (const llvm::Use& argument : call.args())
            {
                const llvm::Value* value = argument.get();
                if (value == nullptr || !value->getType()->isPointerTy())
                    continue;

                const std::optional<RootBinding> root = resolveObjectRoot(*value, 0, scope);
                if (!root.has_value())
                    continue;

                const llvm::MemoryLocation location = llvm::MemoryLocation::getBeforeOrAfter(value);
                const std::optional<AccessKind> allowed =
                    accessKindFromModRef(aaResults.getModRefInfo(&call, location));
                if (!allowed.has_value())
                    continue;

                const unsigned argumentNumber = call.getArgOperandNo(&argument);
                if (!calleeIsDefined || argumentNumber >= callee->arg_size())
                {
                    appendEffect(*root, root->designated, *allowed, false, false);
                    continue;
                }

                const ParameterFootprint footprint = footprints.of(*callee, argumentNumber);
                auto appendExtent = [&](const ParameterFootprint::Extent& extent, bool restates)
                {
                    for (const AccessKind kind : {AccessKind::Read, AccessKind::Write})
                    {
                        // Alias analysis may still prove the callee leaves the object unwritten.
                        if (kind == AccessKind::Write && *allowed == AccessKind::Read)
                            continue;

                        const bool wholeObject = kind == AccessKind::Write
                                                     ? extent.writesWholeObject
                                                     : extent.readsWholeObject;
                        if (wholeObject)
                        {
                            appendEffect(*root, root->designated, kind, false, restates);
                            continue;
                        }

                        for (const ParameterFootprint::Range& range :
                             kind == AccessKind::Write ? extent.writes : extent.reads)
                        {
                            const MemoryRegion calleeRegion{
                                .hasKnownOffset = true,
                                .byteOffset = range.begin,
                                .byteSize = static_cast<std::uint64_t>(range.end - range.begin),
                            };
                            appendEffect(*root,
                                         calleeRegion.rebasedOn(root->region, root->designated),
                                         kind, range.isAtomic, restates);
                        }
                    }
                };
                // What no access shows first, so a restatement never hides it.
                appendExtent(footprint.unshown, false);
                appendExtent(footprint.all, true);
            }
        }
    } // namespace

    SharedAccessCollector::SharedAccessCollector(const ConcurrencySymbolClassifier& classifier,
                                                 LlvmFunctionAnalysisProvider& analyses)
        : classifier_(classifier), analyses_(analyses)
    {
    }

    std::vector<PendingAccess>
    SharedAccessCollector::collect(const llvm::Module& module,
                                   const ProgramDefinedGlobals* programDefined,
                                   const std::unordered_set<std::string>* sharedObjectIds,
                                   const std::unordered_set<std::string>* localObjectIds) const
    {
        std::vector<PendingAccess> accesses;
        std::vector<const llvm::GlobalVariable*> trackedGlobals;
        trackedGlobals.reserve(module.global_size());
        for (const llvm::GlobalVariable& global : module.globals())
        {
            if (shouldTrackSharedGlobal(global, programDefined))
                trackedGlobals.push_back(&global);
        }

        const llvm::DataLayout& layout = module.getDataLayout();
        ParameterFootprints footprints(classifier_, layout);
        static const std::unordered_set<std::string> kNoSharedObjects;
        const std::unordered_set<std::string>& sharedObjects =
            sharedObjectIds != nullptr ? *sharedObjectIds : kNoSharedObjects;
        const MemoryScope scope{
            .layout = layout,
            .programDefined = programDefined,
            .localObjects = localObjectIds != nullptr ? *localObjectIds : kNoSharedObjects,
        };

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::AAResults& aaResults = analyses_.getAAResults(function);

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
                        appendMemoryIntrinsicAccesses(accesses, function, *intrinsic, scope);
                        continue;
                    }

                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        appendCallMemoryEffectAccesses(accesses, function, *call, aaResults,
                                                       classifier_, scope, footprints);
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
                        resolveObjectRoot(*pointerOperand, byteSize, scope);
                    if (!root.has_value())
                    {
                        root = resolveSharedObjectRoot(*pointerOperand, &layout, byteSize,
                                                       sharedObjects);
                    }

                    if (!root.has_value())
                    {
                        const std::optional<AliasResolvedGlobal> aliasResolvedGlobal =
                            resolveAliasGlobal(instruction, aaResults, trackedGlobals,
                                               programDefined);
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
