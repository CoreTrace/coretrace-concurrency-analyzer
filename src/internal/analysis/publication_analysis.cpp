// SPDX-License-Identifier: Apache-2.0
#include "publication_analysis.hpp"

#include "atomic_operations.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <map>
#include <optional>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        using BlockSet = llvm::SmallPtrSet<const llvm::BasicBlock*, 32>;

        constexpr unsigned kMaxFoldDepth = 16;
        constexpr unsigned kFoldWidth = 64;

        /// Evaluates a value to a constant through what an unoptimized build spills to the stack:
        /// a local slot stored exactly once forwards that store, and casts, comparisons and simple
        /// arithmetic fold. One value may be substituted by a constant, which is how a branch is
        /// evaluated as if a load had returned a given value.
        class ConstantFolder
        {
          public:
            /// `live` limits the stores that count to the blocks that can execute; null counts
            /// every store.
            explicit ConstantFolder(const BlockSet* live) : live_(live) {}

            void substitute(const llvm::Value& value, llvm::APInt constant)
            {
                substituted_ = &value;
                substitution_ = std::move(constant);
            }

            [[nodiscard]] bool usedSubstitution() const
            {
                return usedSubstitution_;
            }

            std::optional<llvm::APInt> fold(const llvm::Value& value, unsigned depth = 0)
            {
                if (depth > kMaxFoldDepth)
                    return std::nullopt;

                if (&value == substituted_)
                {
                    usedSubstitution_ = true;
                    return substitution_.zextOrTrunc(widthOf(value));
                }

                if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(&value))
                    return constant->getValue();

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&value))
                {
                    if (load->isAtomic())
                        return std::nullopt;

                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        load->getPointerOperand()->stripPointerCasts());
                    const llvm::StoreInst* store = slot != nullptr ? uniqueStore(*slot) : nullptr;
                    if (store == nullptr)
                        return std::nullopt;

                    std::optional<llvm::APInt> stored = fold(*store->getValueOperand(), depth + 1);
                    if (!stored.has_value())
                        return std::nullopt;
                    return stored->zextOrTrunc(widthOf(value));
                }

                if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(&value))
                {
                    std::optional<llvm::APInt> operand = fold(*cast->getOperand(0), depth + 1);
                    if (!operand.has_value())
                        return std::nullopt;

                    const unsigned width = widthOf(value);
                    switch (cast->getOpcode())
                    {
                    case llvm::Instruction::ZExt:
                    case llvm::Instruction::Trunc:
                        return operand->zextOrTrunc(width);
                    case llvm::Instruction::SExt:
                        return operand->sextOrTrunc(width);
                    default:
                        return std::nullopt;
                    }
                }

                if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&value))
                {
                    std::optional<llvm::APInt> lhs = fold(*binary->getOperand(0), depth + 1);
                    std::optional<llvm::APInt> rhs = fold(*binary->getOperand(1), depth + 1);
                    if (!lhs.has_value() || !rhs.has_value() ||
                        lhs->getBitWidth() != rhs->getBitWidth())
                    {
                        return std::nullopt;
                    }

                    switch (binary->getOpcode())
                    {
                    case llvm::Instruction::And:
                        return *lhs & *rhs;
                    case llvm::Instruction::Or:
                        return *lhs | *rhs;
                    case llvm::Instruction::Xor:
                        return *lhs ^ *rhs;
                    case llvm::Instruction::Add:
                        return *lhs + *rhs;
                    case llvm::Instruction::Sub:
                        return *lhs - *rhs;
                    default:
                        return std::nullopt;
                    }
                }

                if (const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(&value))
                {
                    std::optional<llvm::APInt> lhs = fold(*compare->getOperand(0), depth + 1);
                    std::optional<llvm::APInt> rhs = fold(*compare->getOperand(1), depth + 1);
                    if (!lhs.has_value() || !rhs.has_value() ||
                        lhs->getBitWidth() != rhs->getBitWidth())
                    {
                        return std::nullopt;
                    }
                    return llvm::APInt(
                        1, llvm::ICmpInst::compare(*lhs, *rhs, compare->getPredicate()));
                }

                return std::nullopt;
            }

          private:
            static unsigned widthOf(const llvm::Value& value)
            {
                const llvm::Type* type = value.getType();
                return type->isIntegerTy() ? type->getIntegerBitWidth() : kFoldWidth;
            }

            /// The one store to a local slot that can execute, provided the slot is only ever
            /// loaded and stored; a slot whose address is taken may change behind the fold.
            const llvm::StoreInst* uniqueStore(const llvm::AllocaInst& slot) const
            {
                const llvm::StoreInst* found = nullptr;
                for (const llvm::User* user : slot.users())
                {
                    if (llvm::isa<llvm::LoadInst>(user) || llvm::isa<llvm::DbgInfoIntrinsic>(user))
                        continue;

                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (store == nullptr || store->getPointerOperand() != &slot)
                        return nullptr;
                    if (live_ != nullptr && !live_->contains(store->getParent()))
                        continue;
                    if (found != nullptr)
                        return nullptr;
                    found = store;
                }
                return found;
            }

            const BlockSet* live_ = nullptr;
            const llvm::Value* substituted_ = nullptr;
            llvm::APInt substitution_;
            bool usedSubstitution_ = false;
        };

        /// The successor a branch or switch takes, when its condition folds.
        const llvm::BasicBlock* takenSuccessor(const llvm::Instruction& terminator,
                                               ConstantFolder& folder)
        {
            if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(&terminator))
            {
                if (!branch->isConditional())
                    return nullptr;

                const std::optional<llvm::APInt> condition = folder.fold(*branch->getCondition());
                if (!condition.has_value())
                    return nullptr;
                return branch->getSuccessor(condition->isZero() ? 1 : 0);
            }

            if (const auto* choice = llvm::dyn_cast<llvm::SwitchInst>(&terminator))
            {
                const std::optional<llvm::APInt> condition = folder.fold(*choice->getCondition());
                if (!condition.has_value())
                    return nullptr;

                for (const auto& caseHandle : choice->cases())
                {
                    if (caseHandle.getCaseValue()->getValue() == *condition)
                        return caseHandle.getCaseSuccessor();
                }
                return choice->getDefaultDest();
            }

            return nullptr;
        }

        /// The blocks that can execute once every switch on a constant is resolved. A standard
        /// library may inline an atomic operation as a switch over the memory order with one
        /// atomic instruction per order; only the arm the constant selects ever runs.
        BlockSet liveBlocks(const llvm::Function& function)
        {
            BlockSet live;
            std::vector<const llvm::BasicBlock*> pending{&function.getEntryBlock()};
            ConstantFolder folder(nullptr);
            while (!pending.empty())
            {
                const llvm::BasicBlock* block = pending.back();
                pending.pop_back();
                if (!live.insert(block).second)
                    continue;

                const llvm::Instruction* terminator = block->getTerminator();
                if (terminator == nullptr)
                    continue;

                if (llvm::isa<llvm::SwitchInst>(terminator))
                {
                    if (const llvm::BasicBlock* taken = takenSuccessor(*terminator, folder))
                    {
                        pending.push_back(taken);
                        continue;
                    }
                }
                for (const llvm::BasicBlock* successor : llvm::successors(block))
                    pending.push_back(successor);
            }
            return live;
        }

        /// True when every path through live blocks that reaches `later` passes `earlier` first.
        /// An inlined atomic operation leaves its one live instruction in an arm of a switch on
        /// the memory order, which dominates nothing once the dead arms are counted as paths.
        bool liveDominates(const llvm::Instruction& earlier, const llvm::Instruction& later,
                           const BlockSet& live)
        {
            const llvm::BasicBlock* earlierBlock = earlier.getParent();
            const llvm::BasicBlock* laterBlock = later.getParent();
            if (earlierBlock == laterBlock)
                return &earlier == &later || earlier.comesBefore(&later);
            if (!live.contains(earlierBlock))
                return false;

            BlockSet visited;
            std::vector<const llvm::BasicBlock*> pending{&earlier.getFunction()->getEntryBlock()};
            while (!pending.empty())
            {
                const llvm::BasicBlock* block = pending.back();
                pending.pop_back();
                if (block == earlierBlock || !live.contains(block) || !visited.insert(block).second)
                {
                    continue;
                }
                if (block == laterBlock)
                    return false;
                for (const llvm::BasicBlock* successor : llvm::successors(block))
                    pending.push_back(successor);
            }
            return true;
        }

        /// The value a global holds before any store, read from the start of its initializer.
        std::optional<llvm::APInt> initialValue(const llvm::GlobalVariable& global)
        {
            if (!global.hasDefinitiveInitializer())
                return std::nullopt;

            const llvm::Constant* current = global.getInitializer();
            while (current != nullptr)
            {
                if (current->isNullValue())
                    return llvm::APInt(kFoldWidth, 0);
                if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(current))
                    return constant->getValue().zextOrTrunc(kFoldWidth);
                current = current->getAggregateElement(0U);
            }
            return std::nullopt;
        }

        struct FlagAccess
        {
            const llvm::Instruction* instruction = nullptr;
            const llvm::Function* function = nullptr;
            AtomicOperation operation;
        };

        struct FlagOperations
        {
            const llvm::GlobalVariable* global = nullptr;
            std::vector<FlagAccess> stores;
            std::vector<FlagAccess> loads;
        };

        struct Fence
        {
            const llvm::Instruction* instruction = nullptr;
            MemoryOrder order = MemoryOrder::Relaxed;
        };

        struct Publication
        {
            std::string flag;
            /// The flag named the way accesses name their symbols.
            std::string flagSymbol;
            const FlagAccess* store = nullptr;
            llvm::APInt published;
            /// Where the publishing thread's accesses must stand before to be ordered: the store
            /// itself when it releases, and any releasing fence that precedes it.
            std::vector<const llvm::Instruction*> releasePoints;
        };

        /// An access the analysis follows, with the global it touches when it touches one.
        struct TrackedAccess
        {
            const llvm::Instruction* instruction = nullptr;
            std::string symbol;
        };

        /// An access that takes part in a publication, and whether the flag's ordering covers it.
        struct PublicationAccess
        {
            const TrackedAccess* access = nullptr;
            bool ordered = false;
        };

        class PublicationFinder
        {
          public:
            PublicationFinder(const llvm::Module& module,
                              const std::vector<DirectCallSite>& directCallSites,
                              const std::vector<SpawnFact>& spawns,
                              LlvmFunctionAnalysisProvider& analyses, bool projectAnalysis)
                : module_(module), directCallSites_(directCallSites), spawns_(spawns),
                  analyses_(analyses), projectAnalysis_(projectAnalysis)
            {
            }

            PublicationAnalysis run(const std::vector<PendingAccess>& accesses)
            {
                for (const PendingAccess& access : accesses)
                {
                    accessesByFunction_[access.function].push_back(TrackedAccess{
                        .instruction = access.instruction,
                        .symbol = access.root.kind == RootBindingKind::Global ? access.root.symbol
                                                                              : std::string(),
                    });
                }

                collectFlagOperations();

                PublicationAnalysis analysis;
                for (const auto& [flag, operations] : flags_)
                {
                    const std::optional<Publication> publication =
                        findPublication(flag, operations);
                    if (!publication.has_value())
                        continue;

                    const std::vector<PublicationAccess> published =
                        publishedAccesses(*publication);
                    for (const PublicationAccess& access : published)
                    {
                        if (access.ordered)
                            analysis.ordering[access.access->instruction].beforeRelease.insert(
                                flag);
                    }

                    for (const FlagAccess& load : operations.loads)
                    {
                        if (load.function == publication->store->function)
                            continue;

                        const std::vector<PublicationAccess> observed =
                            observingAccesses(*publication, load);
                        for (const PublicationAccess& access : observed)
                        {
                            if (access.ordered)
                                analysis.ordering[access.access->instruction].afterAcquire.insert(
                                    flag);
                        }

                        if (std::optional<WeakPublicationFact> weak =
                                weakPublication(*publication, load, published, observed))
                        {
                            analysis.weakPublications.push_back(std::move(*weak));
                        }
                    }
                }
                return analysis;
            }

          private:
            std::optional<std::string> flagKey(const llvm::Value& object) const
            {
                const llvm::GlobalVariable* global = resolveBaseGlobal(object);
                if (global == nullptr || (projectAnalysis_ && !global->hasLocalLinkage()))
                    return std::nullopt;

                std::optional<std::string> key = canonicalGlobalId(*global);
                // A flag private to this unit must not meet a namesake from another one.
                if (key.has_value() && global->hasLocalLinkage())
                    key = module_.getModuleIdentifier() + "#" + *key;
                return key;
            }

            void collectFlagOperations()
            {
                for (const llvm::Function& function : module_)
                {
                    if (function.isDeclaration())
                        continue;

                    const BlockSet& live =
                        liveByFunction_.emplace(&function, liveBlocks(function)).first->second;
                    for (const llvm::BasicBlock& block : function)
                    {
                        if (!live.contains(&block))
                            continue;
                        for (const llvm::Instruction& instruction : block)
                            recordInstruction(function, instruction);
                    }
                }
            }

            void recordInstruction(const llvm::Function& function,
                                   const llvm::Instruction& instruction)
            {
                const std::optional<AtomicOperation> operation =
                    recognizer_.operationOf(instruction);
                if (!operation.has_value())
                {
                    // A plain store to a flag means the flag is written behind the protocol.
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (const std::optional<std::string> flag =
                                flagKey(*store->getPointerOperand()))
                        {
                            plainlyStored_.insert(*flag);
                        }
                    }
                    return;
                }

                if (operation->kind == AtomicOperationKind::Fence)
                {
                    if (operation->order.has_value())
                        fencesByFunction_[&function].push_back(
                            Fence{.instruction = &instruction, .order = *operation->order});
                    return;
                }

                const std::optional<std::string> flag = flagKey(*operation->object);
                if (!flag.has_value())
                    return;

                FlagOperations& operations = flags_[*flag];
                operations.global = resolveBaseGlobal(*operation->object);
                const FlagAccess access{
                    .instruction = &instruction, .function = &function, .operation = *operation};
                if (operation->kind == AtomicOperationKind::Load)
                    operations.loads.push_back(access);
                else
                    operations.stores.push_back(access);
            }

            /// True when the function runs at most once in the whole program: a thread entry that
            /// nothing calls directly, started exactly once from `main`, outside any loop.
            bool runsAtMostOnce(const llvm::Function& function) const
            {
                const std::string id = functionId(function);
                for (const DirectCallSite& callSite : directCallSites_)
                {
                    if (callSite.calleeFunctionId == id)
                        return false;
                }

                std::size_t starts = 0;
                for (const SpawnFact& spawn : spawns_)
                {
                    if (spawn.entryFunctionId != id)
                        continue;
                    if (spawn.insideLoop || spawn.location.function != "main")
                        return false;
                    ++starts;
                }
                return starts == 1;
            }

            /// The flag's publication, whatever its ordering, when observing the published
            /// constant proves the publishing store was the one read.
            std::optional<Publication> findPublication(const std::string& flag,
                                                       const FlagOperations& operations)
            {
                if (operations.stores.size() != 1 || plainlyStored_.contains(flag))
                    return std::nullopt;

                const FlagAccess& store = operations.stores.front();
                const AtomicOperation& operation = store.operation;
                if (operation.kind != AtomicOperationKind::Store || !operation.order.has_value() ||
                    operation.storedValue == nullptr || operations.global == nullptr)
                {
                    return std::nullopt;
                }

                ConstantFolder folder(&liveByFunction_.at(store.function));
                const std::optional<llvm::APInt> published = folder.fold(*operation.storedValue);
                const std::optional<llvm::APInt> initial = initialValue(*operations.global);
                const std::optional<std::string> flagSymbol = canonicalGlobalId(*operations.global);
                if (!published.has_value() || !initial.has_value() || !flagSymbol.has_value() ||
                    published->zextOrTrunc(kFoldWidth) == *initial)
                {
                    return std::nullopt;
                }

                const llvm::LoopInfo& loops = analyses_.getLoopInfo(*store.function);
                if (loops.getLoopFor(store.instruction->getParent()) != nullptr ||
                    !runsAtMostOnce(*store.function))
                {
                    return std::nullopt;
                }

                Publication publication{.flag = flag,
                                        .flagSymbol = *flagSymbol,
                                        .store = &store,
                                        .published = published->zextOrTrunc(kFoldWidth)};
                if (releasesAtLeast(*operation.order))
                    publication.releasePoints.push_back(store.instruction);

                const BlockSet& live = liveByFunction_.at(store.function);
                for (const Fence& fence : fencesFor(*store.function))
                {
                    if (releasesAtLeast(fence.order) &&
                        liveDominates(*fence.instruction, *store.instruction, live))
                    {
                        publication.releasePoints.push_back(fence.instruction);
                    }
                }
                return publication;
            }

            /// The publishing thread's accesses made before the store, each ordered when it stands
            /// before a release point.
            std::vector<PublicationAccess> publishedAccesses(const Publication& publication)
            {
                const llvm::Function& function = *publication.store->function;
                const BlockSet& live = liveByFunction_.at(&function);
                std::vector<PublicationAccess> published;
                for (const TrackedAccess& access : accessesIn(function))
                {
                    if (!liveDominates(*access.instruction, *publication.store->instruction, live))
                        continue;

                    const bool ordered = std::any_of(
                        publication.releasePoints.begin(), publication.releasePoints.end(),
                        [&](const llvm::Instruction* releasePoint)
                        { return liveDominates(*access.instruction, *releasePoint, live); });
                    published.push_back(PublicationAccess{.access = &access, .ordered = ordered});
                }
                return published;
            }

            /// The reader's accesses on the branch taken once `load` returned the published
            /// constant, each ordered when the load acquires or an acquire fence precedes it.
            std::vector<PublicationAccess> observingAccesses(const Publication& publication,
                                                             const FlagAccess& load)
            {
                // An order that cannot be read orders nothing and is not known to be too weak.
                if (!load.operation.order.has_value())
                    return {};

                const llvm::Function& function = *load.function;
                const BlockSet& live = liveByFunction_.at(&function);
                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                const bool acquires = acquiresAtLeast(*load.operation.order);

                std::map<const TrackedAccess*, bool> observed;
                for (const llvm::BasicBlock& block : function)
                {
                    if (!live.contains(&block))
                        continue;

                    ConstantFolder folder(&live);
                    folder.substitute(*load.instruction, publication.published);
                    const llvm::BasicBlock* taken = takenSuccessor(*block.getTerminator(), folder);
                    if (taken == nullptr || !folder.usedSubstitution())
                        continue;

                    // The branch arm taken when the load returned the published constant.
                    const llvm::BasicBlockEdge observedArm(&block, taken);
                    std::vector<const llvm::Instruction*> acquireFences;
                    for (const Fence& fence : fencesFor(function))
                    {
                        if (acquiresAtLeast(fence.order) &&
                            dominators.dominates(observedArm, fence.instruction->getParent()))
                        {
                            acquireFences.push_back(fence.instruction);
                        }
                    }

                    for (const TrackedAccess& access : accessesIn(function))
                    {
                        if (!dominators.dominates(observedArm, access.instruction->getParent()))
                            continue;

                        const bool afterAcquireFence = std::any_of(
                            acquireFences.begin(), acquireFences.end(),
                            [&](const llvm::Instruction* fence)
                            { return liveDominates(*fence, *access.instruction, live); });
                        bool& ordered = observed.try_emplace(&access, false).first->second;
                        ordered = ordered || acquires || afterAcquireFence;
                    }
                }

                std::vector<PublicationAccess> accesses;
                for (const auto& [access, ordered] : observed)
                    accesses.push_back(PublicationAccess{.access = access, .ordered = ordered});
                return accesses;
            }

            /// The data both sides of a publication touch, when the flag's ordering fails to
            /// cover it on the publishing side, the reading side, or both.
            std::optional<WeakPublicationFact>
            weakPublication(const Publication& publication, const FlagAccess& load,
                            const std::vector<PublicationAccess>& published,
                            const std::vector<PublicationAccess>& observed) const
            {
                // Per data symbol: whether every access of that side is ordered.
                auto orderingBySymbol = [&](const std::vector<PublicationAccess>& accesses)
                {
                    std::map<std::string, bool> ordering;
                    for (const PublicationAccess& access : accesses)
                    {
                        const std::string& symbol = access.access->symbol;
                        if (symbol.empty() || symbol == publication.flagSymbol)
                            continue;
                        const auto [it, inserted] = ordering.try_emplace(symbol, access.ordered);
                        if (!inserted)
                            it->second = it->second && access.ordered;
                    }
                    return ordering;
                };

                const std::map<std::string, bool> publishing = orderingBySymbol(published);
                const std::map<std::string, bool> reading = orderingBySymbol(observed);

                WeakPublicationFact fact{
                    .flag = publication.flagSymbol, .storeReleases = true, .loadAcquires = true};
                for (const auto& [symbol, readOrdered] : reading)
                {
                    const auto publishedIt = publishing.find(symbol);
                    if (publishedIt == publishing.end())
                        continue;

                    fact.data.push_back(symbol);
                    fact.storeReleases = fact.storeReleases && publishedIt->second;
                    fact.loadAcquires = fact.loadAcquires && readOrdered;
                }

                if (fact.data.empty() || (fact.storeReleases && fact.loadAcquires))
                    return std::nullopt;

                fact.storeLocation =
                    resolveSourceLocations(*publication.store->instruction).userLocation;
                fact.loadLocation = resolveSourceLocations(*load.instruction).userLocation;
                fact.publisherFunctionId = functionId(*publication.store->function);
                fact.readerFunctionId = functionId(*load.function);
                return fact;
            }

            const std::vector<TrackedAccess>& accessesIn(const llvm::Function& function) const
            {
                static const std::vector<TrackedAccess> kNone;
                const auto it = accessesByFunction_.find(&function);
                return it != accessesByFunction_.end() ? it->second : kNone;
            }

            const std::vector<Fence>& fencesFor(const llvm::Function& function) const
            {
                static const std::vector<Fence> kNone;
                const auto it = fencesByFunction_.find(&function);
                return it != fencesByFunction_.end() ? it->second : kNone;
            }

            const llvm::Module& module_;
            const std::vector<DirectCallSite>& directCallSites_;
            const std::vector<SpawnFact>& spawns_;
            LlvmFunctionAnalysisProvider& analyses_;
            const bool projectAnalysis_;

            AtomicOperationRecognizer recognizer_;
            std::map<std::string, FlagOperations> flags_;
            std::set<std::string> plainlyStored_;
            std::unordered_map<const llvm::Function*, BlockSet> liveByFunction_;
            std::unordered_map<const llvm::Function*, std::vector<Fence>> fencesByFunction_;
            std::unordered_map<const llvm::Function*, std::vector<TrackedAccess>>
                accessesByFunction_;
        };
    } // namespace

    PublicationAnalysis analyzePublications(const llvm::Module& module,
                                            const std::vector<PendingAccess>& accesses,
                                            const std::vector<DirectCallSite>& directCallSites,
                                            const std::vector<SpawnFact>& spawns,
                                            LlvmFunctionAnalysisProvider& analyses,
                                            bool projectAnalysis)
    {
        return PublicationFinder(module, directCallSites, spawns, analyses, projectAnalysis)
            .run(accesses);
    }
} // namespace ctrace::concurrency::internal::analysis
