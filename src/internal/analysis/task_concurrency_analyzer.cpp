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
#include <llvm/IR/IntrinsicInst.h>
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

        /// Points past which a thread certainly no longer runs, for one flood.
        struct Kills
        {
            /// The thread runs neither at these instructions nor after them in their block.
            std::vector<const llvm::Instruction*> from;
            std::set<std::pair<const llvm::BasicBlock*, const llvm::BasicBlock*>> edges;

            /// Ended once `instruction` has returned: past a call, along an invoke's normal edge.
            void after(const llvm::Instruction& instruction)
            {
                if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&instruction))
                    edges.emplace(invoke->getParent(), invoke->getNormalDest());
                else
                    from.push_back(instruction.getNextNode());
            }

            void add(const JoinSuccess& success)
            {
                if (success.after != nullptr)
                    after(*success.after);
                else
                    edges.emplace(success.branch, success.successor);
            }
        };

        /// Where a thread may be running in the function holding it: every point some path from
        /// its start reaches without passing a point that certainly ended it.
        struct Reach
        {
            /// Blocks entered from their top, each with the first instruction the thread no
            /// longer runs at, or null when it runs through the whole block.
            std::unordered_map<const llvm::BasicBlock*, const llvm::Instruction*> fromTop;
            /// The start, or null for a thread already running when the function was called.
            const llvm::Instruction* start = nullptr;
            const llvm::Instruction* startEnd = nullptr;

            [[nodiscard]] static bool before(const llvm::Instruction& point,
                                             const llvm::Instruction* end)
            {
                return end == nullptr || point.comesBefore(end);
            }

            /// Blocks entered from their top, each with whether every path entering it had the
            /// thread still in its handle, and the points that fill that handle again.
            std::unordered_map<const llvm::BasicBlock*, bool> enteredNamed;
            llvm::SmallPtrSet<const llvm::Instruction*, 8> restarts;

            [[nodiscard]] bool fromStart(const llvm::Instruction& point) const
            {
                return start != nullptr && point.getParent() == start->getParent() &&
                       start->comesBefore(&point) && before(point, startEnd);
            }

            [[nodiscard]] bool contains(const llvm::Instruction& point) const
            {
                if (const auto it = fromTop.find(point.getParent());
                    it != fromTop.end() && before(point, it->second))
                    return true;
                return fromStart(point);
            }

            /// Whether the handle still holds the thread at `point` on every path reaching it:
            /// none fills it again between the start and `point`.
            [[nodiscard]] bool namedAt(const llvm::Instruction& point) const
            {
                auto stillNamed = [&](const llvm::Instruction* first, bool named)
                {
                    for (const llvm::Instruction* instruction = first;
                         named && instruction != &point; instruction = instruction->getNextNode())
                        named = !restarts.contains(instruction);
                    return named;
                };

                bool reached = false;
                bool named = true;
                if (fromStart(point))
                {
                    reached = true;
                    named = stillNamed(start->getNextNode(), true);
                }
                if (const auto it = fromTop.find(point.getParent());
                    it != fromTop.end() && before(point, it->second))
                {
                    reached = true;
                    named = named && stillNamed(&point.getParent()->front(),
                                                enteredNamed.at(point.getParent()));
                }
                return reached && named;
            }
        };

        /// What ends a thread along one flood. A join ends the thread its handle holds, so once
        /// the storage receives another thread (`restarts`), `byHandle` no longer applies there.
        struct FloodKills
        {
            Kills byHandle;
            Kills always;
            llvm::SmallPtrSet<const llvm::Instruction*, 8> restarts;
        };

        /// The value a condition operand stands for through -O0 spills: a local slot stored once,
        /// by a store dominating the load, reads back what was stored. Null when that value may
        /// differ between two evaluations: an instruction inside a loop.
        const llvm::Value* invariantOperand(const llvm::Value* value,
                                            const llvm::DominatorTree& dominators,
                                            const llvm::LoopInfo& loops)
        {
            for (unsigned steps = 0; steps < 8; ++steps)
            {
                if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                {
                    value = cast->getOperand(0);
                    continue;
                }
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                if (load == nullptr)
                    break;
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand());
                if (slot == nullptr)
                    return nullptr;
                const llvm::StoreInst* only = nullptr;
                for (const llvm::User* user : slot->users())
                {
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand() != slot || only != nullptr)
                            return nullptr;
                        only = store;
                    }
                    else if (!llvm::isa<llvm::LoadInst>(user) &&
                             !llvm::isa<llvm::DbgInfoIntrinsic>(user))
                        return nullptr;
                }
                if (only == nullptr || !dominators.dominates(only, load))
                    return nullptr;
                value = only->getValueOperand();
            }
            if (llvm::isa<llvm::Constant>(value) || llvm::isa<llvm::Argument>(value))
                return value;
            const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value);
            if (instruction == nullptr || loops.getLoopFor(instruction->getParent()) != nullptr ||
                llvm::isa<llvm::LoadInst>(instruction) || llvm::isa<llvm::PHINode>(instruction))
                return nullptr;
            return value;
        }

        /// A branch condition every evaluation of which in one call of the function gives the
        /// same value: a comparison of invariant operands, or an invariant value tested as it is,
        /// such as a `bool` kept in a local. Equal conditions get equal keys.
        std::optional<std::tuple<unsigned, const llvm::Value*, const llvm::Value*>>
        invariantCondition(const llvm::BranchInst& branch, const llvm::DominatorTree& dominators,
                           const llvm::LoopInfo& loops)
        {
            if (!branch.isConditional())
                return std::nullopt;
            const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(branch.getCondition());
            if (compare == nullptr)
            {
                const llvm::Value* tested =
                    invariantOperand(branch.getCondition(), dominators, loops);
                if (tested == nullptr)
                    return std::nullopt;
                return std::tuple{static_cast<unsigned>(llvm::CmpInst::BAD_ICMP_PREDICATE), tested,
                                  static_cast<const llvm::Value*>(nullptr)};
            }
            const llvm::Value* lhs = invariantOperand(compare->getOperand(0), dominators, loops);
            const llvm::Value* rhs = invariantOperand(compare->getOperand(1), dominators, loops);
            if (lhs == nullptr || rhs == nullptr)
                return std::nullopt;
            return std::tuple{static_cast<unsigned>(compare->getPredicate()), lhs, rhs};
        }

        /// One forward flood over the blocks of `function`, from just after `start` (from the
        /// entry when null), cut where `kills` end the thread.
        Reach flood(const llvm::Function& function, const llvm::Instruction* start,
                    const FloodKills& kills)
        {
            auto contains = [](const std::vector<const llvm::Instruction*>& list,
                               const llvm::Instruction* instruction)
            { return std::find(list.begin(), list.end(), instruction) != list.end(); };

            // Walks one block from `first`; the first instruction it no longer runs at, if any,
            // and whether its handle still names it past the block.
            auto walk = [&](const llvm::Instruction* first, bool named)
            {
                for (const llvm::Instruction* instruction = first; instruction != nullptr;
                     instruction = instruction->getNextNode())
                {
                    if (contains(kills.always.from, instruction) ||
                        (named && contains(kills.byHandle.from, instruction)))
                        return std::pair{instruction, named};
                    if (kills.restarts.contains(instruction))
                        named = false;
                }
                return std::pair<const llvm::Instruction*, bool>{nullptr, named};
            };

            Reach reach;
            reach.start = start;
            reach.restarts = kills.restarts;
            // A block is walked again once reached unnamed: no join ends the thread then, so it
            // runs at least as far.
            std::unordered_map<const llvm::BasicBlock*, bool> reachedNamed;
            std::vector<std::pair<const llvm::BasicBlock*, bool>> pending;
            auto leave = [&](const llvm::BasicBlock* block, bool named)
            {
                for (const llvm::BasicBlock* successor : llvm::successors(block))
                {
                    const std::pair edge{block, successor};
                    if (!kills.always.edges.contains(edge) &&
                        !(named && kills.byHandle.edges.contains(edge)))
                        pending.emplace_back(successor, named);
                }
            };

            if (start == nullptr)
                pending.emplace_back(&function.getEntryBlock(), true);
            else if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(start))
            {
                // A start that throws started nothing.
                pending.emplace_back(invoke->getNormalDest(), true);
            }
            else
            {
                const auto [end, named] = walk(start->getNextNode(), true);
                reach.startEnd = end;
                if (end == nullptr)
                    leave(start->getParent(), named);
            }

            while (!pending.empty())
            {
                const auto [block, named] = pending.back();
                pending.pop_back();
                const auto seen = reachedNamed.find(block);
                if (seen != reachedNamed.end() && (named || !seen->second))
                    continue;
                reachedNamed[block] = named;
                const auto [end, namedAfter] = walk(&block->front(), named);
                reach.fromTop[block] = end;
                reach.enteredNamed[block] = named;
                if (end == nullptr)
                    leave(block, namedAfter);
            }
            return reach;
        }

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

            /// Whether the thread started at `spawn` may still be running at `point`: some path
            /// from the spawn reaches the point without passing a join of its handle, in place or
            /// through a call, or the proven completion of the spawn.
            [[nodiscard]] bool spawnIsLiveAt(const llvm::Function& function, const SpawnSite& spawn,
                                             const llvm::Instruction& point) const
            {
                return spawnReach(function, spawn).contains(point);
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

                if (const auto calls = introducedByCalls_.find(&function);
                    calls != introducedByCalls_.end())
                {
                    for (const IntroducedThread& introduced : calls->second)
                    {
                        const Reach& reach =
                            handleReach(function, introduced.call, introduced.storage);
                        if (!reach.contains(point))
                            continue;
                        // Where its storage still holds it on every path, the thread is named by
                        // that storage, even one filled again elsewhere: a callee's join of it
                        // ends it.
                        ThreadInstance instance = introduced.instance;
                        if (!instance.handle.has_value() && introduced.storage.has_value() &&
                            reach.namedAt(point))
                            instance.handle = introduced.storage;
                        started.insert(std::move(instance));
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
                        if (handleReach(function, nullptr, instance.handle).contains(point))
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

            /// Edges a thread started at `start` never takes: the branches that led to the start
            /// compare invariant values, and a later branch comparing the same ones goes the same way.
            void cutCorrelatedEdges(const llvm::Function& function, const llvm::Instruction& start,
                                    Kills& kills) const
            {
                const llvm::DominatorTree& dominators = analyses_.getDominatorTree(function);
                const llvm::LoopInfo& loops = analyses_.getLoopInfo(function);
                using Condition = std::tuple<unsigned, const llvm::Value*, const llvm::Value*>;
                std::vector<std::pair<Condition, unsigned>> known;
                std::vector<std::pair<const llvm::BranchInst*, Condition>> branches;
                for (const llvm::BasicBlock& block : function)
                {
                    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
                    if (branch == nullptr)
                        continue;
                    const auto condition = invariantCondition(*branch, dominators, loops);
                    if (!condition.has_value())
                        continue;
                    branches.emplace_back(branch, *condition);
                    for (unsigned taken = 0; taken < 2; ++taken)
                    {
                        const llvm::BasicBlockEdge edge(&block, branch->getSuccessor(taken));
                        if (branch->getSuccessor(0) != branch->getSuccessor(1) &&
                            dominators.dominates(edge, start.getParent()))
                            known.emplace_back(*condition, taken);
                    }
                }
                for (const auto& [branch, condition] : branches)
                {
                    for (const auto& [value, taken] : known)
                    {
                        if (value == condition)
                            kills.edges.emplace(branch->getParent(),
                                                branch->getSuccessor(1 - taken));
                    }
                }
            }

            /// Joins of `handle` in `function`, in place or through a call.
            [[nodiscard]] Kills joinsOf(const llvm::Function& function,
                                        const std::string& handle) const
            {
                Kills kills;
                for (const JoinSite& join : sitesOf(function).joins)
                {
                    if (join.handleGroupId == handle && join.success.has_value())
                        kills.add(*join.success);
                }
                for (const auto& [call, success] : joiningCalls(function, handle))
                    kills.add(success);
                return kills;
            }

            [[nodiscard]] const Reach& spawnReach(const llvm::Function& function,
                                                  const SpawnSite& spawn) const
            {
                const auto key = std::tuple{spawn.instruction, true, std::string()};
                if (const auto cached = reaches_.find(key); cached != reaches_.end())
                    return cached->second;

                FloodKills kills;
                const std::optional<std::string> handle = spawnHandle(function, spawn);
                if (handle.has_value())
                    kills.byHandle = joinsOf(function, *handle);
                else
                {
                    // In place, a join ends the thread as long as its storage holds no other.
                    for (const JoinSite& join : sitesOf(function).joins)
                    {
                        if (join.handleGroupId == spawn.handleGroupId && join.success.has_value())
                            kills.byHandle.add(*join.success);
                    }
                }
                for (const llvm::Instruction* restart : startSites(function, spawn.handleGroupId))
                    kills.restarts.insert(restart);
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(spawn.instruction))
                {
                    if (const auto completion = completions_.find(call);
                        completion != completions_.end() && completion->second.ended.has_value())
                        kills.always.add(*completion->second.ended);
                }
                cutCorrelatedEdges(function, *spawn.instruction, kills.always);
                return reaches_.emplace(key, flood(function, spawn.instruction, kills))
                    .first->second;
            }

            /// Where a thread `start` leaves running, or one already running on entry when
            /// `start` is null, may still run: a join of its handle ends it.
            [[nodiscard]] const Reach& handleReach(const llvm::Function& function,
                                                   const llvm::Instruction* start,
                                                   const std::optional<std::string>& handle) const
            {
                const auto key =
                    std::tuple{start != nullptr ? start : &*function.getEntryBlock().begin(),
                               start == nullptr, handle.value_or("\n")};
                if (const auto cached = reaches_.find(key); cached != reaches_.end())
                    return cached->second;
                FloodKills kills;
                if (handle.has_value())
                {
                    kills.byHandle = joinsOf(function, *handle);
                    for (const llvm::Instruction* restart : startSites(function, *handle))
                        kills.restarts.insert(restart);
                }
                if (start != nullptr)
                    cutCorrelatedEdges(function, *start, kills.always);
                return reaches_.emplace(key, flood(function, start, kills)).first->second;
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

            /// Where `function` starts or moves a thread into `handle`, in place or through calls.
            [[nodiscard]] std::vector<const llvm::Instruction*>
            startSites(const llvm::Function& function, const std::string& handle) const
            {
                std::vector<const llvm::Instruction*> sites;
                for (const SpawnSite& spawn : sitesOf(function).spawns)
                {
                    if (spawn.handleGroupId == handle)
                        sites.push_back(spawn.instruction);
                }
                for (const AdoptionSite& adoption : sitesOf(function).adoptions)
                {
                    if (adoption.handleGroupId == handle)
                        sites.push_back(adoption.instruction);
                }
                for (const DirectCallSite* site : functions_.at(&function).calls)
                {
                    for (const std::string& calleeHandle : summaryOf(*site).started)
                    {
                        if (handleAtCaller(calleeHandle, site->calleeFunctionId, *site->call) ==
                            handle)
                        {
                            sites.push_back(site->call);
                            break;
                        }
                    }
                }
                return sites;
            }

            /// Threads started or moved into `handle` in `function`, in place or through calls, and
            /// whether one of them is in a loop.
            [[nodiscard]] std::pair<unsigned, bool> startsInto(const llvm::Function& function,
                                                               const std::string& handle) const
            {
                const llvm::LoopInfo& loops = analyses_.getLoopInfo(function);
                const std::vector<const llvm::Instruction*> sites = startSites(function, handle);
                bool inLoop = false;
                for (const llvm::Instruction* site : sites)
                    inLoop = inLoop || loops.getLoopFor(site->getParent()) != nullptr;
                return {static_cast<unsigned>(sites.size()), inLoop};
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
                    if (reachesReturn(spawnReach(function, spawn), data.returns))
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
                                ? reachesReturn(handleReach(function, site->call, handle),
                                                data.returns)
                                : data.sites.joins.empty();
                        if (running)
                            leaked.insert(ThreadInstance{instance.entryFunctionId, handle});
                    }
                }
                return leaked;
            }

            [[nodiscard]] static bool
            reachesReturn(const Reach& reach, const std::vector<const llvm::Instruction*>& returns)
            {
                for (const llvm::Instruction* exit : returns)
                {
                    if (reach.contains(*exit))
                        return true;
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
                            IntroducedThread introduced{
                                .call = site->call,
                                .instance = {instance.entryFunctionId, std::nullopt},
                            };
                            if (instance.handle.has_value())
                                introduced.storage = handleAtCaller(
                                    *instance.handle, site->calleeFunctionId, *site->call);
                            // Storage filled more than once names no single thread, but a join
                            // of it still ends the thread it holds until it is filled again,
                            // as for a thread started there in place.
                            if (introduced.storage.has_value() &&
                                !ambiguousIn(*function, *introduced.storage))
                                introduced.instance.handle = introduced.storage;
                            introducedByCalls_[function].push_back(std::move(introduced));
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
            /// A thread a call leaves running in its caller: the instance as the caller names it,
            /// and the storage holding it there, which a join ends it through even when that
            /// storage receives more than one thread and so names none of them.
            struct IntroducedThread
            {
                const llvm::Instruction* call = nullptr;
                ThreadInstance instance;
                std::optional<std::string> storage;
            };
            std::unordered_map<const llvm::Function*, std::vector<IntroducedThread>>
                introducedByCalls_;
            mutable std::map<std::pair<const llvm::Function*, std::string>,
                             std::vector<std::pair<const llvm::Instruction*, JoinSuccess>>>
                joiningCalls_;
            /// Keyed by the start (the entry's first instruction for a thread already running),
            /// whether it is that entry, and the handle a join ends it through.
            mutable std::map<std::tuple<const llvm::Instruction*, bool, std::string>, Reach>
                reaches_;
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

            // Two instances of an entry overlap when one is started while another may still run:
            // started earlier in the function or in a previous round of its loop, left running by
            // a call, or running in a caller, as when a helper that starts the thread is called
            // twice or in a loop. Counting spawn sites instead would take mutually exclusive
            // branches, or a start and a join repeated in sequence, as concurrent, and a helper
            // called several times as one instance.
            std::unordered_map<const llvm::Instruction*, const std::string*> spawnedEntries;
            for (const SpawnSite& spawn : running.sitesOf(function).spawns)
                spawnedEntries.emplace(spawn.instruction, &spawn.entryFunctionId);

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
                    const ThreadEntrySet liveEntries = entriesOf(live);
                    if (const auto spawned = spawnedEntries.find(&instruction);
                        spawned != spawnedEntries.end() && liveEntries.contains(*spawned->second))
                        result.overlappingSpawnEntries.insert(*spawned->second);
                    if (!started.empty())
                        result.startedEntriesAtInstruction.emplace(&instruction,
                                                                   entriesOf(started));
                    if (!liveEntries.empty())
                        result.liveEntriesAtInstruction.emplace(&instruction, liveEntries);
                }
            }
        }

        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
