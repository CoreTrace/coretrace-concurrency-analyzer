// SPDX-License-Identifier: Apache-2.0
#include "tu_facts_builder.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "lock_scope_tracker.hpp"
#include "shared_access_collector.hpp"
#include "thread_spawn_detector.hpp"

#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

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
            std::string callerFunctionId;
            std::string calleeFunctionId;
            std::unordered_map<unsigned, RootBinding> argumentBindings;
            SourceLocation callsiteLocation;
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
                   << "|" << fact.loweredLocation.line << "|" << fact.loweredLocation.column
                   << "|" << fact.loweredLocation.function;

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

        std::vector<DirectCallBinding>
        collectDirectCallBindings(const llvm::Module& module,
                                  const ConcurrencySymbolClassifier& classifier)
        {
            std::vector<DirectCallBinding> bindings;

            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;

                for (const llvm::BasicBlock& block : function)
                {
                    for (const llvm::Instruction& instruction : block)
                    {
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        if (call == nullptr)
                            continue;

                        const llvm::Function* callee = classifier.directCallee(*call);
                        if (callee == nullptr || callee->isDeclaration())
                            continue;

                        DirectCallBinding binding;
                        binding.callerFunctionId = functionId(function);
                        binding.calleeFunctionId = functionId(*callee);
                        binding.callsiteLocation = resolveSourceLocations(instruction).userLocation;

                        for (unsigned argumentIndex = 0; argumentIndex < call->arg_size();
                             ++argumentIndex)
                        {
                            const std::optional<RootBinding> root =
                                resolveTrackedRoot(*call->getArgOperand(argumentIndex));
                            if (root.has_value())
                                binding.argumentBindings.emplace(argumentIndex, *root);
                        }

                        if (!binding.argumentBindings.empty())
                            bindings.push_back(std::move(binding));
                    }
                }
            }

            return bindings;
        }
    } // namespace

    TUFacts TUFactsBuilder::build(const llvm::Module& module) const
    {
        const ConcurrencySymbolClassifier classifier;

        ThreadSpawnDetector spawnDetector(classifier);
        ThreadSpawnCollection spawnFacts = spawnDetector.collect(module);

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

        TUFacts facts;
        facts.spawns = std::move(spawnFacts.spawns);
        facts.entryConcurrency = std::move(spawnFacts.entryConcurrency);

        std::vector<AccessFact> concreteAccesses;
        std::unordered_set<std::string> concreteAccessKeys;
        std::unordered_map<std::string, std::vector<ParameterizedAccess>> summariesByFunction;
        std::unordered_map<std::string, std::unordered_set<std::string>> summaryKeysByFunction;

        for (PendingAccess& pendingAccess : pendingAccesses)
        {
            const auto heldLocksIt = heldLocksByAccess.find(pendingAccess.instruction);
            if (heldLocksIt != heldLocksByAccess.end())
                pendingAccess.fact.heldLocks = heldLocksIt->second;

            if (pendingAccess.root.kind == RootBindingKind::Global)
            {
                pendingAccess.fact.symbol = pendingAccess.root.symbol;
                addConcreteAccess(concreteAccesses, concreteAccessKeys, std::move(pendingAccess.fact));
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
            collectDirectCallBindings(module, classifier);

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
                    if (shouldRemapAccessToCallsite(propagatedAccess.fact,
                                                   callBinding.callsiteLocation))
                    {
                        propagatedAccess.fact.userLocation = callBinding.callsiteLocation;
                    }

                    changed =
                        addParameterizedAccess(summariesByFunction, summaryKeysByFunction,
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
