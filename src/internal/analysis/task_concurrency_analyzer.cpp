// SPDX-License-Identifier: Apache-2.0
#include "task_concurrency_analyzer.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <deque>
#include <optional>
#include <string_view>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Operand carrying the thread handle, for every lifecycle call this analysis inspects.
        constexpr unsigned kHandleOperandIndex = 0;
        constexpr unsigned kPThreadEntryOperandIndex = 2;
        constexpr unsigned kStdThreadCallableOperandIndex = 1;

        constexpr std::string_view kProgramEntryFunction = "main";

        struct SpawnSite
        {
            const llvm::Instruction* instruction = nullptr;
            std::string handleGroupId;
            std::string entryFunctionId;
        };

        struct JoinSite
        {
            const llvm::Instruction* instruction = nullptr;
            std::string handleGroupId;
        };

        struct FunctionLifecycleSites
        {
            std::vector<SpawnSite> spawns;
            std::vector<JoinSite> joins;
        };

        bool isSpawnCall(CallKind kind)
        {
            return kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor ||
                   kind == CallKind::StdJThreadCtor;
        }

        bool isJoinCall(CallKind kind)
        {
            // A detached thread is never joined, so its lifetime stays open: only joins bound the
            // live range.
            return kind == CallKind::PThreadJoin || kind == CallKind::StdThreadJoin;
        }

        std::optional<unsigned> entryOperandIndex(CallKind kind)
        {
            if (kind == CallKind::PThreadCreate)
                return kPThreadEntryOperandIndex;

            if (kind == CallKind::StdThreadCtor || kind == CallKind::StdJThreadCtor)
                return kStdThreadCallableOperandIndex;

            return std::nullopt;
        }

        FunctionLifecycleSites collectLifecycleSites(const llvm::Function& function,
                                                     const ConcurrencySymbolClassifier& classifier,
                                                     bool includeExternalEntries)
        {
            FunctionLifecycleSites sites;

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_size() == 0)
                        continue;

                    const CallKind kind = classifier.classify(*call);
                    const std::optional<std::string> handleGroupId =
                        canonicalStorageGroupId(*call->getArgOperand(kHandleOperandIndex));
                    if (!handleGroupId.has_value())
                        continue;

                    if (isJoinCall(kind))
                    {
                        sites.joins.push_back(JoinSite{
                            .instruction = &instruction,
                            .handleGroupId = *handleGroupId,
                        });
                        continue;
                    }

                    if (!isSpawnCall(kind))
                        continue;

                    const std::optional<unsigned> entryIndex = entryOperandIndex(kind);
                    if (!entryIndex.has_value() || *entryIndex >= call->arg_size())
                        continue;

                    const llvm::Function* entry =
                        resolveFunctionValue(*call->getArgOperand(*entryIndex));
                    if (entry == nullptr || (entry->isDeclaration() && !includeExternalEntries))
                        continue;

                    sites.spawns.push_back(SpawnSite{
                        .instruction = &instruction,
                        .handleGroupId = *handleGroupId,
                        .entryFunctionId = functionId(*entry),
                    });
                }
            }

            return sites;
        }

        /// Entries whose instance created at `spawn` is still running at `point`: the spawn must
        /// dominate the point, and no join of the same handle may dominate it.
        bool spawnIsLiveAt(const SpawnSite& spawn, const llvm::Instruction& point,
                           const FunctionLifecycleSites& sites,
                           const llvm::DominatorTree& dominatorTree)
        {
            if (spawn.instruction == &point)
                return false;

            if (!dominatorTree.dominates(spawn.instruction, &point))
                return false;

            for (const JoinSite& join : sites.joins)
            {
                if (join.handleGroupId != spawn.handleGroupId)
                    continue;

                if (dominatorTree.dominates(join.instruction, &point))
                    return false;
            }

            return true;
        }

        std::unordered_set<std::string>
        collectRootTaskFunctions(const llvm::Module& module,
                                 const std::vector<DirectCallSite>& directCallSites)
        {
            std::unordered_map<std::string, std::vector<std::string>> calleesByFunction;
            for (const DirectCallSite& site : directCallSites)
                calleesByFunction[site.callerFunctionId].push_back(site.calleeFunctionId);

            std::unordered_set<std::string> rootTaskFunctions;
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration() ||
                    functionId(function) != std::string(kProgramEntryFunction))
                {
                    continue;
                }

                std::deque<std::string> queue{functionId(function)};
                rootTaskFunctions.insert(functionId(function));
                while (!queue.empty())
                {
                    const std::string current = std::move(queue.front());
                    queue.pop_front();

                    const auto calleesIt = calleesByFunction.find(current);
                    if (calleesIt == calleesByFunction.end())
                        continue;

                    for (const std::string& calleeId : calleesIt->second)
                    {
                        if (rootTaskFunctions.insert(calleeId).second)
                            queue.push_back(calleeId);
                    }
                }
            }

            return rootTaskFunctions;
        }
    } // namespace

    TaskConcurrencyAnalyzer::TaskConcurrencyAnalyzer(const ConcurrencySymbolClassifier& classifier)
        : classifier_(classifier)
    {
    }

    TaskConcurrencyResult
    TaskConcurrencyAnalyzer::analyze(const llvm::Module& module,
                                     const std::vector<DirectCallSite>& directCallSites,
                                     bool includeExternalEntries) const
    {
        TaskConcurrencyResult result;
        result.rootTaskFunctions = collectRootTaskFunctions(module, directCallSites);

        std::unordered_map<std::string, FunctionLifecycleSites> sitesByFunction;

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const std::string currentFunctionId = functionId(function);
            FunctionLifecycleSites sites =
                collectLifecycleSites(function, classifier_, includeExternalEntries);
            if (sites.spawns.empty())
            {
                sitesByFunction.emplace(currentFunctionId, std::move(sites));
                continue;
            }

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            const llvm::DominatorTree dominatorTree(mutableFunction);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                for (const llvm::Instruction& instruction : block)
                {
                    ThreadEntrySet liveEntries;
                    for (const SpawnSite& spawn : sites.spawns)
                    {
                        if (spawnIsLiveAt(spawn, instruction, sites, dominatorTree))
                            liveEntries.insert(spawn.entryFunctionId);
                    }

                    if (!liveEntries.empty())
                        result.liveEntriesAtInstruction.emplace(&instruction,
                                                                std::move(liveEntries));
                }
            }

            // Two instances of the same entry overlap when a spawn happens while a previous
            // instance is still live. Counting spawn sites instead would treat mutually exclusive
            // branches, or a spawn/join pair repeated sequentially, as concurrent.
            for (const SpawnSite& spawn : sites.spawns)
            {
                const auto liveIt = result.liveEntriesAtInstruction.find(spawn.instruction);
                if (liveIt != result.liveEntriesAtInstruction.end() &&
                    liveIt->second.contains(spawn.entryFunctionId))
                {
                    result.overlappingSpawnEntries.insert(spawn.entryFunctionId);
                }
            }

            // A spawn dominated by the join of another entry's only instance can never overlap it.
            for (const SpawnSite& spawn : sites.spawns)
            {
                for (const SpawnSite& earlier : sites.spawns)
                {
                    if (earlier.entryFunctionId == spawn.entryFunctionId)
                        continue;

                    const bool earlierIsJoinedFirst =
                        !spawnIsLiveAt(earlier, *spawn.instruction, sites, dominatorTree) &&
                        dominatorTree.dominates(earlier.instruction, spawn.instruction);
                    if (earlierIsJoinedFirst)
                    {
                        result.sequencedEntryPairs.insert(
                            makeEntryPair(earlier.entryFunctionId, spawn.entryFunctionId));
                    }
                }
            }

            sitesByFunction.emplace(currentFunctionId, std::move(sites));
        }

        // A function that starts a thread and returns without waiting for it hands that thread
        // to its caller, still running. One path out is enough: the caller cannot assume the
        // join happened on the branch it did not take.
        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const auto sitesIt = sitesByFunction.find(functionId(function));
            if (sitesIt == sitesByFunction.end() || sitesIt->second.spawns.empty())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            const llvm::DominatorTree dominatorTree(mutableFunction);

            for (const SpawnSite& spawn : sitesIt->second.spawns)
            {
                bool stillRunningOnSomeReturn = true;
                for (const llvm::BasicBlock& block : function)
                {
                    const llvm::Instruction* terminator = block.getTerminator();
                    if (!llvm::isa<llvm::ReturnInst>(terminator) ||
                        !dominatorTree.isReachableFromEntry(&block))
                    {
                        continue;
                    }

                    stillRunningOnSomeReturn = false;
                    if (spawnIsLiveAt(spawn, *terminator, sitesIt->second, dominatorTree))
                    {
                        stillRunningOnSomeReturn = true;
                        break;
                    }
                }

                // A function with no reachable return never gives the thread back, so the
                // question does not arise; leaving the entry recorded is the safe answer.
                if (stillRunningOnSomeReturn)
                {
                    result.entriesLeftRunningByFunction[functionId(function)].insert(
                        spawn.entryFunctionId);
                }
            }
        }

        // The property travels up the call graph: a complete constructor that calls the base one
        // hands the same running thread to whoever called it. A function that joins something is
        // left out — it may well be the one waiting, and claiming otherwise would report a race
        // against a thread that is already finished.
        bool leakedChanged = true;
        while (leakedChanged)
        {
            leakedChanged = false;

            for (const DirectCallSite& site : directCallSites)
            {
                const auto calleeIt =
                    result.entriesLeftRunningByFunction.find(site.calleeFunctionId);
                if (calleeIt == result.entriesLeftRunningByFunction.end())
                    continue;

                const auto callerSitesIt = sitesByFunction.find(site.callerFunctionId);
                if (callerSitesIt != sitesByFunction.end() && !callerSitesIt->second.joins.empty())
                    continue;

                ThreadEntrySet& callerEntries =
                    result.entriesLeftRunningByFunction[site.callerFunctionId];
                for (const std::string& entry : calleeIt->second)
                    leakedChanged = callerEntries.insert(entry).second || leakedChanged;
            }
        }

        // A call to such a function is a spawn as far as its caller is concerned: from there on,
        // the thread runs beside everything the call dominates.
        //
        // Nothing turns it off again. A caller that joins later keeps the entry live to its own
        // return, which costs precision after the join and never soundness — and for the shape
        // this exists for, an object owning its thread, the join is the destructor and the
        // object is gone by then anyway.
        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            llvm::Function& mutableFunction = const_cast<llvm::Function&>(function);
            const llvm::DominatorTree dominatorTree(mutableFunction);

            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;

                    const llvm::Function* callee = classifier_.directCallee(*call);
                    if (callee == nullptr)
                        continue;

                    const auto leakedIt =
                        result.entriesLeftRunningByFunction.find(functionId(*callee));
                    if (leakedIt == result.entriesLeftRunningByFunction.end())
                        continue;

                    for (const llvm::BasicBlock& reachedBlock : function)
                    {
                        for (const llvm::Instruction& reached : reachedBlock)
                        {
                            if (&reached == &instruction ||
                                !dominatorTree.dominates(&instruction, &reached))
                            {
                                continue;
                            }

                            ThreadEntrySet& live = result.liveEntriesAtInstruction[&reached];
                            live.insert(leakedIt->second.begin(), leakedIt->second.end());
                        }
                    }
                }
            }
        }

        // Propagate the live set across direct calls so that a helper invoked by the initial thread
        // inherits the tasks running at its call sites.
        std::unordered_map<std::string, ThreadEntrySet> liveEntriesAtFunctionEntry;
        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const DirectCallSite& site : directCallSites)
            {
                if (site.call == nullptr)
                    continue;

                ThreadEntrySet inherited = liveEntriesAtFunctionEntry[site.callerFunctionId];
                if (const auto liveIt = result.liveEntriesAtInstruction.find(site.call);
                    liveIt != result.liveEntriesAtInstruction.end())
                {
                    inherited.insert(liveIt->second.begin(), liveIt->second.end());
                }

                ThreadEntrySet& calleeEntries = liveEntriesAtFunctionEntry[site.calleeFunctionId];
                for (const std::string& entry : inherited)
                    changed = calleeEntries.insert(entry).second || changed;
            }
        }

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const auto inheritedIt = liveEntriesAtFunctionEntry.find(functionId(function));
            if (inheritedIt == liveEntriesAtFunctionEntry.end() || inheritedIt->second.empty())
                continue;

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& instruction : block)
                {
                    ThreadEntrySet& liveEntries = result.liveEntriesAtInstruction[&instruction];
                    liveEntries.insert(inheritedIt->second.begin(), inheritedIt->second.end());
                }
            }
        }

        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
