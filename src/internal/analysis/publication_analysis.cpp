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
            const llvm::Function* function = nullptr;
            llvm::APInt published;
            /// Where the publishing thread's accesses must stand before: the store itself when it
            /// releases, and any releasing fence that precedes it.
            std::vector<const llvm::Instruction*> releasePoints;
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

            PublicationOrderingMap run(const std::vector<const llvm::Instruction*>& accesses)
            {
                for (const llvm::Instruction* access : accesses)
                    accessesByFunction_[access->getFunction()].push_back(access);

                collectFlagOperations();

                PublicationOrderingMap ordering;
                for (const auto& [flag, operations] : flags_)
                {
                    const std::optional<Publication> publication =
                        findPublication(flag, operations);
                    if (!publication.has_value())
                        continue;

                    tagPublisher(*publication, ordering);
                    for (const FlagAccess& load : operations.loads)
                    {
                        if (load.function != publication->function)
                            tagConsumer(*publication, load, ordering);
                    }
                }
                return ordering;
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
                if (!published.has_value() || !initial.has_value() ||
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
                                        .function = store.function,
                                        .published = published->zextOrTrunc(kFoldWidth)};
                if (releasesAtLeast(*operation.order))
                    publication.releasePoints.push_back(store.instruction);

                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(*store.function);
                for (const Fence& fence : fencesFor(*store.function))
                {
                    if (releasesAtLeast(fence.order) &&
                        dominators.dominates(fence.instruction, store.instruction))
                    {
                        publication.releasePoints.push_back(fence.instruction);
                    }
                }

                if (publication.releasePoints.empty())
                    return std::nullopt;
                return publication;
            }

            void tagPublisher(const Publication& publication, PublicationOrderingMap& ordering)
            {
                const llvm::DominatorTree& dominators =
                    analyses_.getDominatorTree(*publication.function);
                for (const llvm::Instruction* access : accessesIn(*publication.function))
                {
                    for (const llvm::Instruction* releasePoint : publication.releasePoints)
                    {
                        if (dominators.dominates(access, releasePoint))
                        {
                            ordering[access].beforeRelease.insert(publication.flag);
                            break;
                        }
                    }
                }
            }

            void tagConsumer(const Publication& publication, const FlagAccess& load,
                             PublicationOrderingMap& ordering)
            {
                if (!load.operation.order.has_value())
                    return;

                const llvm::Function& function = *load.function;
                const BlockSet& live = liveByFunction_.at(&function);
                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                const bool acquires = acquiresAtLeast(*load.operation.order);

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
                    const llvm::BasicBlockEdge observed(&block, taken);
                    std::vector<const llvm::Instruction*> acquireFences;
                    for (const Fence& fence : fencesFor(function))
                    {
                        if (acquiresAtLeast(fence.order) &&
                            dominators.dominates(observed, fence.instruction->getParent()))
                        {
                            acquireFences.push_back(fence.instruction);
                        }
                    }

                    for (const llvm::Instruction* access : accessesIn(function))
                    {
                        const bool afterObservedAcquire =
                            acquires && dominators.dominates(observed, access->getParent());
                        const bool afterAcquireFence =
                            std::any_of(acquireFences.begin(), acquireFences.end(),
                                        [&](const llvm::Instruction* fence)
                                        { return dominators.dominates(fence, access); });
                        if (afterObservedAcquire || afterAcquireFence)
                            ordering[access].afterAcquire.insert(publication.flag);
                    }
                }
            }

            const std::vector<const llvm::Instruction*>&
            accessesIn(const llvm::Function& function) const
            {
                static const std::vector<const llvm::Instruction*> kNone;
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
            std::unordered_map<const llvm::Function*, std::vector<const llvm::Instruction*>>
                accessesByFunction_;
        };
    } // namespace

    PublicationOrderingMap collectPublicationOrdering(
        const llvm::Module& module, const std::vector<const llvm::Instruction*>& accesses,
        const std::vector<DirectCallSite>& directCallSites, const std::vector<SpawnFact>& spawns,
        LlvmFunctionAnalysisProvider& analyses, bool projectAnalysis)
    {
        return PublicationFinder(module, directCallSites, spawns, analyses, projectAnalysis)
            .run(accesses);
    }
} // namespace ctrace::concurrency::internal::analysis
