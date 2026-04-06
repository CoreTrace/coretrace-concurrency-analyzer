// SPDX-License-Identifier: Apache-2.0
#include "thread_spawn_detector.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        const llvm::Function* threadEntryFromCall(const llvm::CallBase& call, CallKind kind)
        {
            const llvm::Value* entryValue = nullptr;
            switch (kind)
            {
            case CallKind::PThreadCreate:
                if (call.arg_size() > 2)
                    entryValue = call.getArgOperand(2);
                break;
            case CallKind::StdThreadCtor:
                if (call.arg_size() > 1)
                    entryValue = call.getArgOperand(1);
                break;
            default:
                break;
            }

            if (entryValue == nullptr)
                return nullptr;

            entryValue = entryValue->stripPointerCasts();
            return llvm::dyn_cast<llvm::Function>(entryValue);
        }
    } // namespace

    ThreadSpawnDetector::ThreadSpawnDetector(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    ThreadSpawnCollection ThreadSpawnDetector::collect(const llvm::Module& module) const
    {
        ThreadSpawnCollection collection;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            llvm::DominatorTree dominatorTree(mutableFunction);
            llvm::LoopInfo loopInfo(dominatorTree);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const CallKind kind = classifier_.classify(*call);
                    if (kind != CallKind::PThreadCreate && kind != CallKind::StdThreadCtor)
                        continue;

                    const llvm::Function* entry = threadEntryFromCall(*call, kind);
                    if (entry == nullptr || entry->isDeclaration())
                        continue;

                    const bool insideLoop = loopInfo.getLoopFor(call->getParent()) != nullptr;
                    SpawnFact fact;
                    fact.entryFunctionId = functionId(*entry);
                    fact.location = makeSourceLocation(instruction);
                    fact.insideLoop = insideLoop;
                    collection.spawns.push_back(fact);

                    EntryConcurrencyInfo& concurrency =
                        collection.entryConcurrency[fact.entryFunctionId];
                    ++concurrency.staticSpawnCount;
                    concurrency.hasSpawnInLoop = concurrency.hasSpawnInLoop || insideLoop;
                }
            }
        }

        return collection;
    }
} // namespace ctrace::concurrency::internal::analysis
