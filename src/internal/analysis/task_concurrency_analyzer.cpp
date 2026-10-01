// SPDX-License-Identifier: Apache-2.0
#include "task_concurrency_analyzer.hpp"
#include "contained_entry_analysis.hpp"
#include "thread_completion_analysis.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        /// Operand carrying the thread handle, for every lifecycle call this analysis inspects.
        constexpr unsigned kHandleOperandIndex = 0;
        /// Operand of a `std::thread` move naming the thread it moves from.
        constexpr unsigned kMoveSourceOperandIndex = 1;
        constexpr unsigned kPThreadEntryOperandIndex = 2;
        constexpr unsigned kStdThreadCallableOperandIndex = 1;

        constexpr std::string_view kProgramEntryFunction = "main";
        constexpr std::string_view kGlobalStoragePrefix = "global:";
        constexpr std::string_view kParameterStoragePrefix = "arg:";
        constexpr std::string_view kUnknownIndex = "[*]";

        /// Beyond this many indexing steps a handle is no longer named: a recursive call that
        /// keeps descending into an object would otherwise name ever longer paths.
        // ponytail: fixed depth; follow recursive structures if deep object graphs need it.
        constexpr std::size_t kMaxHandleSteps = 16;

        struct SpawnSite
        {
            const llvm::Instruction* instruction = nullptr;
            /// The handle the thread ends up in: where it was created, or where a move carried it.
            std::string handleGroupId;
            std::string entryFunctionId;
        };

        struct JoinSite
        {
            const llvm::Instruction* instruction = nullptr;
            std::string handleGroupId;
            std::optional<JoinSuccess> success;
        };

        /// A `std::thread` moved into storage from anywhere but a thread the same function started
        /// there: the storage then holds a thread this analysis did not see start.
        struct AdoptionSite
        {
            const llvm::Instruction* instruction = nullptr;
            std::string handleGroupId;
        };

        struct FunctionLifecycleSites
        {
            std::vector<SpawnSite> spawns;
            std::vector<JoinSite> joins;
            std::vector<AdoptionSite> adoptions;
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
            std::map<std::string, std::vector<AdoptionSite>> movesFrom;

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
                            .success = joinSuccessOf(*call, kind),
                        });
                        continue;
                    }

                    if (kind == CallKind::StdThreadMove)
                    {
                        const AdoptionSite move{
                            .instruction = &instruction,
                            .handleGroupId = *handleGroupId,
                        };
                        const std::optional<std::string> source =
                            call->arg_size() > kMoveSourceOperandIndex
                                ? canonicalStorageGroupId(
                                      *call->getArgOperand(kMoveSourceOperandIndex))
                                : std::nullopt;
                        if (source.has_value())
                            movesFrom[*source].push_back(move);
                        else
                            sites.adoptions.push_back(move);
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

            // A thread built into a temporary and moved into its owner, as `worker =
            // std::thread(...)` does, is joined through where it was moved to.
            for (SpawnSite& spawn : sites.spawns)
            {
                const auto moves = movesFrom.find(spawn.handleGroupId);
                if (moves != movesFrom.end() && moves->second.size() == 1)
                {
                    spawn.handleGroupId = moves->second.front().handleGroupId;
                    movesFrom.erase(moves);
                }
            }

            // Every other move hands its destination a thread started somewhere else.
            for (const auto& [source, moves] : movesFrom)
                sites.adoptions.insert(sites.adoptions.end(), moves.begin(), moves.end());

            return sites;
        }

        /// A handle a join can be matched on: no index the path leaves unknown, and not so deep
        /// that recursion built it.
        bool isNameableHandle(std::string_view handle)
        {
            return handle.find(kUnknownIndex) == std::string_view::npos &&
                   static_cast<std::size_t>(std::count(handle.begin(), handle.end(), '[')) <=
                       kMaxHandleSteps;
        }

        /// Reads `arg:<function>:<index><path>`, the way `canonicalStorageGroupId` names storage
        /// reached through a parameter of `function`.
        std::optional<std::pair<unsigned, std::string_view>>
        parameterPlaceOf(std::string_view handle, std::string_view function)
        {
            if (!handle.starts_with(kParameterStoragePrefix))
                return std::nullopt;
            handle.remove_prefix(kParameterStoragePrefix.size());
            if (!handle.starts_with(function) || handle.size() <= function.size() ||
                handle[function.size()] != ':')
            {
                return std::nullopt;
            }
            handle.remove_prefix(function.size() + 1);

            const std::size_t digits = handle.find_first_not_of("0123456789");
            const std::string_view index = handle.substr(0, digits);
            const std::string_view path =
                digits == std::string_view::npos ? std::string_view{} : handle.substr(digits);
            if (index.empty() || (!path.empty() && path.front() != '['))
                return std::nullopt;
            return std::pair{static_cast<unsigned>(std::stoul(std::string(index))), path};
        }

        /// A handle the callee names, as the caller names it at `call`: a global stays itself,
        /// storage reached through a parameter becomes what the call passes there.
        std::optional<std::string> handleAtCaller(const std::string& calleeHandle,
                                                  std::string_view calleeFunctionId,
                                                  const llvm::CallBase& call)
        {
            if (calleeHandle.starts_with(kGlobalStoragePrefix))
                return calleeHandle;

            const auto place = parameterPlaceOf(calleeHandle, calleeFunctionId);
            if (!place.has_value() || place->first >= call.arg_size())
                return std::nullopt;

            const std::optional<std::string> argument =
                canonicalStorageGroupId(*call.getArgOperand(place->first));
            if (!argument.has_value())
                return std::nullopt;

            std::string handle = *argument + std::string(place->second);
            if (!isNameableHandle(handle))
                return std::nullopt;
            return handle;
        }

        /// A handle the caller names, as the callee names it: through the parameter the call hands
        /// the storage over on. Nothing when the call hands it over on none.
        std::optional<std::string> handleAtCallee(const std::string& callerHandle,
                                                  const llvm::CallBase& call,
                                                  std::string_view calleeFunctionId)
        {
            if (callerHandle.starts_with(kGlobalStoragePrefix))
                return callerHandle;

            for (unsigned index = 0; index < call.arg_size(); ++index)
            {
                const std::optional<std::string> argument =
                    canonicalStorageGroupId(*call.getArgOperand(index));
                if (!argument.has_value() || !isNameableHandle(*argument) ||
                    !std::string_view(callerHandle).starts_with(*argument))
                {
                    continue;
                }

                const std::string_view path =
                    std::string_view(callerHandle).substr(argument->size());
                if (!path.empty() && path.front() != '[')
                    continue;

                return std::string(kParameterStoragePrefix) + std::string(calleeFunctionId) + ":" +
                       std::to_string(index) + std::string(path);
            }
            return std::nullopt;
        }

        /// A running thread as one function sees it: the entry it runs and, when that function
        /// can name it, the handle a join would take there.
        struct ThreadInstance
        {
            std::string entryFunctionId;
            std::optional<std::string> handle;

            friend bool operator<(const ThreadInstance& lhs, const ThreadInstance& rhs)
            {
                return std::tie(lhs.entryFunctionId, lhs.handle) <
                       std::tie(rhs.entryFunctionId, rhs.handle);
            }

            friend bool operator==(const ThreadInstance& lhs, const ThreadInstance& rhs) = default;
        };

        using ThreadInstances = std::set<ThreadInstance>;

        ThreadEntrySet entriesOf(const ThreadInstances& instances)
        {
            ThreadEntrySet entries;
            for (const ThreadInstance& instance : instances)
                entries.insert(instance.entryFunctionId);
            return entries;
        }

        std::vector<const llvm::Instruction*> normalReturns(const llvm::Function& function,
                                                            const llvm::DominatorTree& dominators)
        {
            std::vector<const llvm::Instruction*> returns;
            for (const llvm::BasicBlock& block : function)
            {
                if (dominators.isReachableFromEntry(&block) &&
                    llvm::isa<llvm::ReturnInst>(block.getTerminator()))
                {
                    returns.push_back(block.getTerminator());
                }
            }
            return returns;
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

        /// The threads running in each function: started there, left running by a function it
        /// calls, or already running where its callers call it. Each is carried with its handle,
        /// named in the frame of the function holding it, so that a join anywhere that names the
        /// same storage ends it. A thread whose handle cannot be named stays running, as it did
        /// before handles were followed.
        class RunningThreads
        {
          public:
            RunningThreads(const llvm::Module& module,
                           const std::vector<DirectCallSite>& directCallSites,
                           const ThreadCompletionMap& completions,
                           const ConcurrencySymbolClassifier& classifier,
                           LlvmFunctionAnalysisProvider& analyses, bool includeExternalEntries)
                : directCallSites_(directCallSites), completions_(completions), analyses_(analyses)
            {
                for (const llvm::Function& function : module)
                {
                    if (function.isDeclaration())
                        continue;
                    functionsById_.emplace(functionId(function), &function);
                    FunctionThreads& threads = functions_[&function];
                    threads.sites =
                        collectLifecycleSites(function, classifier, includeExternalEntries);
                    threads.returns = normalReturns(function, analyses_.getDominatorTree(function));
                }

                for (const DirectCallSite& site : directCallSites_)
                {
                    if (site.call == nullptr)
                        continue;
                    if (const auto it = functions_.find(site.call->getFunction());
                        it != functions_.end() && functionOf(site.calleeFunctionId) != nullptr)
                    {
                        it->second.calls.push_back(&site);
                    }
                }

                summarizeHandles();
            }

            [[nodiscard]] const FunctionLifecycleSites&
            sitesOf(const llvm::Function& function) const
            {
                return functions_.at(&function).sites;
            }

            /// Whether the thread started at `spawn` is still running at `point`: the spawn must
            /// dominate the point, and no join of its handle, in place or through a call, nor the
            /// proven completion of the spawn may have finished it there.
            [[nodiscard]] bool spawnIsLiveAt(const llvm::Function& function, const SpawnSite& spawn,
                                             const llvm::Instruction& point) const
            {
                if (spawn.instruction == &point)
                    return false;

                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                if (!dominators.dominates(spawn.instruction, &point))
                    return false;

                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(spawn.instruction))
                {
                    const auto completion = completions_.find(call);
                    if (completion != completions_.end() && completion->second.ended.has_value() &&
                        completion->second.ended->covers(point, dominators))
                        return false;
                }

                for (const JoinSite& join : sitesOf(function).joins)
                {
                    if (join.handleGroupId == spawn.handleGroupId && join.success.has_value() &&
                        join.success->covers(point, dominators))
                        return false;
                }

                const std::optional<std::string> handle = spawnHandle(function, spawn);
                return !handle.has_value() ||
                       !joinedThroughCallAt(function, *handle, point, spawn.instruction);
            }

            /// Settles what every function leaves running once it returns. Whether a handle is
            /// ambiguous comes from the summaries alone, so what a function leaves running only
            /// grows with what its callees leave running.
            void settleLeaks()
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& [function, data] : functions_)
                    {
                        ThreadInstances leaked = leakedFrom(*function);
                        ThreadInstances& known = leaked_[function];
                        if (leaked != known)
                        {
                            known = std::move(leaked);
                            changed = true;
                        }
                    }
                }
                cacheIntroductions();
            }

            /// Settles what every function receives running from the calls made to it.
            void settleInheritance()
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const DirectCallSite& site : directCallSites_)
                    {
                        if (site.call == nullptr)
                            continue;
                        const llvm::Function* callee = functionOf(site.calleeFunctionId);
                        if (callee == nullptr || !functions_.contains(site.call->getFunction()))
                            continue;

                        for (const ThreadInstance& instance :
                             liveAt(*site.call->getFunction(), *site.call))
                        {
                            ThreadInstance passed{instance.entryFunctionId, std::nullopt};
                            if (instance.handle.has_value())
                                passed.handle = handleAtCallee(*instance.handle, *site.call,
                                                               site.calleeFunctionId);
                            changed =
                                inherited_[callee].insert(std::move(passed)).second || changed;
                        }
                    }
                }
            }

            /// Every thread running at `point`, named in the frame of `function`.
            [[nodiscard]] ThreadInstances liveAt(const llvm::Function& function,
                                                 const llvm::Instruction& point) const
            {
                ThreadInstances live = startedAt(function, point);
                live.merge(inheritedAt(function, point));
                return live;
            }

            /// The threads running at `point` that `function` started, itself or through a call
            /// it made: no caller brings them in.
            [[nodiscard]] ThreadInstances startedAt(const llvm::Function& function,
                                                    const llvm::Instruction& point) const
            {
                ThreadInstances started;
                for (const SpawnSite& spawn : sitesOf(function).spawns)
                {
                    if (spawnIsLiveAt(function, spawn, point))
                        started.insert(
                            ThreadInstance{spawn.entryFunctionId, spawnHandle(function, spawn)});
                }

                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                if (const auto calls = introducedByCalls_.find(&function);
                    calls != introducedByCalls_.end())
                {
                    for (const auto& [call, instance] : calls->second)
                    {
                        if (call == &point || !dominators.dominates(call, &point))
                            continue;
                        if (!instance.handle.has_value() ||
                            !joinedAt(function, *instance.handle, point, call))
                            started.insert(instance);
                    }
                }
                return started;
            }

            /// The threads running where `function` was called that are still running at `point`.
            [[nodiscard]] ThreadInstances inheritedAt(const llvm::Function& function,
                                                      const llvm::Instruction& point) const
            {
                ThreadInstances live;
                if (const auto inherited = inherited_.find(&function);
                    inherited != inherited_.end())
                {
                    // A handle that arrives named is not filled again here: every caller counts
                    // this function's starts into it, and a handle filled twice is not named.
                    for (const ThreadInstance& instance : inherited->second)
                    {
                        if (!instance.handle.has_value() ||
                            !joinedAt(function, *instance.handle, point, nullptr))
                            live.insert(instance);
                    }
                }
                return live;
            }

            /// Whether anything may run beside some instruction of `function`.
            [[nodiscard]] bool mayHoldThreads(const llvm::Function& function) const
            {
                return !sitesOf(function).spawns.empty() ||
                       introducedByCalls_.contains(&function) || inherited_.contains(&function);
            }

            [[nodiscard]] const std::unordered_map<const llvm::Function*, ThreadInstances>&
            leaked() const
            {
                return leaked_;
            }

          private:
            /// What a function does to the handles it names, in place or through the calls it
            /// makes.
            struct HandleSummary
            {
                /// Handles it may start a thread into, whether or not that thread outlives it.
                std::set<std::string> started;
                /// Handles it joins on every path to a normal return.
                std::set<std::string> joined;
                /// Handles it joins, returning that join's status as its own: a call to it is then
                /// a join whose status is the call's result.
                std::set<std::string> reported;
            };

            struct FunctionThreads
            {
                FunctionLifecycleSites sites;
                std::vector<const DirectCallSite*> calls;
                std::vector<const llvm::Instruction*> returns;
                HandleSummary summary;
            };

            [[nodiscard]] const HandleSummary& summaryOf(const DirectCallSite& site) const
            {
                return functions_.at(functionOf(site.calleeFunctionId)).summary;
            }

            [[nodiscard]] const llvm::Function* functionOf(const std::string& id) const
            {
                const auto it = functionsById_.find(id);
                return it != functionsById_.end() ? it->second : nullptr;
            }

            /// Summarizes every function, iterated to a fixed point. No set depends on what
            /// functions leave running, and each only grows.
            void summarizeHandles()
            {
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (auto& [function, data] : functions_)
                    {
                        const llvm::DominatorTree& dominators =
                            analyses_.getDominatorTree(*function);
                        auto onEveryPath = [&](const JoinSuccess& success)
                        {
                            for (const llvm::Instruction* exit : data.returns)
                            {
                                if (!success.covers(*exit, dominators))
                                    return false;
                            }
                            return true;
                        };
                        HandleSummary& summary = data.summary;
                        for (const SpawnSite& spawn : data.sites.spawns)
                            changed = summary.started.insert(spawn.handleGroupId).second || changed;
                        for (const AdoptionSite& adoption : data.sites.adoptions)
                            changed =
                                summary.started.insert(adoption.handleGroupId).second || changed;

                        for (const JoinSite& join : data.sites.joins)
                        {
                            if (!isNameableHandle(join.handleGroupId))
                                continue;
                            if (returnsResultOf(*join.instruction))
                                changed =
                                    summary.reported.insert(join.handleGroupId).second || changed;
                            if (join.success.has_value() && onEveryPath(*join.success))
                                changed =
                                    summary.joined.insert(join.handleGroupId).second || changed;
                        }

                        for (const DirectCallSite* site : data.calls)
                        {
                            // A copy: a recursive call reads the summary being extended.
                            const HandleSummary callee = summaryOf(*site);
                            for (const std::string& calleeHandle : callee.started)
                            {
                                if (std::optional<std::string> handle = handleAtCaller(
                                        calleeHandle, site->calleeFunctionId, *site->call))
                                {
                                    changed = summary.started.insert(std::move(*handle)).second ||
                                              changed;
                                }
                            }

                            // The call is the join it reports, so its result is that join's status.
                            const std::optional<JoinSuccess> reportedSuccess =
                                callee.reported.empty()
                                    ? std::nullopt
                                    : joinSuccessOf(*site->call, CallKind::PThreadJoin);
                            for (const std::string& calleeHandle : callee.reported)
                            {
                                const std::optional<std::string> handle = handleAtCaller(
                                    calleeHandle, site->calleeFunctionId, *site->call);
                                if (!handle.has_value())
                                    continue;
                                if (returnsResultOf(*site->call))
                                    changed = summary.reported.insert(*handle).second || changed;
                                if (reportedSuccess.has_value() && onEveryPath(*reportedSuccess))
                                    changed = summary.joined.insert(*handle).second || changed;
                            }

                            if (!onEveryPath(JoinSuccess{.after = site->call}))
                                continue;
                            for (const std::string& calleeHandle : callee.joined)
                            {
                                if (std::optional<std::string> handle = handleAtCaller(
                                        calleeHandle, site->calleeFunctionId, *site->call))
                                {
                                    changed =
                                        summary.joined.insert(std::move(*handle)).second || changed;
                                }
                            }
                        }
                    }
                }
            }

            /// Calls in `function` that join `handle`, with where they have: after a call to a
            /// function that joins what it is passed on every path out, and where the caller finds
            /// the status a call reports successful.
            [[nodiscard]] const std::vector<std::pair<const llvm::Instruction*, JoinSuccess>>&
            joiningCalls(const llvm::Function& function, const std::string& handle) const
            {
                const auto key = std::pair{&function, handle};
                if (const auto cached = joiningCalls_.find(key); cached != joiningCalls_.end())
                    return cached->second;

                std::vector<std::pair<const llvm::Instruction*, JoinSuccess>> calls;
                for (const DirectCallSite* site : functions_.at(&function).calls)
                {
                    auto passes = [&](const std::set<std::string>& calleeHandles)
                    {
                        for (const std::string& calleeHandle : calleeHandles)
                        {
                            if (handleAtCaller(calleeHandle, site->calleeFunctionId, *site->call) ==
                                handle)
                                return true;
                        }
                        return false;
                    };
                    if (passes(summaryOf(*site).joined))
                        calls.emplace_back(site->call, JoinSuccess{.after = site->call});
                    else if (passes(summaryOf(*site).reported))
                    {
                        if (std::optional<JoinSuccess> success =
                                joinSuccessOf(*site->call, CallKind::PThreadJoin))
                            calls.emplace_back(site->call, *success);
                    }
                }
                return joiningCalls_.emplace(key, std::move(calls)).first->second;
            }

            /// Whether a call joining `handle` has certainly returned at `point`, after `start`: a
            /// join before the thread is started ends an earlier one. A null `start` stands for a
            /// thread already running when `function` was called.
            [[nodiscard]] bool joinedThroughCallAt(const llvm::Function& function,
                                                   const std::string& handle,
                                                   const llvm::Instruction& point,
                                                   const llvm::Instruction* start) const
            {
                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                for (const auto& [call, success] : joiningCalls(function, handle))
                {
                    if (success.covers(point, dominators) &&
                        (start == nullptr || dominators.dominates(start, call)))
                        return true;
                }
                return false;
            }

            /// Whether a join of `handle` in `function`, in place or through a call, has certainly
            /// completed at `point`, after `start` as `joinedThroughCallAt` has it.
            [[nodiscard]] bool joinedAt(const llvm::Function& function, const std::string& handle,
                                        const llvm::Instruction& point,
                                        const llvm::Instruction* start) const
            {
                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                for (const JoinSite& join : sitesOf(function).joins)
                {
                    if (join.handleGroupId == handle && join.success.has_value() &&
                        join.success->covers(point, dominators) &&
                        (start == nullptr || dominators.dominates(start, join.instruction)))
                        return true;
                }
                return joinedThroughCallAt(function, handle, point, start);
            }

            /// The handle a thread started in `function` can be matched on there: none when the
            /// path to it is not nameable, or when the same storage receives more than one thread,
            /// since a join then ends only the last.
            [[nodiscard]] std::optional<std::string> spawnHandle(const llvm::Function& function,
                                                                 const SpawnSite& spawn) const
            {
                if (!isNameableHandle(spawn.handleGroupId) ||
                    ambiguousIn(function, spawn.handleGroupId))
                {
                    return std::nullopt;
                }
                return spawn.handleGroupId;
            }

            /// Threads started or moved into `handle` in `function`, in place or through calls, and
            /// whether one of them is in a loop.
            [[nodiscard]] std::pair<unsigned, bool> startsInto(const llvm::Function& function,
                                                               const std::string& handle) const
            {
                const llvm::LoopInfo& loops = analyses_.getLoopInfo(function);
                unsigned starts = 0;
                bool inLoop = false;
                auto count = [&](const llvm::Instruction& site)
                {
                    ++starts;
                    inLoop = inLoop || loops.getLoopFor(site.getParent()) != nullptr;
                };

                for (const SpawnSite& spawn : sitesOf(function).spawns)
                {
                    if (spawn.handleGroupId == handle)
                        count(*spawn.instruction);
                }
                for (const AdoptionSite& adoption : sitesOf(function).adoptions)
                {
                    if (adoption.handleGroupId == handle)
                        count(*adoption.instruction);
                }
                for (const DirectCallSite* site : functions_.at(&function).calls)
                {
                    for (const std::string& calleeHandle : summaryOf(*site).started)
                    {
                        if (handleAtCaller(calleeHandle, site->calleeFunctionId, *site->call) ==
                            handle)
                        {
                            count(*site->call);
                            break;
                        }
                    }
                }
                return {starts, inLoop};
            }

            [[nodiscard]] bool ambiguousIn(const llvm::Function& function,
                                           const std::string& handle) const
            {
                const auto [starts, inLoop] = startsInto(function, handle);
                return starts > 1 || inLoop;
            }

            /// What `function` leaves running when it returns, named in its own frame. A thread it
            /// starts leaves with it when some return still has it running. A thread a call leaves
            /// running does when a return it reaches has not joined it; without a handle to follow,
            /// it does unless the function joins anything at all, as before handles were followed.
            [[nodiscard]] ThreadInstances leakedFrom(const llvm::Function& function) const
            {
                ThreadInstances leaked;
                const FunctionThreads& data = functions_.at(&function);
                for (const SpawnSite& spawn : data.sites.spawns)
                {
                    bool running = false;
                    for (const llvm::Instruction* exit : data.returns)
                        running = running || spawnIsLiveAt(function, spawn, *exit);
                    if (running)
                        leaked.insert(
                            ThreadInstance{spawn.entryFunctionId, spawnHandle(function, spawn)});
                }

                for (const DirectCallSite* site : data.calls)
                {
                    const auto calleeLeaks = leaked_.find(functionOf(site->calleeFunctionId));
                    if (calleeLeaks == leaked_.end())
                        continue;

                    for (const ThreadInstance& instance : calleeLeaks->second)
                    {
                        std::optional<std::string> handle;
                        if (instance.handle.has_value())
                            handle = handleAtCaller(*instance.handle, site->calleeFunctionId,
                                                    *site->call);
                        if (handle.has_value() && ambiguousIn(function, *handle))
                            handle.reset();

                        const bool running =
                            handle.has_value()
                                ? reachesUnjoinedReturn(function, *site->call, *handle)
                                : data.sites.joins.empty();
                        if (running)
                            leaked.insert(ThreadInstance{instance.entryFunctionId, handle});
                    }
                }
                return leaked;
            }

            [[nodiscard]] bool reachesUnjoinedReturn(const llvm::Function& function,
                                                     const llvm::Instruction& start,
                                                     const std::string& handle) const
            {
                llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
                std::vector<const llvm::BasicBlock*> pending{start.getParent()};
                while (!pending.empty())
                {
                    const llvm::BasicBlock* block = pending.back();
                    pending.pop_back();
                    if (!visited.insert(block).second)
                        continue;
                    const llvm::Instruction* terminator = block->getTerminator();
                    if (llvm::isa<llvm::ReturnInst>(terminator) &&
                        !joinedAt(function, handle, *terminator, &start))
                        return true;
                    for (const llvm::BasicBlock* successor : llvm::successors(block))
                        pending.push_back(successor);
                }
                return false;
            }

            /// Once what functions leave running is settled, the threads each call starts in its
            /// caller, named in the caller's frame.
            void cacheIntroductions()
            {
                introducedByCalls_.clear();
                for (const auto& [function, data] : functions_)
                {
                    for (const DirectCallSite* site : data.calls)
                    {
                        const auto calleeLeaks = leaked_.find(functionOf(site->calleeFunctionId));
                        if (calleeLeaks == leaked_.end())
                            continue;
                        for (const ThreadInstance& instance : calleeLeaks->second)
                        {
                            ThreadInstance started{instance.entryFunctionId, std::nullopt};
                            if (instance.handle.has_value())
                                started.handle = handleAtCaller(
                                    *instance.handle, site->calleeFunctionId, *site->call);
                            if (started.handle.has_value() &&
                                ambiguousIn(*function, *started.handle))
                                started.handle.reset();
                            introducedByCalls_[function].emplace_back(site->call,
                                                                      std::move(started));
                        }
                    }
                }
            }

            const std::vector<DirectCallSite>& directCallSites_;
            const ThreadCompletionMap& completions_;
            LlvmFunctionAnalysisProvider& analyses_;
            std::unordered_map<std::string, const llvm::Function*> functionsById_;
            std::unordered_map<const llvm::Function*, FunctionThreads> functions_;
            std::unordered_map<const llvm::Function*, ThreadInstances> leaked_;
            std::unordered_map<const llvm::Function*, ThreadInstances> inherited_;
            std::unordered_map<const llvm::Function*,
                               std::vector<std::pair<const llvm::Instruction*, ThreadInstance>>>
                introducedByCalls_;
            mutable std::map<std::pair<const llvm::Function*, std::string>,
                             std::vector<std::pair<const llvm::Instruction*, JoinSuccess>>>
                joiningCalls_;
        };
    } // namespace

    TaskConcurrencyAnalyzer::TaskConcurrencyAnalyzer(const ConcurrencySymbolClassifier& classifier,
                                                     LlvmFunctionAnalysisProvider& analyses)
        : classifier_(classifier), analyses_(analyses)
    {
    }

    TaskConcurrencyResult TaskConcurrencyAnalyzer::analyze(
        const llvm::Module& module, const std::vector<DirectCallSite>& directCallSites,
        const ThreadCompletionMap& completions, bool includeExternalEntries) const
    {
        TaskConcurrencyResult result;
        result.rootTaskFunctions = collectRootTaskFunctions(module, directCallSites);
        const auto containedPairs = collectContainedEntryPairs(module, directCallSites, completions,
                                                               classifier_, analyses_);
        result.sequencedEntryPairs.insert(containedPairs.begin(), containedPairs.end());

        RunningThreads running(module, directCallSites, completions, classifier_, analyses_,
                               includeExternalEntries);

        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration())
                continue;

            const FunctionLifecycleSites& sites = running.sitesOf(function);
            if (sites.spawns.empty())
                continue;

            const llvm::DominatorTree& dominatorTree = analyses_.getDominatorTree(function);

            // Two instances of the same entry overlap when a spawn happens while a previous
            // instance is still live. Counting spawn sites instead would treat mutually exclusive
            // branches, or a spawn/join pair repeated sequentially, as concurrent.
            for (const SpawnSite& spawn : sites.spawns)
            {
                for (const SpawnSite& earlier : sites.spawns)
                {
                    if (earlier.entryFunctionId == spawn.entryFunctionId &&
                        running.spawnIsLiveAt(function, earlier, *spawn.instruction))
                    {
                        result.overlappingSpawnEntries.insert(spawn.entryFunctionId);
                    }
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
                        !running.spawnIsLiveAt(function, earlier, *spawn.instruction) &&
                        dominatorTree.dominates(earlier.instruction, spawn.instruction);
                    if (earlierIsJoinedFirst)
                    {
                        result.sequencedEntryPairs.insert(
                            makeEntryPair(earlier.entryFunctionId, spawn.entryFunctionId));
                    }
                }
            }
        }

        running.settleLeaks();
        for (const auto& [function, leaked] : running.leaked())
        {
            if (leaked.empty())
                continue;
            ThreadEntrySet& entries = result.entriesLeftRunningByFunction[functionId(*function)];
            for (const ThreadInstance& instance : leaked)
                entries.insert(instance.entryFunctionId);
        }

        running.settleInheritance();
        for (const llvm::Function& function : module)
        {
            if (function.isDeclaration() || !running.mayHoldThreads(function))
                continue;

            const llvm::DominatorTree& dominatorTree = analyses_.getDominatorTree(function);
            for (const llvm::BasicBlock& block : function)
            {
                if (!dominatorTree.isReachableFromEntry(&block))
                    continue;

                for (const llvm::Instruction& instruction : block)
                {
                    const ThreadInstances started = running.startedAt(function, instruction);
                    ThreadInstances live = running.inheritedAt(function, instruction);
                    live.insert(started.begin(), started.end());
                    if (!started.empty())
                        result.startedEntriesAtInstruction.emplace(&instruction,
                                                                   entriesOf(started));
                    if (!live.empty())
                        result.liveEntriesAtInstruction.emplace(&instruction, entriesOf(live));
                }
            }
        }

        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
