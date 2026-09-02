// SPDX-License-Identifier: Apache-2.0
#include "shared_object_binding_collector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <map>
#include <optional>
#include <set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        constexpr unsigned kPThreadEntryOperandIndex = 2;
        constexpr unsigned kPThreadArgumentOperandIndex = 3;
        constexpr unsigned kStdThreadCallableOperandIndex = 1;
        constexpr unsigned kStdThreadObjectOperandIndex = 2;

        struct SpawnedObject
        {
            std::string entryFunctionId;
            std::string objectId;
            unsigned argumentIndex = 0;
            bool insideLoop = false;
        };

        std::optional<SpawnedObject> spawnedObjectAt(const llvm::CallBase& call, CallKind kind,
                                                     bool insideLoop)
        {
            unsigned entryIndex = 0;
            unsigned objectIndex = 0;
            switch (kind)
            {
            case CallKind::PThreadCreate:
                entryIndex = kPThreadEntryOperandIndex;
                objectIndex = kPThreadArgumentOperandIndex;
                break;
            case CallKind::StdThreadCtor:
            case CallKind::StdJThreadCtor:
                entryIndex = kStdThreadCallableOperandIndex;
                objectIndex = kStdThreadObjectOperandIndex;
                break;
            default:
                return std::nullopt;
            }

            if (call.arg_size() <= objectIndex)
                return std::nullopt;

            const llvm::Function* entry = resolveFunctionValue(*call.getArgOperand(entryIndex));
            if (entry == nullptr || entry->isDeclaration() || entry->arg_empty())
                return std::nullopt;

            // A heap object has no name of its own, but the variable the pointer is read from
            // does, and every spawn reading the same variable receives the same object. That
            // slot is therefore used as the object's identity.
            //
            // It assumes the variable is not repointed between two spawns. Assuming otherwise
            // would leave heap-held state with no identity at all, which is the gap this closes.
            const llvm::Value* object =
                call.getArgOperand(objectIndex)->stripPointerCastsAndAliases();
            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(object))
                object = load->getPointerOperand();

            std::optional<std::string> objectId = canonicalStorageGroupId(*object);
            if (!objectId.has_value())
                return std::nullopt;

            return SpawnedObject{
                .entryFunctionId = functionId(*entry),
                .objectId = std::move(*objectId),
                // Both spawn forms deliver the object on the entry's first parameter: the `void*`
                // of a pthread routine, and the `this` of a member function.
                .argumentIndex = 0,
                .insideLoop = insideLoop,
            };
        }
    } // namespace

    SharedObjectBindingCollector::SharedObjectBindingCollector(
        const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    SharedObjectBindings SharedObjectBindingCollector::collect(const llvm::Module& module) const
    {
        // Keyed by the pair so that the same object handed to two different entries counts as
        // shared just as much as the same entry started twice on it.
        std::map<std::pair<std::string, std::string>, std::set<const llvm::Instruction*>> sites;
        std::map<std::pair<std::string, std::string>, bool> loopedSite;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            const llvm::DominatorTree dominatorTree(mutableFunction);
            const llvm::LoopInfo loopInfo(dominatorTree);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                const bool insideLoop = loopInfo.getLoopFor(&block) != nullptr;
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const std::optional<SpawnedObject> spawned =
                        spawnedObjectAt(*call, classifier_.classify(*call), insideLoop);
                    if (!spawned.has_value())
                        continue;

                    const auto key = std::pair{spawned->entryFunctionId, spawned->objectId};
                    sites[key].insert(&instruction);
                    loopedSite[key] = loopedSite[key] || spawned->insideLoop;
                }
            }
        }

        // Handing an object to a thread is itself the proof: from that point two flows of
        // control can reach it, the one that spawned and the one that was spawned. Requiring a
        // second spawn would miss the ordinary case, where the creator keeps using the object it
        // just handed over.
        //
        // Identity is all this grants. Whether two accesses through it actually race is decided
        // afterwards, by the same concurrency reasoning that judges a global.
        // An entry started on two different objects gets no identity at all. Its body is one
        // function, so every access in it would otherwise be attributed to whichever object was
        // seen first, and two threads working on their own copies would be reported as racing.
        std::map<std::string, std::set<std::string>> objectsByEntry;
        for (const auto& [key, instructions] : sites)
            objectsByEntry[key.first].insert(key.second);

        SharedObjectBindings bindings;
        for (const auto& [entryFunctionId, objects] : objectsByEntry)
        {
            if (objects.size() != 1)
                continue;

            bindings.emplace(entryFunctionId,
                             SharedObjectBinding{.objectId = *objects.begin(), .argumentIndex = 0});
        }

        return bindings;
    }
} // namespace ctrace::concurrency::internal::analysis
