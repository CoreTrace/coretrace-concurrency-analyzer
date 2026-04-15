// SPDX-License-Identifier: Apache-2.0
#include "tu_facts_builder.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "lock_order_collector.hpp"
#include "lock_scope_tracker.hpp"
#include "lock_state_propagator.hpp"
#include "shared_access_collector.hpp"
#include "thread_lifecycle_collector.hpp"
#include "thread_spawn_detector.hpp"
#include "thread_context_propagator.hpp"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <cctype>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        struct ParameterizedAccess
        {
            RootBinding root;
            AccessFact fact;
        };

        struct DirectCallBinding
        {
            const llvm::CallBase* call = nullptr;
            std::string callerFunctionId;
            std::string calleeFunctionId;
            std::unordered_map<unsigned, RootBinding> argumentBindings;
            SourceLocation callsiteLocation;
            std::set<std::string> callsiteHeldLocks;
        };

        struct LifecycleArgumentBinding
        {
            unsigned argumentIndex = 0;
            std::string suffix;
        };

        std::string rootBindingKey(const RootBinding& binding)
        {
            if (binding.kind == RootBindingKind::Global)
                return "global:" + binding.symbol;

            return "argument:" + std::to_string(binding.argumentIndex);
        }

        std::string accessFactKey(const AccessFact& fact)
        {
            std::ostringstream stream;
            stream << fact.symbol << "|" << fact.functionId << "|" << toString(fact.kind) << "|"
                   << fact.loweredLocation.file << "|" << fact.loweredLocation.line << "|"
                   << fact.loweredLocation.column << "|" << fact.loweredLocation.function;

            for (const std::string& lock : fact.heldLocks)
                stream << "|lock:" << lock;

            return stream.str();
        }

        std::string parameterizedAccessKey(const ParameterizedAccess& access)
        {
            return rootBindingKey(access.root) + "|" + accessFactKey(access.fact);
        }

        std::string lifecycleFactKey(const ThreadLifecycleFact& fact)
        {
            std::ostringstream stream;
            stream << static_cast<int>(fact.handleKind) << "|" << static_cast<int>(fact.action)
                   << "|" << fact.handleGroupId << "|" << fact.functionId << "|"
                   << fact.location.file << "|" << fact.location.line << "|" << fact.location.column
                   << "|" << fact.location.function;
            return stream.str();
        }

        bool sameSourceLocation(const SourceLocation& lhs, const SourceLocation& rhs)
        {
            return std::tie(lhs.file, lhs.line, lhs.column, lhs.function) ==
                   std::tie(rhs.file, rhs.line, rhs.column, rhs.function);
        }

        bool hasDistinctUserLocation(const AccessFact& access)
        {
            return !sameSourceLocation(access.userLocation, access.loweredLocation);
        }

        bool addConcreteAccess(std::vector<AccessFact>& accesses,
                               std::unordered_set<std::string>& accessKeys, AccessFact fact)
        {
            const std::string key = accessFactKey(fact);
            if (!accessKeys.insert(key).second)
                return false;

            accesses.push_back(std::move(fact));
            return true;
        }

        std::string projectedAccessPreferenceKey(const AccessFact& fact)
        {
            std::ostringstream stream;
            stream << fact.symbol << "|" << toString(fact.kind) << "|" << fact.loweredLocation.file
                   << "|" << fact.loweredLocation.line << "|" << fact.loweredLocation.column << "|"
                   << fact.loweredLocation.function;

            for (const std::string& lock : fact.heldLocks)
                stream << "|lock:" << lock;

            return stream.str();
        }

        std::vector<AccessFact> filterProjectedConcreteAccesses(std::vector<AccessFact> accesses)
        {
            std::unordered_set<std::string> projectedKeys;
            for (const AccessFact& access : accesses)
            {
                if (hasDistinctUserLocation(access))
                    projectedKeys.insert(projectedAccessPreferenceKey(access));
            }

            std::vector<AccessFact> filtered;
            filtered.reserve(accesses.size());
            for (AccessFact& access : accesses)
            {
                const bool hasProjectedVariant =
                    projectedKeys.contains(projectedAccessPreferenceKey(access));
                if (hasProjectedVariant && !hasDistinctUserLocation(access))
                    continue;

                filtered.push_back(std::move(access));
            }

            return filtered;
        }

        bool shouldRemapAccessToCallsite(const AccessFact& access, const SourceLocation& callsite)
        {
            if (callsite.file.empty() && callsite.line == 0)
                return false;

            if (access.userLocation.file.empty())
                return true;

            return access.userLocation.file != callsite.file;
        }

        bool shouldProjectConcreteAccessToCallsite(const AccessFact& access,
                                                   const SourceLocation& callsite)
        {
            if (!access.allowCallsiteProjection)
                return false;

            if (callsite.file.empty() && callsite.line == 0)
                return false;

            if (access.userLocation.file.empty())
                return true;

            if (access.userLocation.file != callsite.file)
                return true;

            return !hasDistinctUserLocation(access) &&
                   !sameSourceLocation(access.userLocation, callsite);
        }

        bool addParameterizedAccess(
            std::unordered_map<std::string, std::vector<ParameterizedAccess>>& summariesByFunction,
            std::unordered_map<std::string, std::unordered_set<std::string>>& summaryKeysByFunction,
            const std::string& functionId, ParameterizedAccess access)
        {
            const std::string key = parameterizedAccessKey(access);
            if (!summaryKeysByFunction[functionId].insert(key).second)
                return false;

            summariesByFunction[functionId].push_back(std::move(access));
            return true;
        }

        bool addLifecycleFact(
            std::vector<ThreadLifecycleFact>& facts, std::unordered_set<std::string>& factKeys,
            std::unordered_map<std::string, std::vector<ThreadLifecycleFact>>& factsByFunction,
            ThreadLifecycleFact fact)
        {
            const std::string key = lifecycleFactKey(fact);
            if (!factKeys.insert(key).second)
                return false;

            factsByFunction[fact.functionId].push_back(fact);
            facts.push_back(std::move(fact));
            return true;
        }

        std::optional<LifecycleArgumentBinding>
        parseLifecycleArgumentBinding(const ThreadLifecycleFact& fact)
        {
            const std::string prefix = "arg:" + fact.functionId + ":";
            if (!fact.handleGroupId.starts_with(prefix))
                return std::nullopt;

            const std::size_t indexBegin = prefix.size();
            std::size_t indexEnd = indexBegin;
            while (indexEnd < fact.handleGroupId.size() &&
                   std::isdigit(static_cast<unsigned char>(fact.handleGroupId[indexEnd])))
            {
                ++indexEnd;
            }

            if (indexEnd == indexBegin)
                return std::nullopt;

            LifecycleArgumentBinding binding;
            binding.argumentIndex = static_cast<unsigned>(
                std::stoul(fact.handleGroupId.substr(indexBegin, indexEnd - indexBegin)));
            binding.suffix = fact.handleGroupId.substr(indexEnd);
            return binding;
        }

        bool shouldRemapLifecycleLocation(const SourceLocation& location,
                                          const SourceLocation& callsite)
        {
            if (callsite.file.empty() && callsite.line == 0)
                return false;

            if (location.file.empty())
                return true;

            return location.file != callsite.file;
        }

        std::set<std::string> mergeHeldLocks(const std::set<std::string>& lhs,
                                             const std::set<std::string>& rhs)
        {
            std::set<std::string> merged = lhs;
            merged.insert(rhs.begin(), rhs.end());
            return merged;
        }

        std::vector<DirectCallBinding> buildDirectCallBindings(
            const std::vector<DirectCallSite>& sites,
            const std::unordered_map<const llvm::CallBase*, std::set<std::string>>& heldLocksByCall)
        {
            std::vector<DirectCallBinding> bindings;

            for (const DirectCallSite& site : sites)
            {
                if (site.call == nullptr)
                    continue;

                DirectCallBinding binding;
                binding.call = site.call;
                binding.callerFunctionId = site.callerFunctionId;
                binding.calleeFunctionId = site.calleeFunctionId;
                binding.callsiteLocation = site.userLocation;
                if (const auto heldLocksIt = heldLocksByCall.find(site.call);
                    heldLocksIt != heldLocksByCall.end())
                {
                    binding.callsiteHeldLocks = heldLocksIt->second;
                }

                for (unsigned argumentIndex = 0; argumentIndex < site.call->arg_size();
                     ++argumentIndex)
                {
                    const std::optional<RootBinding> root =
                        resolveTrackedRoot(*site.call->getArgOperand(argumentIndex));
                    if (root.has_value())
                        binding.argumentBindings.emplace(argumentIndex, *root);
                }

                if (!binding.argumentBindings.empty())
                    bindings.push_back(std::move(binding));
            }

            return bindings;
        }
    } // namespace

    TUFacts TUFactsBuilder::build(const llvm::Module& module) const
    {
        const ConcurrencySymbolClassifier classifier;

        ThreadSpawnDetector spawnDetector(classifier);
        ThreadSpawnCollection spawnFacts = spawnDetector.collect(module);
        ThreadLifecycleCollector threadLifecycleCollector(classifier);
        const std::vector<DirectCallSite> directCallSites =
            collectDirectCallSites(module, classifier);

        SharedAccessCollector accessCollector;
        std::vector<PendingAccess> pendingAccesses = accessCollector.collect(module);

        std::unordered_map<std::string, std::unordered_set<const llvm::Instruction*>>
            trackedAccessesByFunction;
        std::unordered_map<std::string, const llvm::Function*> functionsById;
        for (const PendingAccess& pendingAccess : pendingAccesses)
        {
            trackedAccessesByFunction[pendingAccess.fact.functionId].insert(
                pendingAccess.instruction);
            functionsById[pendingAccess.fact.functionId] = pendingAccess.function;
        }

        LockScopeTracker lockScopeTracker(classifier);
        std::unordered_map<const llvm::Instruction*, std::set<std::string>> heldLocksByAccess;
        for (const auto& [functionKey, trackedAccesses] : trackedAccessesByFunction)
        {
            const llvm::Function* function = functionsById[functionKey];
            if (function == nullptr)
                continue;

            std::unordered_map<const llvm::Instruction*, std::set<std::string>> functionLocks =
                lockScopeTracker.collectHeldLocks(*function, trackedAccesses);
            heldLocksByAccess.insert(functionLocks.begin(), functionLocks.end());
        }

        ThreadContextPropagator threadContextPropagator(classifier);

        TUFacts facts;
        facts.spawns = std::move(spawnFacts.spawns);
        facts.entryConcurrency = std::move(spawnFacts.entryConcurrency);
        facts.reachableThreadEntriesByFunction =
            threadContextPropagator.collect(module, facts.entryConcurrency);

        std::unordered_map<std::string, std::vector<ThreadLifecycleFact>> lifecycleFactsByFunction;
        std::unordered_set<std::string> lifecycleFactKeys;
        for (ThreadLifecycleFact fact : threadLifecycleCollector.collect(module))
        {
            addLifecycleFact(facts.threadLifecycles, lifecycleFactKeys, lifecycleFactsByFunction,
                             std::move(fact));
        }

        bool lifecycleChanged = true;
        while (lifecycleChanged)
        {
            lifecycleChanged = false;

            for (const DirectCallSite& callSite : directCallSites)
            {
                if (callSite.call == nullptr)
                    continue;

                const auto calleeFactsIt = lifecycleFactsByFunction.find(callSite.calleeFunctionId);
                if (calleeFactsIt == lifecycleFactsByFunction.end())
                    continue;

                const std::vector<ThreadLifecycleFact> calleeFacts = calleeFactsIt->second;
                for (const ThreadLifecycleFact& fact : calleeFacts)
                {
                    const auto binding = parseLifecycleArgumentBinding(fact);
                    if (!binding.has_value() || binding->argumentIndex >= callSite.call->arg_size())
                        continue;

                    const auto callerGroup = canonicalStorageGroupId(
                        *callSite.call->getArgOperand(binding->argumentIndex));
                    if (!callerGroup.has_value())
                        continue;

                    ThreadLifecycleFact propagated = fact;
                    propagated.functionId = callSite.callerFunctionId;
                    propagated.handleGroupId = *callerGroup + binding->suffix;
                    if (shouldRemapLifecycleLocation(propagated.location, callSite.userLocation))
                        propagated.location = callSite.userLocation;

                    lifecycleChanged =
                        addLifecycleFact(facts.threadLifecycles, lifecycleFactKeys,
                                         lifecycleFactsByFunction, std::move(propagated)) ||
                        lifecycleChanged;
                }
            }
        }

        std::vector<AccessFact> concreteAccesses;
        std::unordered_set<std::string> concreteAccessKeys;
        std::unordered_map<std::string, std::vector<ParameterizedAccess>> summariesByFunction;
        std::unordered_map<std::string, std::unordered_set<std::string>> summaryKeysByFunction;

        LockStatePropagator lockStatePropagator(classifier);
        const LockPropagationResult lockPropagation =
            lockStatePropagator.collect(module, directCallSites);

        LockOrderCollector lockOrderCollector(classifier);
        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            std::set<std::string> functionEntryLocks;
            if (const auto entryLocksIt =
                    lockPropagation.entryLocksByFunction.find(functionId(function));
                entryLocksIt != lockPropagation.entryLocksByFunction.end())
            {
                functionEntryLocks = entryLocksIt->second;
            }

            std::vector<LockOrderFact> functionLockOrders =
                lockOrderCollector.collect(function, functionEntryLocks);
            facts.lockOrders.insert(facts.lockOrders.end(), functionLockOrders.begin(),
                                    functionLockOrders.end());
        }

        for (PendingAccess& pendingAccess : pendingAccesses)
        {
            const auto heldLocksIt = heldLocksByAccess.find(pendingAccess.instruction);
            if (heldLocksIt != heldLocksByAccess.end())
                pendingAccess.fact.heldLocks = heldLocksIt->second;

            if (const auto entryLocksIt =
                    lockPropagation.entryLocksByFunction.find(pendingAccess.fact.functionId);
                entryLocksIt != lockPropagation.entryLocksByFunction.end())
            {
                pendingAccess.fact.heldLocks =
                    mergeHeldLocks(pendingAccess.fact.heldLocks, entryLocksIt->second);
            }

            if (pendingAccess.root.kind == RootBindingKind::Global)
            {
                pendingAccess.fact.symbol = pendingAccess.root.symbol;
                addConcreteAccess(concreteAccesses, concreteAccessKeys,
                                  std::move(pendingAccess.fact));
                continue;
            }

            const std::string functionKey = pendingAccess.fact.functionId;
            pendingAccess.fact.allowCallsiteProjection = true;
            addParameterizedAccess(summariesByFunction, summaryKeysByFunction, functionKey,
                                   ParameterizedAccess{
                                       .root = pendingAccess.root,
                                       .fact = std::move(pendingAccess.fact),
                                   });
        }

        const std::vector<DirectCallBinding> directCallBindings =
            buildDirectCallBindings(directCallSites, lockPropagation.effectiveHeldLocksByCall);

        bool changed = true;
        while (changed)
        {
            changed = false;

            for (const DirectCallBinding& callBinding : directCallBindings)
            {
                const auto summaryIt = summariesByFunction.find(callBinding.calleeFunctionId);
                if (summaryIt == summariesByFunction.end())
                    continue;

                const std::vector<ParameterizedAccess> calleeSummary = summaryIt->second;
                for (const ParameterizedAccess& access : calleeSummary)
                {
                    if (access.root.kind != RootBindingKind::Argument)
                        continue;

                    const auto bindingIt =
                        callBinding.argumentBindings.find(access.root.argumentIndex);
                    if (bindingIt == callBinding.argumentBindings.end())
                        continue;

                    if (bindingIt->second.kind == RootBindingKind::Global)
                    {
                        AccessFact concrete = access.fact;
                        concrete.functionId = callBinding.callerFunctionId;
                        concrete.symbol = bindingIt->second.symbol;
                        concrete.heldLocks =
                            mergeHeldLocks(concrete.heldLocks, callBinding.callsiteHeldLocks);
                        concrete.allowCallsiteProjection = true;
                        if (shouldRemapAccessToCallsite(concrete, callBinding.callsiteLocation))
                        {
                            concrete.userLocation = callBinding.callsiteLocation;
                            concrete.allowCallsiteProjection = false;
                        }
                        addConcreteAccess(concreteAccesses, concreteAccessKeys,
                                          std::move(concrete));
                        continue;
                    }

                    ParameterizedAccess propagatedAccess{
                        .root = bindingIt->second,
                        .fact = access.fact,
                    };
                    propagatedAccess.fact.functionId = callBinding.callerFunctionId;
                    propagatedAccess.fact.heldLocks = mergeHeldLocks(
                        propagatedAccess.fact.heldLocks, callBinding.callsiteHeldLocks);
                    if (shouldRemapAccessToCallsite(propagatedAccess.fact,
                                                    callBinding.callsiteLocation))
                    {
                        propagatedAccess.fact.userLocation = callBinding.callsiteLocation;
                    }

                    changed = addParameterizedAccess(summariesByFunction, summaryKeysByFunction,
                                                     callBinding.callerFunctionId,
                                                     std::move(propagatedAccess)) ||
                              changed;
                }
            }
        }

        changed = true;
        while (changed)
        {
            changed = false;

            const std::vector<AccessFact> currentConcreteAccesses = concreteAccesses;
            for (const DirectCallBinding& callBinding : directCallBindings)
            {
                for (const AccessFact& access : currentConcreteAccesses)
                {
                    if (access.functionId != callBinding.calleeFunctionId)
                        continue;

                    if (!shouldProjectConcreteAccessToCallsite(access,
                                                               callBinding.callsiteLocation))
                        continue;

                    AccessFact remapped = access;
                    remapped.functionId = callBinding.callerFunctionId;
                    remapped.heldLocks =
                        mergeHeldLocks(remapped.heldLocks, callBinding.callsiteHeldLocks);
                    remapped.userLocation = callBinding.callsiteLocation;
                    if (remapped.userLocation.file != remapped.loweredLocation.file)
                        remapped.allowCallsiteProjection = false;
                    changed = addConcreteAccess(concreteAccesses, concreteAccessKeys,
                                                std::move(remapped)) ||
                              changed;
                }
            }
        }

        facts.accesses = filterProjectedConcreteAccesses(std::move(concreteAccesses));
        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
