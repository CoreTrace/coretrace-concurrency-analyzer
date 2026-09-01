// SPDX-License-Identifier: Apache-2.0
#include "tu_facts_builder.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "lock_order_collector.hpp"
#include "lock_scope_tracker.hpp"
#include "lock_state_propagator.hpp"
#include "shared_access_collector.hpp"
#include "cross_tu/program_symbol_index.hpp"
#include "task_concurrency_analyzer.hpp"
#include "thread_lifecycle_collector.hpp"
#include "thread_spawn_detector.hpp"
#include "thread_context_propagator.hpp"

#include "synchronization_effects.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
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
            bool callerInRootTask = false;
            ThreadEntrySet callsiteLiveEntries;
        };

        ThreadEntrySet mergeLiveEntries(const ThreadEntrySet& lhs, const ThreadEntrySet& rhs)
        {
            ThreadEntrySet merged = lhs;
            merged.insert(rhs.begin(), rhs.end());
            return merged;
        }

        struct LifecycleArgumentBinding
        {
            unsigned argumentIndex = 0;
            std::string suffix;
        };

        std::string rootBindingKey(const RootBinding& binding)
        {
            if (binding.kind == RootBindingKind::Global)
                return "global:" + binding.symbol + binding.region.suffix();

            return "argument:" + std::to_string(binding.argumentIndex) + binding.region.suffix();
        }

        std::string accessFactKey(const AccessFact& fact)
        {
            std::ostringstream stream;
            stream << fact.symbol << fact.region.suffix() << "|" << fact.functionId << "|"
                   << toString(fact.kind) << "|" << (fact.isAtomic ? "atomic" : "plain") << "|"
                   << toString(fact.aliasProvenance) << "|" << fact.loweredLocation.file << "|"
                   << fact.loweredLocation.line << "|" << fact.loweredLocation.column << "|"
                   << fact.loweredLocation.function;

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
                   << fact.sourceHandleGroupId.value_or("") << "|" << fact.location.file << "|"
                   << fact.location.line << "|" << fact.location.column << "|"
                   << fact.location.function;
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
            stream << fact.symbol << fact.region.suffix() << "|" << toString(fact.kind) << "|"
                   << fact.loweredLocation.file << "|" << toString(fact.aliasProvenance) << "|"
                   << fact.loweredLocation.line << "|" << fact.loweredLocation.column << "|"
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
        parseLifecycleArgumentBinding(const std::string& functionId,
                                      const std::string& handleGroupId)
        {
            const std::string prefix = "arg:" + functionId + ":";
            if (!handleGroupId.starts_with(prefix))
                return std::nullopt;

            const std::size_t indexBegin = prefix.size();
            std::size_t indexEnd = indexBegin;
            while (indexEnd < handleGroupId.size() &&
                   std::isdigit(static_cast<unsigned char>(handleGroupId[indexEnd])))
            {
                ++indexEnd;
            }

            if (indexEnd == indexBegin)
                return std::nullopt;

            LifecycleArgumentBinding binding;
            binding.argumentIndex = static_cast<unsigned>(
                std::stoul(handleGroupId.substr(indexBegin, indexEnd - indexBegin)));
            binding.suffix = handleGroupId.substr(indexEnd);
            return binding;
        }

        std::optional<LifecycleArgumentBinding>
        parseLifecycleArgumentBinding(const ThreadLifecycleFact& fact)
        {
            return parseLifecycleArgumentBinding(fact.functionId, fact.handleGroupId);
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

        /// Locks that may legally be reacquired by their owner: `std::recursive_mutex` and friends,
        /// recognized by type, plus any `pthread_mutex_t` initialized with a non-default type
        /// attribute. `PTHREAD_MUTEX_NORMAL` is zero on every supported platform, so a non-zero
        /// setting selects either recursive or error-checking behaviour, neither of which
        /// deadlocks on reacquisition.
        std::unordered_set<std::string>
        collectRecursiveLockIds(const llvm::Module& module,
                                const ConcurrencySymbolClassifier& classifier)
        {
            constexpr unsigned kAttrOperandIndex = 0;
            constexpr unsigned kAttrTypeOperandIndex = 1;
            constexpr unsigned kMutexOperandIndex = 0;
            constexpr unsigned kInitAttrOperandIndex = 1;

            const SynchronizationEffectResolver effectResolver(classifier, module.getDataLayout());
            std::unordered_set<std::string> recursiveLockIds;

            // Recursiveness belongs to the object's type, so it is read from the global itself
            // rather than from an acquisition site. A standard library that wraps the lock in
            // another layer hides the type at the call, but never on the definition.
            for (const llvm::GlobalVariable& global : module.globals())
            {
                if (!designatesRecursiveLockType(global))
                    continue;

                if (const auto lockId = canonicalLockId(global, &module.getDataLayout());
                    lockId.has_value())
                {
                    recursiveLockIds.insert(*lockId);
                }
            }
            std::unordered_set<std::string> nonDefaultAttributeGroups;
            std::vector<const llvm::CallBase*> mutexInitCalls;

            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;

                for (const llvm::BasicBlock& block : function)
                {
                    for (const llvm::Instruction& instruction : block)
                    {
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        if (call == nullptr || call->arg_size() == 0)
                            continue;

                        for (const LockEffect& effect : effectResolver.resolve(*call))
                        {
                            if (!effect.recursiveLock)
                                continue;

                            recursiveLockIds.insert(effect.lockIds.begin(), effect.lockIds.end());
                        }

                        const CallKind kind = classifier.classify(*call);
                        if (kind == CallKind::PThreadMutexAttrSetType &&
                            call->arg_size() > kAttrTypeOperandIndex)
                        {
                            const auto* attributeType = llvm::dyn_cast<llvm::ConstantInt>(
                                call->getArgOperand(kAttrTypeOperandIndex));
                            if (attributeType == nullptr || attributeType->isZero())
                                continue;

                            if (const auto attrGroup = canonicalStorageGroupId(
                                    *call->getArgOperand(kAttrOperandIndex));
                                attrGroup.has_value())
                            {
                                nonDefaultAttributeGroups.insert(*attrGroup);
                            }
                            continue;
                        }

                        if (kind == CallKind::PThreadMutexInit &&
                            call->arg_size() > kInitAttrOperandIndex)
                        {
                            mutexInitCalls.push_back(call);
                        }
                    }
                }
            }

            for (const llvm::CallBase* call : mutexInitCalls)
            {
                const auto attrGroup =
                    canonicalStorageGroupId(*call->getArgOperand(kInitAttrOperandIndex));
                if (!attrGroup.has_value() || !nonDefaultAttributeGroups.contains(*attrGroup))
                    continue;

                if (const auto lockId = canonicalLockId(*call->getArgOperand(kMutexOperandIndex),
                                                        &module.getDataLayout());
                    lockId.has_value())
                {
                    recursiveLockIds.insert(*lockId);
                }
            }

            return recursiveLockIds;
        }

        std::vector<DirectCallBinding> buildDirectCallBindings(
            const std::vector<DirectCallSite>& sites,
            const std::unordered_map<const llvm::CallBase*, std::set<std::string>>& heldLocksByCall,
            const TaskConcurrencyResult& taskConcurrency)
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
                binding.callerInRootTask =
                    taskConcurrency.rootTaskFunctions.contains(site.callerFunctionId);
                if (const auto liveIt = taskConcurrency.liveEntriesAtInstruction.find(site.call);
                    liveIt != taskConcurrency.liveEntriesAtInstruction.end())
                {
                    binding.callsiteLiveEntries = liveIt->second;
                }
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

    TUFacts TUFactsBuilder::build(const llvm::Module& module,
                                  const ProgramSymbolIndex* program) const
    {
        const ConcurrencySymbolClassifier classifier;

        ThreadSpawnDetector spawnDetector(classifier);
        const bool crossTU = program != nullptr;
        ThreadSpawnCollection spawnFacts = spawnDetector.collect(module, crossTU);
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
        const TaskConcurrencyAnalyzer taskConcurrencyAnalyzer(classifier);
        const TaskConcurrencyResult taskConcurrency =
            taskConcurrencyAnalyzer.analyze(module, directCallSites, crossTU);

        TUFacts facts;
        facts.spawns = std::move(spawnFacts.spawns);
        facts.entryConcurrency = std::move(spawnFacts.entryConcurrency);
        facts.sequencedEntryPairs = taskConcurrency.sequencedEntryPairs;

        // Replace the raw spawn-site count by the number of instances that can actually be alive at
        // once: two spawn sites on mutually exclusive branches, or a spawn/join pair repeated
        // sequentially, never yield two concurrent instances.
        for (auto& [entryId, concurrency] : facts.entryConcurrency)
        {
            if (concurrency.staticSpawnCount >= 2 &&
                !taskConcurrency.overlappingSpawnEntries.contains(entryId))
            {
                concurrency.staticSpawnCount = 1;
            }
        }

        // A worker defined here may be spawned only from another unit: this module sees its body
        // and never its creation. The program index supplies the missing half, already corrected
        // by the unit that owns those spawn sites, so the local rule above must not run on it.
        if (crossTU)
        {
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration() || !program->isThreadEntry(function))
                    continue;

                EntryConcurrencyInfo& concurrency = facts.entryConcurrency[functionId(function)];
                concurrency.staticSpawnCount =
                    std::max(concurrency.staticSpawnCount, program->spawnCount(function));
                concurrency.hasSpawnInLoop =
                    concurrency.hasSpawnInLoop || program->spawnedInLoop(function);
            }
        }

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
                    propagated.propagated = true;
                    propagated.handleGroupId = *callerGroup + binding->suffix;
                    if (fact.sourceHandleGroupId.has_value())
                    {
                        const auto sourceBinding = parseLifecycleArgumentBinding(
                            fact.functionId, *fact.sourceHandleGroupId);
                        if (sourceBinding.has_value() &&
                            sourceBinding->argumentIndex < callSite.call->arg_size())
                        {
                            const auto sourceCallerGroup = canonicalStorageGroupId(
                                *callSite.call->getArgOperand(sourceBinding->argumentIndex));
                            if (!sourceCallerGroup.has_value())
                                continue;

                            propagated.sourceHandleGroupId =
                                *sourceCallerGroup + sourceBinding->suffix;
                        }
                    }
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

            const bool inRootTask =
                taskConcurrency.rootTaskFunctions.contains(functionId(function));
            std::vector<LockOrderFact> functionLockOrders = lockOrderCollector.collect(
                function, functionEntryLocks, taskConcurrency.liveEntriesAtInstruction);
            for (LockOrderFact& lockOrder : functionLockOrders)
                lockOrder.inRootTask = inRootTask;

            facts.lockOrders.insert(facts.lockOrders.end(), functionLockOrders.begin(),
                                    functionLockOrders.end());
        }

        facts.recursiveLockIds = collectRecursiveLockIds(module, classifier);

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

            if (const auto liveIt =
                    taskConcurrency.liveEntriesAtInstruction.find(pendingAccess.instruction);
                liveIt != taskConcurrency.liveEntriesAtInstruction.end())
            {
                pendingAccess.fact.liveEntries = liveIt->second;
            }
            pendingAccess.fact.inRootTask =
                taskConcurrency.rootTaskFunctions.contains(pendingAccess.fact.functionId);

            if (pendingAccess.root.kind == RootBindingKind::Global)
            {
                pendingAccess.fact.symbol = pendingAccess.root.symbol;
                pendingAccess.fact.region = pendingAccess.root.region;
                addConcreteAccess(concreteAccesses, concreteAccessKeys,
                                  std::move(pendingAccess.fact));
                continue;
            }

            const std::string functionKey = pendingAccess.fact.functionId;
            pendingAccess.fact.region = pendingAccess.root.region;
            pendingAccess.fact.allowCallsiteProjection = true;
            addParameterizedAccess(summariesByFunction, summaryKeysByFunction, functionKey,
                                   ParameterizedAccess{
                                       .root = pendingAccess.root,
                                       .fact = std::move(pendingAccess.fact),
                                   });
        }

        const std::vector<DirectCallBinding> directCallBindings = buildDirectCallBindings(
            directCallSites, lockPropagation.effectiveHeldLocksByCall, taskConcurrency);

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
                        // The callee's region is relative to the argument, which the call site
                        // itself may already have indexed into.
                        concrete.region = access.fact.region.rebasedOn(bindingIt->second.region);
                        concrete.heldLocks =
                            mergeHeldLocks(concrete.heldLocks, callBinding.callsiteHeldLocks);
                        concrete.inRootTask = callBinding.callerInRootTask;
                        concrete.liveEntries =
                            mergeLiveEntries(concrete.liveEntries, callBinding.callsiteLiveEntries);
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
                    propagatedAccess.root.region =
                        access.fact.region.rebasedOn(bindingIt->second.region);
                    propagatedAccess.fact.functionId = callBinding.callerFunctionId;
                    propagatedAccess.fact.region = propagatedAccess.root.region;
                    propagatedAccess.fact.heldLocks = mergeHeldLocks(
                        propagatedAccess.fact.heldLocks, callBinding.callsiteHeldLocks);
                    propagatedAccess.fact.inRootTask = callBinding.callerInRootTask;
                    propagatedAccess.fact.liveEntries = mergeLiveEntries(
                        propagatedAccess.fact.liveEntries, callBinding.callsiteLiveEntries);
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
                    remapped.inRootTask = callBinding.callerInRootTask;
                    remapped.liveEntries =
                        mergeLiveEntries(remapped.liveEntries, callBinding.callsiteLiveEntries);
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
