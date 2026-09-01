// SPDX-License-Identifier: Apache-2.0
#include "thread_lifecycle_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <unordered_set>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        struct LifecycleDescriptor
        {
            ThreadHandleKind handleKind = ThreadHandleKind::PThread;
            ThreadLifecycleAction action = ThreadLifecycleAction::Create;
            unsigned operandIndex = 0;
            std::optional<unsigned> sourceOperandIndex;
        };

        std::optional<LifecycleDescriptor> classifyLifecycle(CallKind kind)
        {
            switch (kind)
            {
            case CallKind::PThreadCreate:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::PThread,
                    .action = ThreadLifecycleAction::Create,
                    .operandIndex = 0,
                };
            case CallKind::PThreadJoin:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::PThread,
                    .action = ThreadLifecycleAction::Join,
                    .operandIndex = 0,
                };
            case CallKind::PThreadDetach:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::PThread,
                    .action = ThreadLifecycleAction::Detach,
                    .operandIndex = 0,
                };
            case CallKind::StdThreadCtor:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Create,
                    .operandIndex = 0,
                };
            case CallKind::StdThreadMove:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Move,
                    .operandIndex = 0,
                    .sourceOperandIndex = 1,
                };
            case CallKind::StdThreadJoin:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Join,
                    .operandIndex = 0,
                };
            case CallKind::StdThreadDetach:
                return LifecycleDescriptor{
                    .handleKind = ThreadHandleKind::StdThread,
                    .action = ThreadLifecycleAction::Detach,
                    .operandIndex = 0,
                };
            default:
                return std::nullopt;
            }
        }

        std::string lifecycleKey(const ThreadLifecycleFact& fact)
        {
            return std::to_string(static_cast<int>(fact.handleKind)) + "|" +
                   std::to_string(static_cast<int>(fact.action)) + "|" + fact.handleGroupId + "|" +
                   fact.sourceHandleGroupId.value_or("") + "|" + fact.functionId + "|" +
                   fact.location.file + "|" + std::to_string(fact.location.line) + "|" +
                   std::to_string(fact.location.column);
        }

        std::optional<std::filesystem::path> primarySourceRoot(const llvm::Module& module)
        {
            for (llvm::DICompileUnit* compileUnit : module.debug_compile_units())
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

        bool isRuntimeOwnedLifecycle(const ThreadLifecycleFact& fact,
                                     const SourceLocation& userLocation,
                                     const std::optional<std::filesystem::path>& sourceRoot)
        {
            if (!fact.handleGroupId.starts_with("arg:"))
                return false;

            return !isLikelyUserLocation(userLocation, sourceRoot);
        }

        struct ResolutionSite
        {
            const llvm::Instruction* instruction = nullptr;
            std::string handleGroupId;
        };

        /// True for calls that may take over a thread handle passed by pointer. Lifecycle
        /// operations and `~thread()` keep ownership where it is; anything else — a container
        /// insertion, a user helper storing the handle — may move it beyond reach.
        bool transfersHandleOwnership(const llvm::CallBase& call,
                                      const ConcurrencySymbolClassifier& classifier)
        {
            switch (classifier.classify(call))
            {
            case CallKind::PThreadCreate:
            case CallKind::PThreadJoin:
            case CallKind::PThreadDetach:
            case CallKind::StdThreadCtor:
            case CallKind::StdJThreadCtor:
            case CallKind::StdThreadJoin:
            case CallKind::StdThreadDetach:
            case CallKind::StdThreadDtor:
                return false;
            default:
                break;
            }

            return !llvm::isa<llvm::DbgInfoIntrinsic>(call) && !llvm::isa<llvm::MemIntrinsic>(call);
        }

        /// True when a join or detach of the same handle runs on every normal path out of the
        /// creation. Balanced create/join counts are not enough: a join under an `if` still leaves
        /// a path where the handle is abandoned.
        ///
        /// Post-dominance alone would answer "no" for every C++ handle, because the exceptional
        /// edge out of a constructor bypasses the join. Only paths reaching a normal return are
        /// considered, which is the control flow a missing join is about.
        bool isResolvedOnAllPaths(const llvm::Instruction& createInstruction,
                                  const std::string& handleGroupId,
                                  const std::vector<ResolutionSite>& resolutionSites,
                                  const std::vector<const llvm::BasicBlock*>& returnBlocks,
                                  const llvm::DominatorTree& dominatorTree,
                                  const llvm::LoopInfo& loopInfo)
        {
            const llvm::BasicBlock* createBlock = createInstruction.getParent();

            // The site covers the creation when no return can be reached from it while stepping
            // around the site. Plain dominance would reject two creations on exclusive branches,
            // each resolved by its own join.
            auto coversEveryReturn = [&](const llvm::Instruction& site)
            {
                const llvm::BasicBlock* siteBlock = site.getParent();
                if (siteBlock == createBlock)
                    return createInstruction.comesBefore(&site);

                llvm::SmallPtrSet<llvm::BasicBlock*, 4> detour{
                    const_cast<llvm::BasicBlock*>(siteBlock)};
                return std::none_of(returnBlocks.begin(), returnBlocks.end(),
                                    [&](const llvm::BasicBlock* returnBlock)
                                    {
                                        return returnBlock != siteBlock &&
                                               llvm::isPotentiallyReachable(
                                                   createBlock, returnBlock, &detour,
                                                   &dominatorTree, &loopInfo);
                                    });
            };

            return std::any_of(resolutionSites.begin(), resolutionSites.end(),
                               [&](const ResolutionSite& site)
                               {
                                   return site.handleGroupId == handleGroupId &&
                                          coversEveryReturn(*site.instruction);
                               });
        }

        /// A creation inside a loop yields one handle per iteration. Unless the resolution happens
        /// in the same loop, every iteration but the last leaks its handle.
        bool isCreatedInLoopWithoutInnerResolution(
            const llvm::Instruction& createInstruction, const std::string& handleGroupId,
            const std::vector<ResolutionSite>& resolutionSites, const llvm::LoopInfo& loopInfo)
        {
            const llvm::Loop* createLoop = loopInfo.getLoopFor(createInstruction.getParent());
            if (createLoop == nullptr)
                return false;

            return std::none_of(resolutionSites.begin(), resolutionSites.end(),
                                [&](const ResolutionSite& site)
                                {
                                    return site.handleGroupId == handleGroupId &&
                                           createLoop->contains(site.instruction->getParent());
                                });
        }

        /// Storage of `pthread_attr_t` objects configured with a non-default detach state. The
        /// enum values differ per platform, so the constant is resolved from the target triple
        /// rather than assumed.
        std::unordered_set<std::string>
        collectDetachedAttributeGroups(const llvm::Module& module,
                                       const ConcurrencySymbolClassifier& classifier)
        {
            constexpr unsigned kAttrOperandIndex = 0;
            constexpr unsigned kDetachStateOperandIndex = 1;
            constexpr std::int64_t kDarwinCreateDetached = 2;
            constexpr std::int64_t kPosixCreateDetached = 1;

            const bool isDarwin = llvm::StringRef(module.getTargetTriple()).contains("apple") ||
                                  llvm::StringRef(module.getTargetTriple()).contains("darwin");
            const std::int64_t detachedValue =
                isDarwin ? kDarwinCreateDetached : kPosixCreateDetached;

            std::unordered_set<std::string> detachedGroups;
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;

                for (const llvm::BasicBlock& block : function)
                {
                    for (const llvm::Instruction& instruction : block)
                    {
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        if (call == nullptr || call->arg_size() <= kDetachStateOperandIndex)
                        {
                            continue;
                        }

                        const llvm::Function* callee = classifier.directCallee(*call);
                        if (callee == nullptr ||
                            !callee->getName().contains("pthread_attr_setdetachstate"))
                        {
                            continue;
                        }

                        const auto* detachState = llvm::dyn_cast<llvm::ConstantInt>(
                            call->getArgOperand(kDetachStateOperandIndex));
                        if (detachState == nullptr || detachState->getSExtValue() != detachedValue)
                        {
                            continue;
                        }

                        if (const auto attrGroup =
                                canonicalStorageGroupId(*call->getArgOperand(kAttrOperandIndex));
                            attrGroup.has_value())
                        {
                            detachedGroups.insert(*attrGroup);
                        }
                    }
                }
            }

            return detachedGroups;
        }

        bool isDetachedAtCreation(const llvm::CallBase& call,
                                  const std::unordered_set<std::string>& detachedAttributeGroups)
        {
            constexpr unsigned kAttrOperandIndex = 1;
            if (detachedAttributeGroups.empty() || call.arg_size() <= kAttrOperandIndex)
                return false;

            const auto attrGroup = canonicalStorageGroupId(*call.getArgOperand(kAttrOperandIndex));
            return attrGroup.has_value() && detachedAttributeGroups.contains(*attrGroup);
        }
    } // namespace

    ThreadLifecycleCollector::ThreadLifecycleCollector(
        const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    std::vector<ThreadLifecycleFact>
    ThreadLifecycleCollector::collect(const llvm::Module& module) const
    {
        std::vector<ThreadLifecycleFact> facts;
        std::unordered_set<std::string> factKeys;
        const std::optional<std::filesystem::path> sourceRoot = primarySourceRoot(module);
        const std::unordered_set<std::string> detachedAttributeGroups =
            collectDetachedAttributeGroups(module, classifier_);

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            llvm::DominatorTree dominatorTree(mutableFunction);
            const llvm::LoopInfo loopInfo(dominatorTree);

            std::vector<const llvm::BasicBlock*> returnBlocks;
            for (const llvm::BasicBlock& block : function)
            {
                if (llvm::isa<llvm::ReturnInst>(block.getTerminator()))
                    returnBlocks.push_back(&block);
            }

            // Resolution sites are needed before creates can be classified, so the function is
            // walked once to index them and once to emit facts.
            std::vector<ResolutionSite> resolutionSites;
            std::unordered_set<std::string> escapedGroups;
            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const CallKind kind = classifier_.classify(*call);
                    const std::optional<LifecycleDescriptor> descriptor = classifyLifecycle(kind);
                    if (!descriptor.has_value() || descriptor->operandIndex >= call->arg_size())
                        continue;

                    const std::optional<std::string> handleGroupId =
                        canonicalStorageGroupId(*call->getArgOperand(descriptor->operandIndex));

                    if (descriptor->action == ThreadLifecycleAction::Move &&
                        descriptor->sourceOperandIndex.has_value() &&
                        *descriptor->sourceOperandIndex < call->arg_size())
                    {
                        // A move whose destination cannot be named puts the handle beyond reach:
                        // its join may well happen through that storage.
                        if (!handleGroupId.has_value())
                        {
                            if (const auto sourceGroup = canonicalStorageGroupId(
                                    *call->getArgOperand(*descriptor->sourceOperandIndex));
                                sourceGroup.has_value())
                            {
                                escapedGroups.insert(*sourceGroup);
                            }
                        }
                        continue;
                    }

                    if (!handleGroupId.has_value())
                        continue;

                    if (descriptor->action == ThreadLifecycleAction::Join ||
                        descriptor->action == ThreadLifecycleAction::Detach)
                    {
                        resolutionSites.push_back(ResolutionSite{
                            .instruction = &instruction,
                            .handleGroupId = *handleGroupId,
                        });
                    }
                }

                // A handle handed to anything other than a lifecycle operation may have its
                // ownership taken over — `v.push_back(std::move(t))` moves it into storage this
                // analysis cannot name, and the join then happens through that storage.
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || !transfersHandleOwnership(*call, classifier_))
                        continue;

                    for (const llvm::Use& argument : call->args())
                    {
                        const llvm::Value* value = argument.get();
                        if (value == nullptr || !value->getType()->isPointerTy())
                            continue;

                        if (const auto group = canonicalStorageGroupId(*value); group.has_value())
                            escapedGroups.insert(*group);
                    }
                }
            }

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const CallKind kind = classifier_.classify(*call);
                    const std::optional<LifecycleDescriptor> descriptor = classifyLifecycle(kind);
                    if (!descriptor.has_value() || descriptor->operandIndex >= call->arg_size())
                        continue;

                    const std::optional<std::string> handleGroupId =
                        canonicalStorageGroupId(*call->getArgOperand(descriptor->operandIndex));
                    if (!handleGroupId.has_value())
                        continue;

                    std::optional<std::string> sourceHandleGroupId;
                    if (descriptor->sourceOperandIndex.has_value())
                    {
                        if (*descriptor->sourceOperandIndex >= call->arg_size())
                            continue;

                        sourceHandleGroupId = canonicalStorageGroupId(
                            *call->getArgOperand(*descriptor->sourceOperandIndex));
                        if (!sourceHandleGroupId.has_value())
                            continue;
                    }

                    const ResolvedSourceLocations locations = resolveSourceLocations(instruction);
                    ThreadLifecycleFact fact{
                        .handleKind = descriptor->handleKind,
                        .action = descriptor->action,
                        .handleGroupId = *handleGroupId,
                        .sourceHandleGroupId = std::move(sourceHandleGroupId),
                        .functionId = functionId(function),
                        .location = locations.userLocation,
                    };

                    // A thread created detached needs no join at all, so the creation is resolved
                    // where it stands.
                    const bool createdDetached =
                        kind == CallKind::PThreadCreate &&
                        isDetachedAtCreation(*call, detachedAttributeGroups);

                    if (descriptor->action == ThreadLifecycleAction::Create)
                    {
                        fact.resolvedOnAllPaths =
                            createdDetached ||
                            isResolvedOnAllPaths(instruction, *handleGroupId, resolutionSites,
                                                 returnBlocks, dominatorTree, loopInfo);
                        fact.insideLoop = !createdDetached && isCreatedInLoopWithoutInnerResolution(
                                                                  instruction, *handleGroupId,
                                                                  resolutionSites, loopInfo);
                        fact.escapedToUntrackedStorage = escapedGroups.contains(*handleGroupId);
                    }

                    if (isRuntimeOwnedLifecycle(fact, locations.userLocation, sourceRoot))
                    {
                        continue;
                    }

                    const std::string key = lifecycleKey(fact);
                    if (!factKeys.insert(key).second)
                        continue;

                    facts.push_back(std::move(fact));

                    if (!createdDetached)
                        continue;

                    // `PTHREAD_CREATE_DETACHED` resolves the handle at creation, exactly like an
                    // explicit `pthread_detach`.
                    ThreadLifecycleFact detachFact = facts.back();
                    detachFact.action = ThreadLifecycleAction::Detach;
                    detachFact.resolvedOnAllPaths = true;
                    detachFact.insideLoop = false;
                    if (factKeys.insert(lifecycleKey(detachFact)).second)
                        facts.push_back(std::move(detachFact));
                }
            }
        }

        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
