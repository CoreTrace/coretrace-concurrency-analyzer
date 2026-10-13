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
#include <unordered_set>
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

        /// The entry a thread runs when its spawn takes it from parameter `index` of `function`:
        /// whatever each call passes there, settled at the calls.
        std::string parameterEntryId(const std::string& function, unsigned index)
        {
            return "<entry-parameter " + std::to_string(index) + " of " + function + ">";
        }

        bool isParameterEntry(const std::string& entry)
        {
            return entry.starts_with("<entry-parameter ");
        }

        std::optional<unsigned> parameterEntryIndex(const std::string& entry,
                                                    const std::string& function)
        {
            const std::string prefix = "<entry-parameter ";
            const std::string suffix = " of " + function + ">";
            if (!entry.starts_with(prefix) || !entry.ends_with(suffix))
                return std::nullopt;
            return static_cast<unsigned>(std::stoul(
                entry.substr(prefix.size(), entry.size() - prefix.size() - suffix.size())));
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

                    const std::optional<FunctionBinding> entry =
                        resolveFunctionBinding(*call->getArgOperand(*entryIndex));
                    if (!entry.has_value())
                        continue;
                    std::string entryId;
                    if (entry->function != nullptr)
                    {
                        if (entry->function->isDeclaration() && !includeExternalEntries)
                            continue;
                        entryId = functionId(*entry->function);
                    }
                    else if (entry->argumentIndex.has_value())
                        entryId = parameterEntryId(functionId(function), *entry->argumentIndex);
                    else
                        continue;

                    sites.spawns.push_back(SpawnSite{
                        .instruction = &instruction,
                        .handleGroupId = *handleGroupId,
                        .entryFunctionId = std::move(entryId),
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

        /// Two entries a function runs at once, one started while the other may run, named in
        /// that function's frame: its id, then the two entries.
        using EntryOverlap = std::tuple<std::string, std::string, std::string>;

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
                : directCallSites_(directCallSites), completions_(completions),
                  classifier_(classifier), analyses_(analyses),
                  includeExternalEntries_(includeExternalEntries),
                  selfContainedConstructors_(selfContainedThreadConstructors(module, classifier))
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

            /// Settles the entries each function starts before it returns, itself or through the
            /// functions it calls, named in its own frame, whether or not they still run then.
            void settleStarts()
            {
                for (const auto& [function, data] : functions_)
                    starts_[function];
                bool changed = true;
                while (changed)
                {
                    changed = false;
                    for (const auto& [function, data] : functions_)
                    {
                        ThreadEntrySet& starts = starts_.at(function);
                        for (const SpawnSite& spawn : data.sites.spawns)
                            changed = starts.insert(spawn.entryFunctionId).second || changed;
                        for (const DirectCallSite* site : data.calls)
                        {
                            const auto callee = starts_.find(functionOf(site->calleeFunctionId));
                            if (callee == starts_.end())
                                continue;
                            for (const std::string& entry :
                                 atCaller(*function, *site, callee->second))
                                changed = starts.insert(entry).second || changed;
                        }
                    }
                }
            }

            /// The entries `point` starts while `instance`, running in `function`, may still run,
            /// named in the frame of `function`: a call starts them before it returns, at points
            /// its callee has not joined that thread by.
            [[nodiscard]] ThreadEntrySet startedWhileRunning(const llvm::Function& function,
                                                             const llvm::Instruction& point,
                                                             const ThreadInstance& instance) const
            {
                for (const DirectCallSite* site : functions_.at(&function).calls)
                {
                    if (site->call != &point)
                        continue;
                    const llvm::Function* callee = functionOf(site->calleeFunctionId);
                    if (callee == nullptr || !functions_.contains(callee))
                        return {};
                    std::optional<std::string> handle;
                    if (instance.handle.has_value())
                        handle =
                            handleAtCallee(*instance.handle, *site->call, site->calleeFunctionId);
                    return atCaller(function, *site, startedWhileHeld(*callee, handle));
                }
                return {};
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
                // A call proven to leave its threads in a vector this function joins ends them
                // there, as a start proven so does.
                if (const auto* call = llvm::dyn_cast_or_null<llvm::CallBase>(start);
                    call != nullptr && completionCovers(function, *call, handle))
                {
                    if (const auto completion = completions_.find(call);
                        completion != completions_.end() && completion->second.ended.has_value())
                        kills.always.add(*completion->second.ended);
                }
                if (start != nullptr)
                    cutCorrelatedEdges(function, *start, kills.always);
                return reaches_.emplace(key, flood(function, start, kills)).first->second;
            }

            /// Whether the proven completion of `call` ends a thread it leaves running in
            /// `function`, held at `handle` there. A call that creates no thread is proven to leave
            /// no thread but those its completion ends. A creation's completion ends the thread it
            /// creates, held in the object it constructs: the caller names where a callee holds a
            /// thread only through the parameter that thread was started into, so storage in that
            /// object, or in a member such as the one holding libc++'s native handle, comes from
            /// a start handed the object, the constructor's own. A thread the copy of an argument
            /// starts is handed no such parameter: it is held elsewhere, or nowhere this function
            /// can name, and may still run.
            [[nodiscard]] bool completionCovers(const llvm::Function& function,
                                                const llvm::CallBase& call,
                                                const std::optional<std::string>& handle) const
            {
                const CallKind kind = classifier_.classify(call);
                if (kind != CallKind::StdThreadCtor && kind != CallKind::PThreadCreate)
                    return true;
                if (!handle.has_value() || call.arg_empty())
                    return false;
                const std::optional<std::string> object =
                    canonicalStorageGroupId(*call.getArgOperand(0));
                return object.has_value() &&
                       (*handle == *object || handle->starts_with(*object + "["));
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
                    // A spawn's own body starts the thread the spawn already counts.
                    if (isSpawn(function, *site->call))
                        continue;
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

            /// Whether `call` is itself one of `function`'s spawns, such as a `std::thread`
            /// constructor: the thread its body starts is the one the spawn already counts.
            [[nodiscard]] bool isSpawn(const llvm::Function& function,
                                       const llvm::CallBase& call) const
            {
                for (const SpawnSite& spawn : sitesOf(function).spawns)
                {
                    if (spawn.instruction == &call)
                        return true;
                }
                return false;
            }

            /// Whether every thread the callee of `site` starts is the one the call counts as a
            /// spawn of `function`: the callee is a `std::thread` constructor starting no other. A
            /// constructor that may, as the copy of an argument may, has all its threads counted,
            /// the spawn's own twice.
            [[nodiscard]] bool countedBySpawn(const llvm::Function& function,
                                              const DirectCallSite& site) const
            {
                return isSpawn(function, *site.call) &&
                       selfContainedConstructors_.contains(functionOf(site.calleeFunctionId));
            }

            /// `entries`, which the callee of `site` starts, named in the frame of `function`;
            /// none when the call counts them as a spawn.
            [[nodiscard]] ThreadEntrySet atCaller(const llvm::Function& function,
                                                  const DirectCallSite& site,
                                                  const ThreadEntrySet& entries) const
            {
                ThreadEntrySet named;
                if (countedBySpawn(function, site))
                    return named;
                for (const std::string& entry : entries)
                {
                    if (const std::optional<std::string> atCaller =
                            entryAtCaller(ThreadInstance{entry, std::nullopt}, site))
                        named.insert(*atCaller);
                }
                return named;
            }

            /// The entries `function` starts, itself or through its calls, while a thread it was
            /// entered with, held at `handle`, may still run: where it has not joined it yet.
            /// A recursion falls back on every entry the function starts.
            [[nodiscard]] ThreadEntrySet
            startedWhileHeld(const llvm::Function& function,
                             const std::optional<std::string>& handle) const
            {
                const auto key = std::pair{&function, handle.value_or("\n")};
                if (const auto known = startedWhileHeld_.find(key);
                    known != startedWhileHeld_.end())
                    return known->second;
                startedWhileHeld_.emplace(key, starts_.at(&function));
                ThreadEntrySet started;
                const Reach& reach = handleReach(function, nullptr, handle);
                const FunctionThreads& data = functions_.at(&function);
                for (const SpawnSite& spawn : data.sites.spawns)
                {
                    if (reach.contains(*spawn.instruction))
                        started.insert(spawn.entryFunctionId);
                }
                for (const DirectCallSite* site : data.calls)
                {
                    const llvm::Function* callee = functionOf(site->calleeFunctionId);
                    if (callee == nullptr || !functions_.contains(callee) ||
                        !reach.contains(*site->call))
                        continue;
                    std::optional<std::string> calleeHandle;
                    if (handle.has_value())
                        calleeHandle = handleAtCallee(*handle, *site->call, site->calleeFunctionId);
                    started.merge(
                        atCaller(function, *site, startedWhileHeld(*callee, calleeHandle)));
                }
                startedWhileHeld_[key] = started;
                return started;
            }

            /// The entry a thread a callee leaves running runs, as the caller names it: a named
            /// entry stays itself; an entry the callee took from a parameter is what `site`
            /// passes there, itself a parameter of the caller or a function. Nothing when the call
            /// passes something else.
            [[nodiscard]] std::optional<std::string> entryAtCaller(const ThreadInstance& instance,
                                                                   const DirectCallSite& site) const
            {
                const std::optional<unsigned> index =
                    parameterEntryIndex(instance.entryFunctionId, site.calleeFunctionId);
                if (!index.has_value())
                    return instance.entryFunctionId;
                if (*index >= site.call->arg_size())
                    return std::nullopt;
                const std::optional<FunctionBinding> passed =
                    resolveFunctionBinding(*site.call->getArgOperand(*index));
                if (!passed.has_value())
                    return std::nullopt;
                if (passed->function != nullptr)
                {
                    if (passed->function->isDeclaration() && !includeExternalEntries_)
                        return std::nullopt;
                    return functionId(*passed->function);
                }
                if (passed->argumentIndex.has_value())
                    return parameterEntryId(site.callerFunctionId, *passed->argumentIndex);
                return std::nullopt;
            }

          public:
            /// The entries `entries` may stand for: a function's parameter entry is every entry
            /// the calls to it pass there.
            [[nodiscard]] ThreadEntrySet concreteEntries(const ThreadEntrySet& entries) const
            {
                ThreadEntrySet concrete;
                std::vector<std::string> pending(entries.begin(), entries.end());
                std::unordered_set<std::string> seen;
                while (!pending.empty())
                {
                    const std::string entry = std::move(pending.back());
                    pending.pop_back();
                    if (!seen.insert(entry).second)
                        continue;
                    if (!entry.starts_with("<entry-parameter "))
                    {
                        concrete.insert(entry);
                        continue;
                    }
                    for (const DirectCallSite& site : directCallSites_)
                    {
                        if (site.call == nullptr)
                            continue;
                        if (const std::optional<std::string> passed =
                                entryAtCaller(ThreadInstance{entry, std::nullopt}, site);
                            passed.has_value() && *passed != entry)
                            pending.push_back(*passed);
                    }
                }
                return concrete;
            }

            /// The entries running twice at once in some context, from `overlaps`: pairs of
            /// entries a function runs together, one started while the other may run. A pair
            /// naming a parameter's entry is followed through each call to the function, which
            /// substitutes what it passes into both: it may overlap before the function returns.
            [[nodiscard]] ThreadEntrySet
            overlappingAtCallers(const std::set<EntryOverlap>& overlaps) const
            {
                ThreadEntrySet overlapping;
                std::vector<EntryOverlap> pending(overlaps.begin(), overlaps.end());
                std::set<EntryOverlap> seen(overlaps.begin(), overlaps.end());
                while (!pending.empty())
                {
                    const auto [function, first, second] = std::move(pending.back());
                    pending.pop_back();
                    if (!isParameterEntry(first) && !isParameterEntry(second))
                    {
                        if (first == second)
                            overlapping.insert(first);
                        continue;
                    }
                    for (const DirectCallSite& site : directCallSites_)
                    {
                        if (site.call == nullptr || site.calleeFunctionId != function)
                            continue;
                        const std::optional<std::string> firstAtCaller =
                            entryAtCaller(ThreadInstance{first, std::nullopt}, site);
                        const std::optional<std::string> secondAtCaller =
                            entryAtCaller(ThreadInstance{second, std::nullopt}, site);
                        if (!firstAtCaller.has_value() || !secondAtCaller.has_value())
                            continue;
                        EntryOverlap atCaller{site.callerFunctionId, *firstAtCaller,
                                              *secondAtCaller};
                        if (seen.insert(atCaller).second)
                            pending.push_back(std::move(atCaller));
                    }
                }
                return overlapping;
            }

          private:
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
                    if (calleeLeaks == leaked_.end() || countedBySpawn(function, *site))
                        continue;

                    for (const ThreadInstance& instance : calleeLeaks->second)
                    {
                        const std::optional<std::string> entry = entryAtCaller(instance, *site);
                        if (!entry.has_value())
                            continue;
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
                            leaked.insert(ThreadInstance{*entry, handle});
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
                        if (calleeLeaks == leaked_.end() || countedBySpawn(*function, *site))
                            continue;
                        for (const ThreadInstance& instance : calleeLeaks->second)
                        {
                            const std::optional<std::string> entry = entryAtCaller(instance, *site);
                            if (!entry.has_value())
                                continue;
                            IntroducedThread introduced{
                                .call = site->call,
                                .instance = {*entry, std::nullopt},
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
            const ConcurrencySymbolClassifier& classifier_;
            LlvmFunctionAnalysisProvider& analyses_;
            bool includeExternalEntries_ = false;
            std::unordered_set<const llvm::Function*> selfContainedConstructors_;
            std::unordered_map<std::string, const llvm::Function*> functionsById_;
            std::unordered_map<const llvm::Function*, FunctionThreads> functions_;
            std::unordered_map<const llvm::Function*, ThreadInstances> leaked_;
            std::unordered_map<const llvm::Function*, ThreadInstances> inherited_;
            std::unordered_map<const llvm::Function*, ThreadEntrySet> starts_;
            mutable std::map<std::pair<const llvm::Function*, std::string>, ThreadEntrySet>
                startedWhileHeld_;
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
            for (const std::string& entry : running.concreteEntries(entriesOf(leaked)))
                entries.insert(entry);
        }

        running.settleInheritance();
        running.settleStarts();
        std::set<EntryOverlap> overlaps;
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
                    const ThreadEntrySet liveEntries = running.concreteEntries(entriesOf(live));
                    // An entry a spawn takes from a parameter is the one each call passes: the
                    // check is made at the calls, where it is known. A thread this function
                    // starts, itself or through a call, while another it started may run is
                    // paired with it in this frame, whether or not either outlasts the function,
                    // and each call to it substitutes what it passes into both. Threads running in
                    // a caller meet the call there, through what it starts before joining them.
                    const auto spawned = spawnedEntries.find(&instruction);
                    if (spawned != spawnedEntries.end() && !isParameterEntry(*spawned->second) &&
                        liveEntries.contains(*spawned->second))
                        result.overlappingSpawnEntries.insert(*spawned->second);
                    for (const ThreadInstance& instance : started)
                    {
                        ThreadEntrySet starting =
                            running.startedWhileRunning(function, instruction, instance);
                        if (spawned != spawnedEntries.end())
                            starting.insert(*spawned->second);
                        for (const std::string& entry : starting)
                            overlaps.emplace(functionId(function), entry, instance.entryFunctionId);
                    }
                    if (!started.empty())
                        result.startedEntriesAtInstruction.emplace(
                            &instruction, running.concreteEntries(entriesOf(started)));
                    if (!liveEntries.empty())
                        result.liveEntriesAtInstruction.emplace(&instruction, liveEntries);
                }
            }
        }

        const ThreadEntrySet overlapping = running.overlappingAtCallers(overlaps);
        result.overlappingSpawnEntries.insert(overlapping.begin(), overlapping.end());
        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
