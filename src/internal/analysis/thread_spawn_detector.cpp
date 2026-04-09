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

#include <sstream>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        struct ParameterizedSpawn
        {
            unsigned argumentIndex = 0;
            SourceLocation location;
            bool insideLoop = false;
        };

        struct DirectFunctionCallBinding
        {
            std::string callerFunctionId;
            std::string calleeFunctionId;
            std::unordered_map<unsigned, FunctionBinding> argumentBindings;
            SourceLocation location;
            bool insideLoop = false;
        };

        struct ThreadEntryBinding
        {
            const llvm::Function* function = nullptr;
            std::optional<unsigned> argumentIndex;
        };

        std::string parameterizedSpawnKey(const ParameterizedSpawn& spawn)
        {
            std::ostringstream stream;
            stream << spawn.argumentIndex << "|" << spawn.location.file << "|"
                   << spawn.location.line << "|" << spawn.location.column << "|"
                   << spawn.location.function << "|" << spawn.insideLoop;
            return stream.str();
        }

        std::string concreteSpawnKey(const llvm::Function& entry, const SourceLocation& location,
                                     bool insideLoop)
        {
            std::ostringstream stream;
            stream << functionId(entry) << "|" << location.file << "|" << location.line << "|"
                   << location.column << "|" << location.function << "|" << insideLoop;
            return stream.str();
        }

        bool addParameterizedSpawn(
            std::unordered_map<std::string, std::vector<ParameterizedSpawn>>& summariesByFunction,
            std::unordered_map<std::string, std::unordered_set<std::string>>& summaryKeysByFunction,
            const std::string& functionId, ParameterizedSpawn spawn)
        {
            const std::string key = parameterizedSpawnKey(spawn);
            if (!summaryKeysByFunction[functionId].insert(key).second)
                return false;

            summariesByFunction[functionId].push_back(std::move(spawn));
            return true;
        }

        bool addConcreteSpawn(ThreadSpawnCollection& collection,
                              std::unordered_set<std::string>& spawnKeys,
                              const llvm::Function& entry, const SourceLocation& location,
                              bool insideLoop)
        {
            if (entry.isDeclaration())
                return false;

            const std::string key = concreteSpawnKey(entry, location, insideLoop);
            if (!spawnKeys.insert(key).second)
                return false;

            SpawnFact fact;
            fact.entryFunctionId = functionId(entry);
            fact.location = location;
            fact.insideLoop = insideLoop;
            collection.spawns.push_back(fact);

            EntryConcurrencyInfo& concurrency = collection.entryConcurrency[fact.entryFunctionId];
            ++concurrency.staticSpawnCount;
            concurrency.hasSpawnInLoop = concurrency.hasSpawnInLoop || insideLoop;
            return true;
        }

        std::optional<ThreadEntryBinding> threadEntryBindingFromCall(const llvm::CallBase& call,
                                                                     CallKind kind)
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
                return std::nullopt;

            const std::optional<FunctionBinding> binding = resolveFunctionBinding(*entryValue);
            if (!binding.has_value())
                return std::nullopt;

            return ThreadEntryBinding{
                .function = binding->function,
                .argumentIndex = binding->argumentIndex,
            };
        }

        std::vector<DirectFunctionCallBinding>
        collectDirectCallBindings(const llvm::Module& module,
                                  const ConcurrencySymbolClassifier& classifier)
        {
            std::vector<DirectFunctionCallBinding> bindings;

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

                        const llvm::Function* callee = classifier.directCallee(*call);
                        if (callee == nullptr || callee->isDeclaration())
                            continue;

                        DirectFunctionCallBinding binding;
                        binding.callerFunctionId = functionId(function);
                        binding.calleeFunctionId = functionId(*callee);
                        binding.location = makeSourceLocation(instruction);
                        binding.insideLoop = loopInfo.getLoopFor(call->getParent()) != nullptr;

                        for (unsigned argumentIndex = 0; argumentIndex < call->arg_size();
                             ++argumentIndex)
                        {
                            const std::optional<FunctionBinding> argumentBinding =
                                resolveFunctionBinding(*call->getArgOperand(argumentIndex));
                            if (argumentBinding.has_value())
                                binding.argumentBindings.emplace(argumentIndex, *argumentBinding);
                        }

                        if (!binding.argumentBindings.empty())
                            bindings.push_back(std::move(binding));
                    }
                }
            }

            return bindings;
        }
    } // namespace

    ThreadSpawnDetector::ThreadSpawnDetector(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    ThreadSpawnCollection ThreadSpawnDetector::collect(const llvm::Module& module) const
    {
        ThreadSpawnCollection collection;
        std::unordered_set<std::string> concreteSpawnKeys;
        std::unordered_map<std::string, std::vector<ParameterizedSpawn>> summariesByFunction;
        std::unordered_map<std::string, std::unordered_set<std::string>> summaryKeysByFunction;

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

                    const bool insideLoop = loopInfo.getLoopFor(call->getParent()) != nullptr;
                    const SourceLocation location = makeSourceLocation(instruction);
                    const std::optional<ThreadEntryBinding> entryBinding =
                        threadEntryBindingFromCall(*call, kind);
                    if (!entryBinding.has_value())
                        continue;

                    if (entryBinding->function != nullptr)
                    {
                        addConcreteSpawn(collection, concreteSpawnKeys, *entryBinding->function,
                                         location, insideLoop);
                        continue;
                    }

                    if (!entryBinding->argumentIndex.has_value())
                        continue;

                    addParameterizedSpawn(summariesByFunction, summaryKeysByFunction,
                                          functionId(function),
                                          ParameterizedSpawn{
                                              .argumentIndex = *entryBinding->argumentIndex,
                                              .location = location,
                                              .insideLoop = insideLoop,
                                          });
                }
            }
        }

        const std::vector<DirectFunctionCallBinding> directCallBindings =
            collectDirectCallBindings(module, classifier_);

        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const DirectFunctionCallBinding& callBinding : directCallBindings)
            {
                const auto summaryIt = summariesByFunction.find(callBinding.calleeFunctionId);
                if (summaryIt == summariesByFunction.end())
                    continue;

                const std::vector<ParameterizedSpawn> calleeSummaries = summaryIt->second;
                for (const ParameterizedSpawn& spawn : calleeSummaries)
                {
                    const auto bindingIt = callBinding.argumentBindings.find(spawn.argumentIndex);
                    if (bindingIt == callBinding.argumentBindings.end())
                        continue;

                    const bool insideLoop = spawn.insideLoop || callBinding.insideLoop;
                    if (bindingIt->second.function != nullptr)
                    {
                        addConcreteSpawn(collection, concreteSpawnKeys, *bindingIt->second.function,
                                         callBinding.location, insideLoop);
                        continue;
                    }

                    if (!bindingIt->second.argumentIndex.has_value())
                        continue;

                    changed =
                        addParameterizedSpawn(summariesByFunction, summaryKeysByFunction,
                                              callBinding.callerFunctionId,
                                              ParameterizedSpawn{
                                                  .argumentIndex =
                                                      *bindingIt->second.argumentIndex,
                                                  .location = callBinding.location,
                                                  .insideLoop = insideLoop,
                                              }) ||
                        changed;
                }
            }
        }

        return collection;
    }
} // namespace ctrace::concurrency::internal::analysis
