// SPDX-License-Identifier: Apache-2.0
#include "tu_facts_builder.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "condition_wait_collector.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"
#include "lock_order_collector.hpp"
#include "lock_scope_tracker.hpp"
#include "lock_state_propagator.hpp"
#include "lock_wrapper_summaries.hpp"
#include "process_lifecycle_collector.hpp"
#include "publication_analysis.hpp"
#include "shared_access_collector.hpp"
#include "shared_object_binding_collector.hpp"
#include "signal_handler_collector.hpp"
#include "cross_tu/program_symbol_index.hpp"
#include "fact_selection.hpp"
#include "thread_completion_analysis.hpp"
#include "task_concurrency_analyzer.hpp"
#include "thread_lifecycle_collector.hpp"
#include "thread_spawn_detector.hpp"
#include "thread_argument_escape_collector.hpp"
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
#include <filesystem>
#include <tuple>
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
            /// What the call passes for the integer parameters it knows: a constant, or one of
            /// the caller's own parameters. An integer argument missing here is unknown.
            std::unordered_map<unsigned, LinearIndex> indexArguments;
            SourceLocation callsiteLocation;
            std::set<std::string> callsiteHeldLocks;
            bool callerInRootTask = false;
            /// The threads running at the call, and the part of them the caller started.
            ThreadEntrySet liveAtCall;
            ThreadEntrySet startedAtCall;
            /// For a constructor call, the entries its class's constructors start on the object
            /// they construct: those running at the call hold objects built earlier.
            ThreadEntrySet entriesOnConstructedObject;
        };

        struct LifecycleArgumentBinding
        {
            unsigned argumentIndex = 0;
            std::string suffix;
        };

        /// Where an access the spawning function makes to `symbol`, over `byteSize` bytes starting
        /// `offset` bytes past `pointer`, lies among the elements of a spawn loop, with the entry
        /// it hands them to: in the element the current round is about to hand over, or in the one
        /// of the thread the current round of its join loop has certainly joined. Nothing when
        /// neither is proven.
        std::optional<std::pair<ElementOwner, std::string>> reachesElementNoThreadHolds(
            const llvm::Instruction& access, const llvm::Value& pointer, std::int64_t offset,
            std::uint64_t byteSize, const std::string& symbol, const SharedObjectBindings& bindings,
            const ThreadCompletionMap& completions, LlvmFunctionAnalysisProvider& analyses)
        {
            const llvm::DataLayout& layout = access.getModule()->getDataLayout();
            for (const auto& [entryFunctionId, binding] : bindings)
            {
                if (!binding.elementPerThread.has_value() || binding.object.symbol != symbol)
                    continue;
                const ElementPerThread& elements = *binding.elementPerThread;
                if (reachesElementBeforeHandOver(elements, access, pointer, offset, byteSize,
                                                 layout))
                    return std::pair{ElementOwner::NotYetHandedOver, entryFunctionId};
                const auto completion = completions.find(elements.spawn);
                if (completion != completions.end() && completion->second.roundJoin.has_value() &&
                    reachesElementOfJoinedThread(elements, *completion->second.roundJoin, access,
                                                 pointer, offset, byteSize, layout,
                                                 analyses.getDominatorTree(*access.getFunction())))
                    return std::pair{ElementOwner::JoinedThisRound, entryFunctionId};
            }
            return std::nullopt;
        }

        std::optional<std::pair<ElementOwner, std::string>> loadOrStoreReachesElementNoThreadHolds(
            const PendingAccess& access, const SharedObjectBindings& bindings,
            const ThreadCompletionMap& completions, LlvmFunctionAnalysisProvider& analyses)
        {
            const llvm::Value* pointer = llvm::getLoadStorePointerOperand(access.instruction);
            if (pointer == nullptr)
                return std::nullopt;
            const llvm::DataLayout& layout = access.instruction->getModule()->getDataLayout();
            const std::uint64_t byteSize =
                layout.getTypeStoreSize(llvm::getLoadStoreType(access.instruction)).getFixedValue();
            return reachesElementNoThreadHolds(*access.instruction, *pointer, 0, byteSize,
                                               access.root.symbol, bindings, completions, analyses);
        }

        std::string rootBindingKey(const RootBinding& binding)
        {
            std::string index;
            if (binding.index.has_value())
            {
                index = binding.index->widened ? std::string("@*")
                                               : "@" + std::to_string(binding.index->offset) + "+" +
                                                     std::to_string(binding.index->scale);
                index += "*arg" + std::to_string(binding.index->parameter.value_or(0));
            }

            if (binding.kind == RootBindingKind::Global)
                return "global:" + binding.symbol + binding.region.identity() + index;

            return "argument:" + std::to_string(binding.argumentIndex) + binding.region.identity() +
                   index;
        }

        /// Accesses with one key are one access reached several ways, and merge: the threads of
        /// each are kept, and the first one's place of report. Every other field the checkers read
        /// is in the key, except `boundBySpawn`. A thread's access copied to a direct call of its
        /// entry (#155) merges with the call's own access to the same object: the race between
        /// the call and the thread is found on the merged access, and is lost if the two are kept
        /// apart.
        std::string accessFactKey(const AccessFact& fact)
        {
            std::ostringstream stream;
            stream << fact.symbol << fact.region.identity() << "|" << fact.functionId << "|"
                   << toString(fact.kind) << "|" << (fact.isAtomic ? "atomic" : "plain") << "|"
                   << toString(fact.aliasProvenance) << "|" << fact.loweredLocation.file << "|"
                   << fact.loweredLocation.line << "|" << fact.loweredLocation.column << "|"
                   << fact.loweredLocation.function << "|" << fact.coarseCallEffect
                   << fact.guessedIdentity << fact.sharedObject << fact.inRootTask << "|"
                   << fact.instanceOwner;

            for (const std::string& lock : fact.heldLocks)
                stream << "|lock:" << lock;
            for (const std::string& lock : fact.beforeRelease)
                stream << "|release:" << lock;
            for (const std::string& lock : fact.afterAcquire)
                stream << "|acquire:" << lock;

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

        /// Merges the threads of `from` into `into`, the same access reached another way: it runs
        /// beside the threads of either. Whether any was new.
        bool mergeThreads(AccessFact& into, const AccessFact& from)
        {
            const std::size_t known = into.liveEntries.size() + into.startedEntries.size();
            into.liveEntries.insert(from.liveEntries.begin(), from.liveEntries.end());
            into.startedEntries.insert(from.startedEntries.begin(), from.startedEntries.end());
            return into.liveEntries.size() + into.startedEntries.size() != known;
        }

        /// Adds `fact`, or merges its threads into the same access already known. Whether
        /// anything changed.
        bool addConcreteAccess(std::vector<AccessFact>& accesses,
                               std::unordered_map<std::string, std::size_t>& accessKeys,
                               AccessFact fact)
        {
            const auto [known, inserted] = accessKeys.emplace(accessFactKey(fact), accesses.size());
            if (!inserted)
                return mergeThreads(accesses[known->second], fact);

            accesses.push_back(std::move(fact));
            return true;
        }

        std::string projectedAccessPreferenceKey(const AccessFact& fact)
        {
            std::ostringstream stream;
            stream << fact.symbol << fact.region.identity() << "|" << toString(fact.kind) << "|"
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
            std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>>&
                summaryKeysByFunction,
            const std::string& functionId, ParameterizedAccess access)
        {
            std::vector<ParameterizedAccess>& summary = summariesByFunction[functionId];
            const auto [known, inserted] = summaryKeysByFunction[functionId].emplace(
                parameterizedAccessKey(access), summary.size());
            if (!inserted)
                return mergeThreads(summary[known->second].fact, access.fact);

            summary.push_back(std::move(access));
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

        /// Identity of an argument that is an object a thread already holds.
        ///
        /// A method called on such an object by the thread that owns it describes the same bytes
        /// as the thread's own accesses, so it takes the same identity rather than being dropped
        /// for lack of a name. Kept separate from the access collector's resolution: this one
        /// answers about a call argument, where the address is passed directly rather than read
        /// out of a field.
        std::optional<RootBinding>
        sharedObjectArgument(const llvm::Value& operand,
                             const std::unordered_set<std::string>& sharedObjectIds)
        {
            if (sharedObjectIds.empty())
                return std::nullopt;

            // Only a pointer read out of the slot is the object; the slot's own address is the
            // variable holding it.
            const auto* load =
                llvm::dyn_cast<llvm::LoadInst>(operand.stripPointerCastsAndAliases());
            if (load == nullptr)
                return std::nullopt;

            const std::optional<std::string> slotId =
                canonicalStorageGroupId(*load->getPointerOperand());
            if (!slotId.has_value() || !sharedObjectIds.contains(*slotId))
                return std::nullopt;

            return RootBinding::global(*slotId);
        }

        /// Whether `symbol` names an object handed to a thread rather than a global: its name is
        /// an internal label the report must not print as a global's. The owner's own accesses
        /// and the accesses projected at call sites both ask here, so they cannot disagree.
        bool namesObjectSharedWithThread(const std::string& symbol,
                                         const std::unordered_set<std::string>& sharedObjectIds,
                                         const std::unordered_set<std::string>& localObjectIds)
        {
            return sharedObjectIds.contains(symbol) || localObjectIds.contains(symbol);
        }

        /// The locks of a callee's access as its caller holds them. A lock the callee named after
        /// one of its parameters is the lock at that place in what the call passes there; with
        /// nothing nameable passed, it protects nothing the caller can see.
        std::set<std::string> locksAtCallSite(const std::set<std::string>& calleeLocks,
                                              const DirectCallBinding& callBinding)
        {
            std::set<std::string> locks;
            for (const std::string& lockId : calleeLocks)
            {
                const std::optional<ParameterLockPlace> place = parameterLockPlace(lockId);
                if (!place.has_value())
                {
                    locks.insert(lockId);
                    continue;
                }

                const auto argumentIt = callBinding.argumentBindings.find(place->argumentIndex);
                if (argumentIt != callBinding.argumentBindings.end())
                    locks.insert(lockIdAt(argumentIt->second, place->place));
            }
            return locks;
        }

        /// The locks of a thread entry's access to the object it was handed, named after that
        /// object.
        std::set<std::string> locksOnSharedObject(const std::set<std::string>& entryLocks,
                                                  const SharedObjectBinding& binding)
        {
            std::set<std::string> locks;
            for (const std::string& lockId : entryLocks)
            {
                const std::optional<ParameterLockPlace> place = parameterLockPlace(lockId);
                if (!place.has_value())
                    locks.insert(lockId);
                else if (place->argumentIndex == binding.argumentIndex)
                    locks.insert(lockIdAt(binding.object, place->place));
            }
            return locks;
        }

        /// Bounds the orders a function keeps open for its callers. A recursion that moves the
        /// pointer it passes names a new lock at every step, so a fixpoint alone would not end.
        /// Past the bound, the orders kept no longer change: an order, or a thread beside one, is
        /// lost, never invented.
        constexpr std::size_t kMaxOpenLockOrdersPerFunction = 64;

        /// True for a lock only a call site can name: one reached through a parameter, or what
        /// the caller holds.
        bool namesCallerFrame(const std::string& lockId)
        {
            return lockId == kLocksHeldByCaller || parameterLockPlace(lockId).has_value();
        }

        bool isOpenLockOrder(const LockOrderFact& order)
        {
            return namesCallerFrame(order.firstLockId) || namesCallerFrame(order.secondLockId);
        }

        /// Identifies an order in its function. Instantiated at a call, every order a callee takes
        /// on two locks gets the call's location: the address mark keeps an order taken either way
        /// apart from one taken lower address first, which would otherwise hide it.
        std::string lockOrderKey(const LockOrderFact& order)
        {
            return order.functionId + "|" + order.firstLockId + "|" + order.secondLockId + "|" +
                   order.location.file + "|" + std::to_string(order.location.line) + "|" +
                   std::to_string(order.location.column) +
                   (order.lowerAddressFirst ? "|lower-address-first" : "");
        }

        /// Keeps one copy of `order` in `orders`. At a call, two acquisitions of the callee may
        /// give the same order, each beside its own threads: the copy kept runs beside both.
        /// False when `orders` gained nothing.
        bool keepLockOrder(std::vector<LockOrderFact>& orders,
                           std::unordered_map<std::string, std::size_t>& indexByKey,
                           LockOrderFact order)
        {
            const auto [indexIt, inserted] =
                indexByKey.try_emplace(lockOrderKey(order), orders.size());
            if (inserted)
            {
                orders.push_back(std::move(order));
                return true;
            }

            LockOrderFact& kept = orders[indexIt->second];
            const std::size_t known = kept.liveEntries.size() + kept.startedEntries.size();
            kept.liveEntries.insert(order.liveEntries.begin(), order.liveEntries.end());
            kept.startedEntries.insert(order.startedEntries.begin(), order.startedEntries.end());
            return kept.liveEntries.size() + kept.startedEntries.size() != known;
        }

        void addLockOrder(std::vector<LockOrderFact>& orders,
                          std::unordered_map<std::string, std::size_t>& orderIndexByKey,
                          LockOrderFact order)
        {
            // A lock named after a parameter is a different lock at every call, and so is what
            // the caller holds: neither is a gate two threads share.
            std::erase_if(order.heldLocks, namesCallerFrame);
            keepLockOrder(orders, orderIndexByKey, std::move(order));
        }

        /// The orders a function leaves open for its callers.
        struct OpenLockOrders
        {
            std::vector<LockOrderFact> orders;
            std::unordered_map<std::string, std::size_t> indexByKey;
        };

        /// False when the function's open orders gained nothing: it already keeps the order with
        /// every thread beside it, or keeps as many orders as it may, which then stay as they are.
        bool addOpenLockOrder(std::unordered_map<std::string, OpenLockOrders>& openByFunction,
                              LockOrderFact order)
        {
            OpenLockOrders& open = openByFunction[order.functionId];
            if (open.indexByKey.size() >= kMaxOpenLockOrdersPerFunction)
                return false;
            return keepLockOrder(open.orders, open.indexByKey, std::move(order));
        }

        /// The lock a callee names `calleeLockId`, as the caller of `call` names it. A named lock
        /// is the same lock in every frame; one named after a parameter is the lock at that place
        /// in what the call passes there, and nothing when the caller cannot name that.
        std::optional<std::string> lockAtCall(const std::string& calleeLockId,
                                              const llvm::CallBase& call,
                                              const SynchronizationEffectResolver& callerLocks)
        {
            const std::optional<ParameterLockPlace> place = parameterLockPlace(calleeLockId);
            if (!place.has_value())
                return calleeLockId;

            if (place->argumentIndex >= call.arg_size())
                return std::nullopt;

            const std::optional<std::string> passed =
                callerLocks.lockIdOf(*call.getArgOperand(place->argumentIndex));
            if (!passed.has_value())
                return std::nullopt;

            return lockIdAt(lockIdRoot(*passed), place->place);
        }

        /// What a direct call says about the orders its callee leaves open.
        struct LockOrderCall
        {
            const DirectCallSite& site;
            /// Names the locks the call passes as its caller names them.
            const SynchronizationEffectResolver& callerLocks;
            const std::set<std::string>& heldAtCall;
            /// Held at every call to the callee: the callee already orders its own acquisitions
            /// after them.
            const std::set<std::string>& calleeEntryLocks;
            const ThreadEntrySet& liveAtCall;
            /// The part of `liveAtCall` the caller started, itself or through an earlier call.
            const ThreadEntrySet& startedAtCall;
            bool callerInRootTask = false;
        };

        /// The threads of a callee's fact as one call sees them. At a point of the callee run the
        /// threads running at any call to it that it has not ended by then, and those it started
        /// below: through this call, only the ones running at this call, and those started below.
        /// Of those, the ones started in the caller's frame are this call's and its own starts.
        std::pair<ThreadEntrySet, ThreadEntrySet>
        threadsThroughCall(const ThreadEntrySet& liveEntries, const ThreadEntrySet& startedEntries,
                           const ThreadEntrySet& liveAtCall, const ThreadEntrySet& startedAtCall)
        {
            ThreadEntrySet live;
            ThreadEntrySet started;
            for (const std::string& entry : liveEntries)
            {
                const bool startedBelow = startedEntries.contains(entry);
                if (!startedBelow && !liveAtCall.contains(entry))
                    continue;

                live.insert(entry);
                if (startedBelow || startedAtCall.contains(entry))
                    started.insert(entry);
            }
            return {std::move(live), std::move(started)};
        }

        /// The orders `order`, left open by the callee, stands for at `call`: orders of the
        /// caller, taken at the call, between the locks the call passes and holds.
        std::vector<LockOrderFact> instantiateAtCall(const LockOrderFact& order,
                                                     const LockOrderCall& call)
        {
            const std::optional<std::string> second =
                lockAtCall(order.secondLockId, *call.site.call, call.callerLocks);
            if (!second.has_value())
                return {};

            std::set<std::string> held = call.heldAtCall;
            for (const std::string& lockId : order.heldLocks)
            {
                if (lockId == kLocksHeldByCaller)
                    continue;
                if (std::optional<std::string> heldLock =
                        lockAtCall(lockId, *call.site.call, call.callerLocks);
                    heldLock.has_value())
                {
                    held.insert(std::move(*heldLock));
                }
            }

            // Taken after whatever the caller holds: after each lock held at this call, and
            // after whatever the caller's own callers hold.
            std::vector<std::string> firsts;
            if (order.firstLockId == kLocksHeldByCaller)
            {
                for (const std::string& heldLock : call.heldAtCall)
                {
                    if (!call.calleeEntryLocks.contains(heldLock))
                        firsts.push_back(heldLock);
                }
                firsts.emplace_back(kLocksHeldByCaller);
            }
            else if (std::optional<std::string> first =
                         lockAtCall(order.firstLockId, *call.site.call, call.callerLocks);
                     first.has_value())
            {
                firsts.push_back(std::move(*first));
            }

            const auto [liveEntries, startedEntries] = threadsThroughCall(
                order.liveEntries, order.startedEntries, call.liveAtCall, call.startedAtCall);

            std::vector<LockOrderFact> instances;
            for (std::string& first : firsts)
            {
                // Taking again a lock whose type allows it is not a deadlock.
                if (first == *second && order.secondIsRecursive)
                    continue;

                // Two locks the callee keeps apart are one only for this caller, and the callee
                // may test for that before taking the second, as in `if (from == to) return;`:
                // a path this reading does not follow.
                const bool aliasedByCall = order.firstLockId != kLocksHeldByCaller &&
                                           order.firstLockId != order.secondLockId;
                if (first == *second && aliasedByCall)
                    continue;

                instances.push_back(LockOrderFact{
                    .functionId = call.site.callerFunctionId,
                    .firstLockId = std::move(first),
                    .secondLockId = *second,
                    .location = call.site.userLocation,
                    .heldLocks = held,
                    .inRootTask = call.callerInRootTask,
                    .liveEntries = liveEntries,
                    .startedEntries = startedEntries,
                    // The callee compares the addresses of whatever objects it is handed: at every
                    // call, it takes this order only when its first lock's object is the lower.
                    .lowerAddressFirst = order.lowerAddressFirst,
                    .secondIsRecursive = order.secondIsRecursive,
                });
            }
            return instances;
        }

        /// Whether `site` takes over its callee's orders. A standard function's body is the
        /// library's own way of doing what it does to its arguments: std::lock takes its locks in
        /// both orders, backing off with try_lock, which only a path-sensitive reading would tell
        /// from a deadlock.
        bool takesCalleeLockOrders(const DirectCallSite& site,
                                   const ConcurrencySymbolClassifier& classifier)
        {
            return site.call != nullptr && !classifier.callsStandardLibrary(*site.call);
        }

        /// The functions that run only where the calls taking over their orders run them: called
        /// that way, and never started as a thread. As for the locks held on entry, the calls this
        /// unit makes are taken to be all of them; a call through a pointer runs a function in no
        /// context the analysis knows of.
        std::unordered_set<std::string> functionsRunOnlyByDirectCalls(
            const std::vector<DirectCallSite>& sites, const ConcurrencySymbolClassifier& classifier,
            const std::unordered_map<std::string, EntryConcurrencyInfo>& threadEntries)
        {
            std::unordered_set<std::string> functions;
            for (const DirectCallSite& site : sites)
            {
                if (takesCalleeLockOrders(site, classifier) &&
                    !threadEntries.contains(site.calleeFunctionId))
                {
                    functions.insert(site.calleeFunctionId);
                }
            }
            return functions;
        }

        /// The functions some call to which holds a lock another call does not. The locks held on
        /// entry keep only those every call holds, so only the calls show such a lock. A lock the
        /// caller reaches through its own parameter is left out: it names a different lock at each
        /// call of the caller, and is no gate in the caller's order either.
        std::unordered_set<std::string>
        functionsCalledUnderDifferentLocks(const std::vector<DirectCallSite>& sites,
                                           const LockPropagationResult& locks)
        {
            std::unordered_set<std::string> functions;
            for (const DirectCallSite& site : sites)
            {
                const auto heldIt = locks.effectiveHeldLocksByCall.find(site.call);
                const auto entryIt = locks.entryLocksByFunction.find(site.calleeFunctionId);
                if (heldIt == locks.effectiveHeldLocksByCall.end() ||
                    entryIt == locks.entryLocksByFunction.end())
                {
                    continue;
                }

                for (const std::string& lockId : heldIt->second)
                {
                    if (!parameterLockPlace(lockId).has_value() &&
                        !entryIt->second.contains(lockId))
                    {
                        functions.insert(site.calleeFunctionId);
                    }
                }
            }
            return functions;
        }

        /// True when `order` is taken holding a lock one of its function's parameters leads to:
        /// each call names that lock, and two calls handed the same object hold the same one.
        bool isHeldThroughParameter(const LockOrderFact& order)
        {
            for (const std::string& lockId : order.heldLocks)
            {
                if (parameterLockPlace(lockId).has_value())
                    return true;
            }
            return false;
        }

        /// The threads running at `call` that may reach what `access` reaches through it: an
        /// access to the object a constructor builds is out of reach of the threads its class
        /// started on objects built before.
        std::pair<ThreadEntrySet, ThreadEntrySet>
        threadsAtCallFor(const ParameterizedAccess& access, const DirectCallBinding& call)
        {
            if (access.root.kind == RootBindingKind::Global || access.root.argumentIndex != 0 ||
                call.entriesOnConstructedObject.empty())
                return {call.liveAtCall, call.startedAtCall};
            ThreadEntrySet live = call.liveAtCall;
            ThreadEntrySet started = call.startedAtCall;
            for (const std::string& entry : call.entriesOnConstructedObject)
            {
                live.erase(entry);
                started.erase(entry);
            }
            return {std::move(live), std::move(started)};
        }

        /// The object a callee's access reaches, and the place in it, as the caller of `call` sees
        /// them. A pointer parameter becomes the object the call hands over, and an index
        /// parameter what the call passes: a constant settles the place, the caller's own
        /// parameter leaves it waiting on that one, widened or not, and anything else leaves it
        /// unknown. Empty when the call hands over no object the analysis can name.
        std::optional<RootBinding> rootAtCall(const ParameterizedAccess& access,
                                              const DirectCallBinding& call)
        {
            // In the callee's global, or relative to the pointer parameter it goes through.
            MemoryRegion place = access.fact.region;
            std::optional<LinearIndex> index;
            if (access.root.index.has_value())
            {
                if (const auto argumentIt = call.indexArguments.find(*access.root.index->parameter);
                    argumentIt != call.indexArguments.end())
                {
                    index = substituteLinearIndex(*access.root.index, argumentIt->second);
                }

                if (index.has_value() && !index->parameter.has_value())
                {
                    if (!index->widened)
                    {
                        place.hasKnownOffset = true;
                        place.byteOffset = index->offset;
                    }
                    index.reset();
                }
            }

            if (access.root.kind == RootBindingKind::Global)
            {
                RootBinding root = access.root;
                root.region = place;
                root.index = index;
                return root;
            }

            const auto bindingIt = call.argumentBindings.find(access.root.argumentIndex);
            if (bindingIt == call.argumentBindings.end())
                return std::nullopt;

            // The callee's place is relative to the argument, which the call site itself may
            // already have indexed into. An index the caller used there is not followed.
            const RootBinding& object = bindingIt->second;
            RootBinding root = object;
            root.region = place.rebasedOn(object.region, object.designated);
            root.index.reset();
            if (index.has_value() && object.region.hasKnownOffset)
            {
                root.index = substituteLinearIndex(
                    LinearIndex{.scale = 1, .offset = object.region.byteOffset}, *index);
            }
            return root;
        }

        /// The caller's own parameter, handed on unchanged: the one index a call may carry around
        /// a cycle, since it names the same value every round.
        bool handsOnParameter(const std::optional<LinearIndex>& index)
        {
            return index.has_value() && index->parameter.has_value() && index->scale == 1 &&
                   index->offset == 0;
        }

        /// Any other index a call closing a cycle passes would move the place every round, as a
        /// pointer the cycle moves would (#118): it is widened. The access keeps waiting on an
        /// integer parameter of the caller, the one the index comes from or else the one in the
        /// same position, so that the call entering the cycle makes it an access of its own
        /// caller, at an unknown place. Empty when the caller has neither.
        std::optional<LinearIndex> widenedIndexArgument(const llvm::Value& operand,
                                                        unsigned position,
                                                        const llvm::CallBase& call)
        {
            if (const std::optional<unsigned> source = integerSourceParameter(operand))
                return LinearIndex{.parameter = source, .widened = true};

            const llvm::Function& caller = *call.getFunction();
            if (position < caller.arg_size() && caller.getArg(position)->getType()->isIntegerTy())
                return LinearIndex{.parameter = position, .widened = true};
            return std::nullopt;
        }

        /// The entries started on objects of the class `call` constructs, when it calls a
        /// constructor: none of their threads started before the call holds the object it builds.
        const ThreadEntrySet* entriesOnObjectConstructedBy(
            const llvm::CallBase& call,
            const std::unordered_map<std::string, ThreadEntrySet>& entriesByConstructedClass)
        {
            const llvm::Function* callee = call.getCalledFunction();
            if (callee == nullptr)
                return nullptr;
            const std::optional<DemangledName> name = demangleFunction(callee->getName());
            if (!name.has_value() || !name->constructor)
                return nullptr;
            const auto entries = entriesByConstructedClass.find(name->context);
            return entries != entriesByConstructedClass.end() ? &entries->second : nullptr;
        }

        std::vector<DirectCallBinding> buildDirectCallBindings(
            const std::vector<DirectCallSite>& sites,
            const std::unordered_map<const llvm::CallBase*, std::set<std::string>>& heldLocksByCall,
            const TaskConcurrencyResult& taskConcurrency,
            const ProgramDefinedGlobals* programDefined,
            const std::unordered_set<std::string>& sharedObjectIds,
            const std::unordered_set<std::string>& localObjectIds,
            const std::unordered_map<std::string, ThreadEntrySet>& entriesByConstructedClass,
            const llvm::DataLayout& layout)
        {
            std::vector<DirectCallBinding> bindings;
            const std::unordered_set<const llvm::CallBase*> cycleCalls = callsClosingCycles(sites);

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
                if (const auto live = taskConcurrency.liveEntriesAtInstruction.find(site.call);
                    live != taskConcurrency.liveEntriesAtInstruction.end())
                    binding.liveAtCall = live->second;
                if (const auto started =
                        taskConcurrency.startedEntriesAtInstruction.find(site.call);
                    started != taskConcurrency.startedEntriesAtInstruction.end())
                    binding.startedAtCall = started->second;
                if (const ThreadEntrySet* entries =
                        entriesOnObjectConstructedBy(*site.call, entriesByConstructedClass))
                    binding.entriesOnConstructedObject = *entries;
                if (const auto heldLocksIt = heldLocksByCall.find(site.call);
                    heldLocksIt != heldLocksByCall.end())
                {
                    binding.callsiteHeldLocks = heldLocksIt->second;
                }

                // A call passing an integer instantiates the accesses its callee places with it
                // even when it passes nothing else, if only at an unknown place.
                bool passesInteger = false;
                for (unsigned argumentIndex = 0; argumentIndex < site.call->arg_size();
                     ++argumentIndex)
                {
                    const llvm::Value& operand = *site.call->getArgOperand(argumentIndex);
                    if (operand.getType()->isIntegerTy())
                    {
                        passesInteger = true;
                        std::optional<LinearIndex> index = resolveLinearIndex(operand);
                        if (cycleCalls.contains(site.call) && !handsOnParameter(index))
                            index = widenedIndexArgument(operand, argumentIndex, *site.call);
                        if (index.has_value())
                            binding.indexArguments.emplace(argumentIndex, *index);
                    }

                    // Resolved against the layout, like the accesses themselves: without it every
                    // indexing step reads as an unknown offset, and a member handed to a helper
                    // would stand for the whole object the helper's accesses are projected onto.
                    std::optional<RootBinding> root =
                        resolveTrackedRoot(operand, &layout, 0, programDefined);
                    if (!root.has_value())
                        root = resolveLocalObjectRoot(operand, &layout, 0, localObjectIds);
                    if (!root.has_value())
                        root = sharedObjectArgument(operand, sharedObjectIds);

                    // A call that closes a cycle and moves the pointer it hands on would move it
                    // again each time a callee's access is carried around the cycle, naming a new
                    // place every round, and the summaries would never settle. Around a cycle,
                    // where it points is left unknown, which overlaps every place it may reach.
                    if (root.has_value() && root->kind == RootBindingKind::Argument &&
                        cycleCalls.contains(site.call) &&
                        (!root->region.hasKnownOffset || root->region.byteOffset != 0))
                    {
                        root->region.hasKnownOffset = false;
                    }

                    if (root.has_value())
                        binding.argumentBindings.emplace(argumentIndex, *root);
                }

                if (!binding.argumentBindings.empty() || passesInteger)
                    bindings.push_back(std::move(binding));
            }

            return bindings;
        }
        /// Records the program symbols the whole-program passes will ask about this unit, so
        /// they can be answered once the module is gone. Runs last: it needs the handle group
        /// ids the lifecycle facts settled on.
        ProgramSymbolFacts collectProgramSymbolFacts(const llvm::Module& module,
                                                     const TUFacts& facts)
        {
            ProgramSymbolFacts program;
            program.abiKey = module.getTargetTriple() + '|' + module.getDataLayoutStr();
            // Only storage the access analysis can follow: a constant such as a vtable or type
            // information declared here and defined elsewhere never becomes shared state, so it
            // must not make this unit worth a second pass.
            for (const llvm::GlobalVariable& global : module.globals())
            {
                if (!canBeSharedState(global))
                    continue;
                (global.isDeclaration() ? program.declaredGlobals : program.definedGlobals)
                    .insert(programSymbol(global));
            }

            for (const llvm::Function& function : module)
            {
                std::string symbol = programSymbol(function);
                if (function.isDeclaration())
                    program.declaredFunctions.insert(symbol);
                program.functionSymbolsById.emplace(functionId(function), std::move(symbol));
            }

            const auto recordHandle = [&](const std::string& handleGroupId)
            {
                if (program.handleSymbolsByGroupId.contains(handleGroupId))
                    return;
                if (const llvm::GlobalVariable* handle =
                        globalOfStorageGroupId(module, handleGroupId);
                    handle != nullptr)
                {
                    program.handleSymbolsByGroupId.emplace(handleGroupId, programSymbol(*handle));
                }
            };
            for (const std::string& handleGroupId : facts.resolvedGlobalHandleIds)
                recordHandle(handleGroupId);
            for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
            {
                if (fact.action == ThreadLifecycleAction::Create)
                    recordHandle(fact.handleGroupId);
            }

            return program;
        }
    } // namespace

    TUFacts TUFactsBuilder::build(const llvm::Module& module, const FactSelection& selection,
                                  const ProgramSymbolIndex* program) const
    {
        const ConcurrencySymbolClassifier classifier;
        // One set of LLVM function analyses for this unit, shared by every collector below.
        // The module is never modified during the analysis, so nothing computed here goes
        // stale; the provider lives on this frame because a builder is shared across threads,
        // each analysing its own module.
        LlvmFunctionAnalysisProvider analyses;

        // Resolved once per unit and handed to every collector that follows calls: the spawn
        // detector, the context propagator and the builder itself used to walk them apart.
        // Every step below is gated on the facts the selected rules read; a skipped step leaves
        // its output empty, and nothing downstream of it runs either.
        const std::vector<DirectCallSite> directCallSites =
            selection.directCallSites() ? collectDirectCallSites(module, classifier, analyses)
                                        : std::vector<DirectCallSite>{};

        const bool crossTU = program != nullptr;
        ThreadSpawnCollection spawnFacts;
        if (selection.spawns())
        {
            spawnFacts =
                ThreadSpawnDetector(classifier, analyses).collect(module, directCallSites, crossTU);
        }

        // An `extern` declared here is real shared state only if the program defines it; a unit
        // on its own cannot tell that from an unresolved symbol.
        const ProgramDefinedGlobals* programDefined =
            crossTU ? &program->definedGlobals() : nullptr;

        // Resolved once for the whole unit: a wrapper's effect on the lock it is handed does not
        // depend on which caller is being looked at.
        LockWrapperSummaries lockWrapperSummaries;
        if (selection.lockWrapperSummaries())
        {
            lockWrapperSummaries =
                collectLockWrapperSummaries(module, classifier, analyses, module.getDataLayout());
        }

        // A helper defined in another unit is only a declaration here, so its body cannot be
        // summarised; the program supplies the summary the other unit produced.
        if (crossTU && selection.lockWrapperSummaries())
        {
            for (const llvm::Function& function : module)
            {
                if (!function.isDeclaration())
                    continue;

                if (const std::vector<ParameterLockEffect>* summary =
                        program->lockSummaryFor(programSymbol(function));
                    summary != nullptr)
                {
                    lockWrapperSummaries.emplace(functionId(function), *summary);
                }
            }
        }

        // Computed before the accesses are gathered: the thread that hands an object over keeps
        // reaching it through the same slot, so its own accesses need the identity too.
        SharedObjectBindings sharedObjectBindings;
        std::unordered_set<std::string> sharedObjectIds;
        std::unordered_set<std::string> localObjectIds;
        if (selection.lockState())
        {
            sharedObjectBindings =
                SharedObjectBindingCollector(classifier, analyses)
                    .collect(module, directCallSites, programDefined, &spawnFacts.entryConcurrency);

            // A thread entry another unit also starts may receive the same element there.
            if (crossTU)
            {
                for (const llvm::Function& function : module)
                {
                    const auto bindingIt = sharedObjectBindings.find(functionId(function));
                    if (bindingIt != sharedObjectBindings.end() &&
                        program->spawnCount(programSymbol(function)) > 1)
                    {
                        bindingIt->second.elementPerThread.reset();
                    }
                }
            }
            sharedObjectIds =
                sharedObjectNames(sharedObjectBindings, SharedObjectKind::BehindPointer);
            localObjectIds = sharedObjectNames(sharedObjectBindings, SharedObjectKind::Local);
        }

        std::vector<PendingAccess> pendingAccesses;
        std::unordered_map<const llvm::Instruction*, std::set<std::string>> heldLocksByAccess;
        if (selection.accesses)
        {
            pendingAccesses =
                SharedAccessCollector(classifier, analyses)
                    .collect(module, programDefined, &sharedObjectIds, &localObjectIds);

            std::unordered_map<std::string, std::unordered_set<const llvm::Instruction*>>
                trackedAccessesByFunction;
            std::unordered_map<std::string, const llvm::Function*> functionsById;
            for (const PendingAccess& pendingAccess : pendingAccesses)
            {
                trackedAccessesByFunction[pendingAccess.fact.functionId].insert(
                    pendingAccess.instruction);
                functionsById[pendingAccess.fact.functionId] = pendingAccess.function;
            }

            LockScopeTracker lockScopeTracker(classifier, analyses, &lockWrapperSummaries,
                                              &sharedObjectBindings);
            for (const auto& [functionKey, trackedAccesses] : trackedAccessesByFunction)
            {
                const llvm::Function* function = functionsById[functionKey];
                if (function == nullptr)
                    continue;

                std::unordered_map<const llvm::Instruction*, std::set<std::string>> functionLocks =
                    lockScopeTracker.collectHeldLocks(*function, trackedAccesses);
                heldLocksByAccess.insert(functionLocks.begin(), functionLocks.end());
            }
        }

        const bool readsCompletions = selection.taskConcurrency() || selection.threadLifecycles ||
                                      selection.threadArgumentEscapes ||
                                      selection.threadArgumentFrees ||
                                      selection.expiredThreadLocals;

        // A helper defined in another unit is only a declaration here; the program supplies the
        // parameters it joins, so a call to it still ends the thread it is handed.
        JoiningHelpers joiningHelpers;
        if (readsCompletions)
        {
            JoiningHelpers known;
            if (crossTU && selection.channels.helperJoins)
            {
                for (const llvm::Function& function : module)
                {
                    if (!function.isDeclaration())
                        continue;

                    if (const std::vector<JoinedParameter>* joined =
                            program->joinedParametersOf(programSymbol(function));
                        joined != nullptr)
                    {
                        known.emplace(&function, *joined);
                    }
                }
            }
            joiningHelpers = collectJoiningHelpers(module, classifier, std::move(known));
        }

        const ThreadCompletionMap completions =
            readsCompletions
                ? collectThreadCompletions(module, classifier, analyses, joiningHelpers)
                : ThreadCompletionMap{};
        TaskConcurrencyResult taskConcurrency;
        if (selection.taskConcurrency())
        {
            taskConcurrency = TaskConcurrencyAnalyzer(classifier, analyses)
                                  .analyze(module, directCallSites, completions, crossTU);
        }

        TUFacts facts;
        facts.spawns = std::move(spawnFacts.spawns);
        facts.entryConcurrency = std::move(spawnFacts.entryConcurrency);
        facts.lockWrapperSummaries = lockWrapperSummaries;
        if (selection.channels.helperJoins)
        {
            for (const auto& [function, joined] : joiningHelpers)
            {
                if (!function->isDeclaration())
                    facts.joiningHelpers.emplace(functionId(*function), joined);
            }
        }
        facts.sequencedEntryPairs = taskConcurrency.sequencedEntryPairs;

        // Replace the raw spawn-site count by the number of instances that can actually be alive at
        // once: two spawn sites on mutually exclusive branches, or a spawn/join pair repeated
        // sequentially, never yield two concurrent instances, and one spawn site in a helper
        // called twice does. Only meaningful once the task analysis has run, which is exactly
        // when the entry concurrency is read.
        if (selection.threadEntries)
        {
            for (auto& [entryId, concurrency] : facts.entryConcurrency)
            {
                concurrency.staticSpawnCount =
                    taskConcurrency.overlappingSpawnEntries.contains(entryId)
                        ? std::max<std::size_t>(concurrency.staticSpawnCount, 2)
                        : std::min<std::size_t>(concurrency.staticSpawnCount, 1);
            }
        }

        // A worker defined here may be spawned only from another unit: this module sees its body
        // and never its creation. The program index supplies the missing half, already corrected
        // by the unit that owns those spawn sites, so the local rule above must not run on it.
        if (crossTU)
        {
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;

                const std::string symbol = programSymbol(function);
                if (!program->isThreadEntry(symbol))
                    continue;

                EntryConcurrencyInfo& concurrency = facts.entryConcurrency[functionId(function)];
                concurrency.staticSpawnCount =
                    std::max(concurrency.staticSpawnCount, program->spawnCount(symbol));
                concurrency.hasSpawnInLoop =
                    concurrency.hasSpawnInLoop || program->spawnedInLoop(symbol);
            }
        }

        if (selection.threadEntries)
        {
            facts.reachableThreadEntriesByFunction =
                ThreadContextPropagator(classifier)
                    .collect(module, directCallSites, facts.entryConcurrency);
        }

        std::unordered_map<std::string, std::vector<ThreadLifecycleFact>> lifecycleFactsByFunction;
        std::unordered_set<std::string> lifecycleFactKeys;
        if (selection.threadLifecycles)
        {
            for (ThreadLifecycleFact fact :
                 ThreadLifecycleCollector(classifier, analyses).collect(module, completions))
            {
                addLifecycleFact(facts.threadLifecycles, lifecycleFactKeys,
                                 lifecycleFactsByFunction, std::move(fact));
            }
        }

        bool lifecycleChanged = selection.threadLifecycles;
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

        if (selection.threadArgumentEscapes || selection.threadArgumentFrees ||
            selection.expiredThreadLocals)
        {
            ThreadLifetimeFacts lifetimes =
                ThreadArgumentEscapeCollector(classifier, analyses).collect(module, completions);
            if (selection.threadArgumentEscapes)
                facts.threadArgumentEscapes = std::move(lifetimes.escapes);
            if (selection.threadArgumentFrees)
                facts.threadArgumentFrees = std::move(lifetimes.frees);
            if (selection.expiredThreadLocals)
                facts.expiredThreadLocals = std::move(lifetimes.expiredThreadLocals);
        }
        if (selection.signalHandlers)
        {
            facts.signalHandlers =
                SignalHandlerCollector(classifier).collect(module, directCallSites);
        }

        // A fork whose child goes on to replace its process image inherits nothing that
        // outlives the exec, so neither question this analysis asks about a fork applies to it.
        // Reachability runs over direct calls, the same way the wait obligation travels.
        if (selection.processLifecycle)
        {
            const ProcessLifecycleCollector processCollector(classifier);
            ProcessLifecycleCollection processes = processCollector.collect(module);
            facts.reapsChildProcesses =
                processes.reapsChildren || (crossTU && program->programReapsChildren());

            bool execChanged = true;
            while (execChanged)
            {
                execChanged = false;

                for (const DirectCallSite& site : directCallSites)
                {
                    if (!processes.functionsReachingExec.contains(site.calleeFunctionId))
                        continue;

                    execChanged =
                        processes.functionsReachingExec.insert(site.callerFunctionId).second ||
                        execChanged;
                }
            }

            facts.programCreatesThreads =
                !facts.spawns.empty() || (crossTU && program->programCreatesThreads());

            for (ProcessForkFact& fork : processes.forks)
            {
                fork.execReachable = processes.functionsReachingExec.contains(fork.functionId);
                facts.processForks.push_back(std::move(fork));
            }
        }

        // A helper cannot recheck a condition it does not know, so the obligation to loop around
        // a bare wait belongs to whoever called it — and keeps travelling outwards until some
        // caller does loop. Only the outermost function still carrying it is worth reporting:
        // that is where the missing loop has to be written.
        if (selection.conditionWaits)
        {
            std::unordered_map<std::string, ConditionWaitFact> unguardedByFunction;
            const ConditionWaitCollector conditionWaitCollector(classifier, analyses);
            for (const ConditionWaitFact& fact : conditionWaitCollector.collect(module))
            {
                if (!fact.guardedByLoop)
                    unguardedByFunction.emplace(fact.functionId, fact);
            }

            bool waitsChanged = true;
            while (waitsChanged)
            {
                waitsChanged = false;

                for (const DirectCallSite& site : directCallSites)
                {
                    if (site.insideLoop || unguardedByFunction.contains(site.callerFunctionId))
                        continue;

                    const auto calleeIt = unguardedByFunction.find(site.calleeFunctionId);
                    if (calleeIt == unguardedByFunction.end())
                        continue;

                    ConditionWaitFact inherited = calleeIt->second;
                    inherited.functionId = site.callerFunctionId;
                    inherited.location = site.userLocation;
                    inherited.viaHelper = true;
                    unguardedByFunction.emplace(site.callerFunctionId, std::move(inherited));
                    waitsChanged = true;
                }
            }

            // A function stops being the place to fix the wait once someone above it answers
            // for it: either a caller that inherited the obligation and is itself further out,
            // or a caller that loops. A helper every caller already wraps in a loop is used
            // correctly, and pointing at its body would be pointing at the wrong file.
            std::unordered_set<std::string> answeredByACaller;
            std::unordered_set<std::string> calledFromSomewhere;
            std::unordered_set<std::string> calledOutsideALoop;
            for (const DirectCallSite& site : directCallSites)
            {
                if (!unguardedByFunction.contains(site.calleeFunctionId))
                    continue;

                calledFromSomewhere.insert(site.calleeFunctionId);
                if (site.insideLoop)
                    continue;

                calledOutsideALoop.insert(site.calleeFunctionId);
                if (unguardedByFunction.contains(site.callerFunctionId))
                    answeredByACaller.insert(site.calleeFunctionId);
            }

            for (const std::string& called : calledFromSomewhere)
            {
                if (!calledOutsideALoop.contains(called))
                    answeredByACaller.insert(called);
            }

            // Filtering happens only here, after propagation: a standard library's own bare
            // wait is a necessary link in the chain, and dropping it at collection time would
            // cut the obligation before it ever reached the code that must loop.
            const std::optional<std::filesystem::path> sourceRoot = primarySourceRoot(module);
            for (auto& [waitFunctionId, fact] : unguardedByFunction)
            {
                if (answeredByACaller.contains(waitFunctionId))
                    continue;

                if (!isLikelyUserLocation(fact.location, sourceRoot))
                    continue;

                facts.conditionWaits.push_back(std::move(fact));
            }

            // The map order is an implementation detail; the report must not inherit it.
            std::sort(facts.conditionWaits.begin(), facts.conditionWaits.end(),
                      [](const ConditionWaitFact& lhs, const ConditionWaitFact& rhs)
                      {
                          return std::tie(lhs.location.file, lhs.location.line, lhs.location.column,
                                          lhs.functionId) <
                                 std::tie(rhs.location.file, rhs.location.line, rhs.location.column,
                                          rhs.functionId);
                      });
        }

        // Published so another unit's creation of the same global handle knows it is resolved.
        for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
        {
            const bool resolves = fact.action == ThreadLifecycleAction::Join ||
                                  fact.action == ThreadLifecycleAction::Detach;
            if (resolves && globalOfStorageGroupId(module, fact.handleGroupId) != nullptr)
                facts.resolvedGlobalHandleIds.insert(fact.handleGroupId);
        }

        // A handle this unit creates on a global that another unit joins is not outstanding: the
        // program resolves it, and only the program can see that.
        if (crossTU)
        {
            std::erase_if(facts.threadLifecycles,
                          [&](const ThreadLifecycleFact& fact)
                          {
                              if (fact.action != ThreadLifecycleAction::Create)
                                  return false;

                              const llvm::GlobalVariable* handle =
                                  globalOfStorageGroupId(module, fact.handleGroupId);
                              return handle != nullptr &&
                                     program->resolvesHandleElsewhere(programSymbol(*handle));
                          });
        }

        std::vector<AccessFact> concreteAccesses;
        std::unordered_map<std::string, std::size_t> concreteAccessKeys;
        std::unordered_map<std::string, std::vector<ParameterizedAccess>> summariesByFunction;
        std::unordered_map<std::string, std::unordered_map<std::string, std::size_t>>
            summaryKeysByFunction;

        LockPropagationResult lockPropagation;
        if (selection.lockState())
        {
            lockPropagation = LockStatePropagator(classifier, analyses, &lockWrapperSummaries,
                                                  &sharedObjectBindings)
                                  .collect(module, directCallSites);
        }

        LockOrderCollector lockOrderCollector(classifier, analyses, &lockWrapperSummaries,
                                              &sharedObjectBindings);
        std::unordered_map<std::string, std::size_t> lockOrderIndexByKey;
        std::unordered_map<std::string, OpenLockOrders> openLockOrders;
        // A closed order is judged once, where its function takes it, unless the calls know of a
        // lock around it that the function does not: the lock of an object a parameter leads to,
        // a different lock at each call that the function's own order cannot keep, or a lock
        // some calls hold and others do not, which its entry locks leave out. Either may be a
        // gate the calls that run together share. Such an order, in a function run only by direct
        // calls, is judged at each call instead, with the locks held and the threads running
        // there. A reacquisition needs no other thread, and no gate prevents it: it is judged
        // where it is taken.
        std::unordered_set<std::string> runOnlyByDirectCalls;
        std::unordered_set<std::string> calledUnderDifferentLocks;
        if (selection.lockOrders)
        {
            runOnlyByDirectCalls =
                functionsRunOnlyByDirectCalls(directCallSites, classifier, facts.entryConcurrency);
            calledUnderDifferentLocks =
                functionsCalledUnderDifferentLocks(directCallSites, lockPropagation);
        }
        std::unordered_map<std::string, std::vector<LockOrderFact>> ordersJudgedAtCalls;
        for (const llvm::Function& function : module)
        {
            if (!selection.lockOrders || function.isDeclaration())
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
            const bool runOnlyByCalls = runOnlyByDirectCalls.contains(functionId(function));
            const bool callsHoldDifferentLocks =
                calledUnderDifferentLocks.contains(functionId(function));
            std::vector<LockOrderFact> functionLockOrders = lockOrderCollector.collect(
                function, functionEntryLocks, taskConcurrency.liveEntriesAtInstruction,
                taskConcurrency.startedEntriesAtInstruction);
            for (LockOrderFact& lockOrder : functionLockOrders)
            {
                lockOrder.inRootTask = inRootTask;
                if (isOpenLockOrder(lockOrder))
                {
                    addOpenLockOrder(openLockOrders, std::move(lockOrder));
                }
                else if (runOnlyByCalls && lockOrder.firstLockId != lockOrder.secondLockId &&
                         (callsHoldDifferentLocks || isHeldThroughParameter(lockOrder)))
                {
                    ordersJudgedAtCalls[functionId(function)].push_back(std::move(lockOrder));
                }
                else
                {
                    addLockOrder(facts.lockOrders, lockOrderIndexByKey, std::move(lockOrder));
                }
            }
        }

        // An order a helper takes on the locks it is handed, or after the locks its caller
        // holds, happens at each call to it, between the locks that call passes and holds. It is
        // instantiated there as the caller's own order, which the caller's callers instantiate in
        // turn while it still names one of the caller's parameters. An order no call site can
        // name is left out: its locks are not known to be any lock another thread takes. The
        // closed orders left to the calls become the caller's own orders the same way.
        const std::set<std::string> noLocks;
        const ThreadEntrySet noEntries;
        bool lockOrdersChanged = selection.lockOrders;
        while (lockOrdersChanged)
        {
            lockOrdersChanged = false;
            for (const DirectCallSite& site : directCallSites)
            {
                if (!takesCalleeLockOrders(site, classifier))
                    continue;

                const auto openIt = openLockOrders.find(site.calleeFunctionId);
                const auto closedIt = ordersJudgedAtCalls.find(site.calleeFunctionId);
                if (openIt == openLockOrders.end() && closedIt == ordersJudgedAtCalls.end())
                    continue;

                const SharedObjectBinding* callerObject = nullptr;
                if (const auto objectIt = sharedObjectBindings.find(site.callerFunctionId);
                    objectIt != sharedObjectBindings.end())
                {
                    callerObject = &objectIt->second;
                }
                const SynchronizationEffectResolver callerLocks(
                    classifier, module.getDataLayout(), &lockWrapperSummaries,
                    /*nameParameterLocks=*/false, callerObject, &localObjectIds,
                    /*nameParameterFieldLocks=*/true);

                const auto heldIt = lockPropagation.effectiveHeldLocksByCall.find(site.call);
                const auto entryIt =
                    lockPropagation.entryLocksByFunction.find(site.calleeFunctionId);
                const auto liveIt = taskConcurrency.liveEntriesAtInstruction.find(site.call);
                const auto startedIt = taskConcurrency.startedEntriesAtInstruction.find(site.call);
                const LockOrderCall call{
                    .site = site,
                    .callerLocks = callerLocks,
                    .heldAtCall = heldIt != lockPropagation.effectiveHeldLocksByCall.end()
                                      ? heldIt->second
                                      : noLocks,
                    .calleeEntryLocks = entryIt != lockPropagation.entryLocksByFunction.end()
                                            ? entryIt->second
                                            : noLocks,
                    .liveAtCall = liveIt != taskConcurrency.liveEntriesAtInstruction.end()
                                      ? liveIt->second
                                      : noEntries,
                    .startedAtCall = startedIt != taskConcurrency.startedEntriesAtInstruction.end()
                                         ? startedIt->second
                                         : noEntries,
                    .callerInRootTask =
                        taskConcurrency.rootTaskFunctions.contains(site.callerFunctionId),
                };

                // Copied: in a recursion the caller is the callee, whose orders this adds to.
                std::vector<LockOrderFact> calleeOrders;
                if (openIt != openLockOrders.end())
                    calleeOrders = openIt->second.orders;
                if (closedIt != ordersJudgedAtCalls.end())
                {
                    calleeOrders.insert(calleeOrders.end(), closedIt->second.begin(),
                                        closedIt->second.end());
                }
                for (const LockOrderFact& order : calleeOrders)
                {
                    for (LockOrderFact& instance : instantiateAtCall(order, call))
                    {
                        if (isOpenLockOrder(instance))
                        {
                            lockOrdersChanged =
                                addOpenLockOrder(openLockOrders, std::move(instance)) ||
                                lockOrdersChanged;
                        }
                        else
                        {
                            addLockOrder(facts.lockOrders, lockOrderIndexByKey,
                                         std::move(instance));
                        }
                    }
                }
            }
        }

        if (selection.recursiveLockIds)
            facts.recursiveLockIds = collectRecursiveLockIds(module, classifier);

        // Reads the lifecycle facts, not the accesses, so it can precede them; the rest of this
        // function is the access pipeline, which only a rule reading accesses pays for.
        if (selection.program)
            facts.program = collectProgramSymbolFacts(module, facts);
        if (!selection.accesses)
            return facts;

        PublicationAnalysis publications = analyzePublications(
            module, pendingAccesses, directCallSites, facts.spawns, analyses, crossTU);
        if (selection.weakPublications)
            facts.weakPublications = std::move(publications.weakPublications);

        // An access to a global whose place waits on an index is placed at each call passing it.
        // A function some run of which no call of this unit starts, a thread's or main's, gets no
        // index for that run: the access stays in the function, at an unknown place that covers
        // every run, and is placed at none of its calls, where it would be counted twice.
        std::unordered_set<std::string> calledFunctionIds;
        for (const DirectCallSite& site : directCallSites)
            calledFunctionIds.insert(site.calleeFunctionId);
        auto addSummary = [&](const std::string& functionKey, ParameterizedAccess access)
        {
            if (access.root.kind != RootBindingKind::Global ||
                (calledFunctionIds.contains(functionKey) &&
                 !facts.entryConcurrency.contains(functionKey)))
            {
                return addParameterizedAccess(summariesByFunction, summaryKeysByFunction,
                                              functionKey, std::move(access));
            }

            AccessFact unplaced = std::move(access.fact);
            unplaced.symbol = access.root.symbol;
            unplaced.sharedObject =
                namesObjectSharedWithThread(unplaced.symbol, sharedObjectIds, localObjectIds);
            unplaced.allowCallsiteProjection = false;
            addConcreteAccess(concreteAccesses, concreteAccessKeys, std::move(unplaced));
            return false;
        };

        std::unordered_map<std::string, ThreadEntrySet> entriesByConstructedClass;
        for (const auto& [entryFunctionId, binding] : sharedObjectBindings)
        {
            if (!binding.constructedClass.empty())
                entriesByConstructedClass[binding.constructedClass].insert(entryFunctionId);
        }
        for (PendingAccess& pendingAccess : pendingAccesses)
        {
            if (pendingAccess.restatesCalleeAccesses)
                continue;

            if (const auto publicationIt = publications.ordering.find(pendingAccess.instruction);
                publicationIt != publications.ordering.end())
            {
                pendingAccess.fact.beforeRelease = publicationIt->second.beforeRelease;
                pendingAccess.fact.afterAcquire = publicationIt->second.afterAcquire;
            }

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
            if (const auto startedIt =
                    taskConcurrency.startedEntriesAtInstruction.find(pendingAccess.instruction);
                startedIt != taskConcurrency.startedEntriesAtInstruction.end())
            {
                pendingAccess.fact.startedEntries = startedIt->second;
            }
            // What a constructor call does to the object it builds is out of reach of the threads
            // its class started on objects built before, as the constructor's own accesses are.
            // This effect is the caller's own access at the call: where no other constructor
            // stands between, as when an ELF target calls the base-object constructor directly,
            // nothing projected through a constructor call carries it.
            if (const auto* call = llvm::dyn_cast<llvm::CallBase>(pendingAccess.instruction);
                call != nullptr && pendingAccess.callOperand == 0)
            {
                if (const ThreadEntrySet* entries =
                        entriesOnObjectConstructedBy(*call, entriesByConstructedClass))
                {
                    for (const std::string& entry : *entries)
                    {
                        pendingAccess.fact.liveEntries.erase(entry);
                        pendingAccess.fact.startedEntries.erase(entry);
                    }
                }
            }
            pendingAccess.fact.inRootTask =
                taskConcurrency.rootTaskFunctions.contains(pendingAccess.fact.functionId);

            if (pendingAccess.root.kind == RootBindingKind::Global &&
                !pendingAccess.root.index.has_value())
            {
                if (const auto element = loadOrStoreReachesElementNoThreadHolds(
                        pendingAccess, sharedObjectBindings, completions, analyses))
                {
                    pendingAccess.root.region.placeInElement(element->first, element->second);
                }
                pendingAccess.fact.symbol = pendingAccess.root.symbol;
                pendingAccess.fact.region = pendingAccess.root.region;
                // The root may name an object a thread holds rather than a global, and the two
                // are not described the same way to the reader.
                pendingAccess.fact.sharedObject = namesObjectSharedWithThread(
                    pendingAccess.fact.symbol, sharedObjectIds, localObjectIds);
                addConcreteAccess(concreteAccesses, concreteAccessKeys,
                                  std::move(pendingAccess.fact));
                continue;
            }

            const std::string functionKey = pendingAccess.fact.functionId;
            pendingAccess.fact.region = pendingAccess.root.region;
            pendingAccess.fact.allowCallsiteProjection = true;
            addSummary(functionKey, ParameterizedAccess{
                                        .root = pendingAccess.root,
                                        .fact = std::move(pendingAccess.fact),
                                    });
        }

        const std::vector<DirectCallBinding> directCallBindings = buildDirectCallBindings(
            directCallSites, lockPropagation.effectiveHeldLocksByCall, taskConcurrency,
            programDefined, sharedObjectIds, localObjectIds, entriesByConstructedClass,
            module.getDataLayout());

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
                    const std::optional<RootBinding> root = rootAtCall(access, callBinding);
                    if (!root.has_value())
                        continue;

                    if (root->kind == RootBindingKind::Global && !root->index.has_value())
                    {
                        AccessFact concrete = access.fact;
                        concrete.functionId = callBinding.callerFunctionId;
                        concrete.symbol = root->symbol;
                        // The binding may name an object a thread holds rather than a global,
                        // and the two are not described the same way to the reader.
                        concrete.sharedObject = namesObjectSharedWithThread(
                            concrete.symbol, sharedObjectIds, localObjectIds);
                        concrete.region = root->region;
                        // The callee reaches the element its argument points into: where the
                        // call stays in an element no running thread holds, so does the access.
                        const MemoryRegion& place = access.fact.region;
                        if (access.root.kind != RootBindingKind::Global &&
                            !access.root.index.has_value() && place.hasKnownOffset &&
                            access.root.argumentIndex < callBinding.call->arg_size())
                        {
                            if (const auto element = reachesElementNoThreadHolds(
                                    *callBinding.call,
                                    *callBinding.call->getArgOperand(access.root.argumentIndex),
                                    place.byteOffset, place.byteSize, root->symbol,
                                    sharedObjectBindings, completions, analyses))
                            {
                                concrete.region.placeInElement(element->first, element->second);
                            }
                        }
                        concrete.heldLocks =
                            mergeHeldLocks(locksAtCallSite(concrete.heldLocks, callBinding),
                                           callBinding.callsiteHeldLocks);
                        concrete.inRootTask = callBinding.callerInRootTask;
                        // The object is the one this call hands over: only the threads running
                        // beside the access through this call can reach it.
                        const auto [liveAtCall, startedAtCall] =
                            threadsAtCallFor(access, callBinding);
                        std::tie(concrete.liveEntries, concrete.startedEntries) =
                            threadsThroughCall(concrete.liveEntries, concrete.startedEntries,
                                               liveAtCall, startedAtCall);
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
                        .root = *root,
                        .fact = access.fact,
                    };
                    propagatedAccess.fact.functionId = callBinding.callerFunctionId;
                    propagatedAccess.fact.region = propagatedAccess.root.region;
                    propagatedAccess.fact.heldLocks = mergeHeldLocks(
                        locksAtCallSite(propagatedAccess.fact.heldLocks, callBinding),
                        callBinding.callsiteHeldLocks);
                    propagatedAccess.fact.inRootTask = callBinding.callerInRootTask;
                    const auto [liveAtCall, startedAtCall] = threadsAtCallFor(access, callBinding);
                    std::tie(propagatedAccess.fact.liveEntries,
                             propagatedAccess.fact.startedEntries) =
                        threadsThroughCall(propagatedAccess.fact.liveEntries,
                                           propagatedAccess.fact.startedEntries, liveAtCall,
                                           startedAtCall);
                    if (shouldRemapAccessToCallsite(propagatedAccess.fact,
                                                    callBinding.callsiteLocation))
                    {
                        propagatedAccess.fact.userLocation = callBinding.callsiteLocation;
                    }

                    changed =
                        addSummary(callBinding.callerFunctionId, std::move(propagatedAccess)) ||
                        changed;
                }
            }
        }

        // An access left rooted at a parameter has no identity of its own. When that parameter
        // is how a thread entry receives an object the spawn sites prove is shared, the object's
        // storage supplies one: every thread reached it from the same place, so two accesses
        // through it really are two accesses to the same bytes.
        //
        // Nothing is granted without that proof. An entry handed a different object each time
        // keeps no identity at all, which is what separates this from comparing objects by type.
        {
            for (const auto& [summaryFunctionId, summary] : summariesByFunction)
            {
                const auto bindingIt = sharedObjectBindings.find(summaryFunctionId);
                if (bindingIt == sharedObjectBindings.end())
                    continue;

                for (const ParameterizedAccess& access : summary)
                {
                    if (access.root.kind != RootBindingKind::Argument ||
                        access.root.argumentIndex != bindingIt->second.argumentIndex)
                    {
                        continue;
                    }

                    const SharedObjectBinding& binding = bindingIt->second;
                    AccessFact fact = access.fact;
                    fact.symbol = binding.object.symbol;
                    // The thread's accesses are relative to what it was handed, which the spawn
                    // may have pointed into the object, exactly as for a call argument.
                    fact.region = access.root.region.rebasedOn(binding.object.region,
                                                               binding.object.designated);
                    // Every access through it stays in the element the thread was handed.
                    if (binding.elementPerThread.has_value())
                    {
                        fact.region.placeInElement(ElementOwner::AccessingThread,
                                                   summaryFunctionId);
                    }
                    fact.heldLocks = locksOnSharedObject(fact.heldLocks, binding);
                    // A global keeps its own name in the report; other objects have none to show.
                    fact.sharedObject = binding.kind != SharedObjectKind::Global;
                    // Only the thread the spawn starts reaches this object. The initial thread, or
                    // another thread, reaches the entry through a direct call, on what that call
                    // passes, and the call's own binding makes that an access of the caller.
                    fact.inRootTask = false;
                    fact.boundBySpawn = true;
                    if (!binding.constructedClass.empty())
                        fact.instanceOwner = summaryFunctionId;
                    addConcreteAccess(concreteAccesses, concreteAccessKeys, std::move(fact));
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
                // Only the calls that hand their callee an object: one bound for its integers
                // alone is here to place the accesses those integers index, nothing else.
                if (callBinding.argumentBindings.empty())
                    continue;

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
                        mergeHeldLocks(locksAtCallSite(remapped.heldLocks, callBinding),
                                       callBinding.callsiteHeldLocks);
                    remapped.inRootTask = callBinding.callerInRootTask;
                    remapped.instanceOwner.clear();
                    std::tie(remapped.liveEntries, remapped.startedEntries) =
                        threadsThroughCall(remapped.liveEntries, remapped.startedEntries,
                                           callBinding.liveAtCall, callBinding.startedAtCall);
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
        // A lock still named after a parameter was never substituted by a call site: it is the
        // lock of whatever some caller passes, and two such names in different functions are not
        // the same lock.
        for (AccessFact& access : facts.accesses)
        {
            std::erase_if(access.heldLocks, [](const std::string& lockId)
                          { return parameterLockPlace(lockId).has_value(); });
        }
        orderStaticInitialization(module, facts.accesses, crossTU);
        return facts;
    }
} // namespace ctrace::concurrency::internal::analysis
