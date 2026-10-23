// SPDX-License-Identifier: Apache-2.0
#include "thread_completion_analysis.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "held_member_analysis.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/STLFunctionalExtras.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CheckedArithmetic.h>

#include <cstdint>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        const llvm::Value* stripIntegerCasts(const llvm::Value* value)
        {
            while (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
            {
                if (cast->getOpcode() != llvm::Instruction::SExt &&
                    cast->getOpcode() != llvm::Instruction::ZExt)
                    break;
                value = cast->getOperand(0);
            }
            return value;
        }

        const llvm::StoreInst* uniqueStore(const llvm::Value* slot)
        {
            const llvm::StoreInst* result = nullptr;
            for (const llvm::User* user : slot->users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand() != slot || result != nullptr)
                        return nullptr;
                    result = store;
                }
                else if (!llvm::isa<llvm::LoadInst>(user) &&
                         !llvm::isa<llvm::DbgInfoIntrinsic>(user))
                    return nullptr;
            }
            return result;
        }

        bool isZero(const llvm::Value* value)
        {
            const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value);
            return constant != nullptr && constant->isZero();
        }

        /// Where `user` finds that the join succeeded, when it compares `result` with zero for a
        /// branch and nothing else.
        std::optional<JoinSuccess> successTestedBy(const llvm::User& user,
                                                   const llvm::Value& result)
        {
            const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(&user);
            if (compare == nullptr || !compare->isEquality() || !compare->hasOneUse())
                return std::nullopt;

            const llvm::Value* other =
                compare->getOperand(0) == &result ? compare->getOperand(1) : compare->getOperand(0);
            const auto* branch = llvm::dyn_cast<llvm::BranchInst>(*compare->user_begin());
            if (!isZero(other) || branch == nullptr || !branch->isConditional())
                return std::nullopt;

            const bool successWhenTrue = compare->getPredicate() == llvm::CmpInst::ICMP_EQ;
            return JoinSuccess{
                .branch = branch->getParent(),
                .successor = branch->getSuccessor(successWhenTrue ? 0 : 1),
            };
        }

        /// The local `store` writes to, when that store is its only write and nothing takes its
        /// address.
        const llvm::AllocaInst* localWrittenOnlyBy(const llvm::StoreInst& store)
        {
            const auto* local = llvm::dyn_cast<llvm::AllocaInst>(store.getPointerOperand());
            if (local == nullptr)
                return nullptr;
            for (const llvm::User* user : local->users())
            {
                if (user != &store && !llvm::isa<llvm::LoadInst>(user))
                    return nullptr;
            }
            return local;
        }

        /// Whether `value` reads `result` back from a local only the store of `result` writes. A
        /// defined program reads such a local only once that store has run.
        bool readsBack(const llvm::Value* value, const llvm::Value& result)
        {
            const auto* load = llvm::dyn_cast_or_null<llvm::LoadInst>(value);
            if (load == nullptr)
                return false;
            for (const llvm::User* user : result.users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store != nullptr && store->getValueOperand() == &result &&
                    store->getPointerOperand() == load->getPointerOperand() &&
                    localWrittenOnlyBy(*store) != nullptr)
                    return true;
            }
            return false;
        }

        /// Only immutable local copies are forwarded. Reaching through a mutable bound
        /// would incorrectly equate a complete join loop with a shortened one.
        const llvm::Value* invariantValue(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 8> visited;
            while (visited.insert(value).second)
            {
                value = stripIntegerCasts(value);
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                if (load == nullptr || !llvm::isa<llvm::AllocaInst>(load->getPointerOperand()))
                    return value;
                const auto* store = uniqueStore(load->getPointerOperand());
                if (store == nullptr)
                    return value;
                value = store->getValueOperand();
            }
            return nullptr;
        }

        /// The value the pointer `address` is a constant number of bytes past, and that number,
        /// `extra` added: through the locals unoptimized code keeps a pointer in, as
        /// `invariantValue` follows them, and the in-bounds offsets the data layout computes
        /// exactly. Any step LLVM cannot compute exactly stops there, so the value found is the
        /// pointer that step produces; a sum that overflows, or locals forwarding a pointer around
        /// a cycle, give nothing.
        std::optional<std::pair<const llvm::Value*, std::int64_t>>
        constantOffsetFrom(const llvm::Value& address, std::int64_t extra,
                           const llvm::DataLayout& layout)
        {
            if (!address.getType()->isPointerTy())
                return std::nullopt;
            llvm::APInt offset(layout.getIndexTypeSizeInBits(address.getType()),
                               static_cast<std::uint64_t>(extra), /*isSigned=*/true);
            const llvm::Value* value = &address;
            llvm::SmallPtrSet<const llvm::Value*, 8> visited;
            while (true)
            {
                if (!visited.insert(value).second)
                    return std::nullopt;
                const llvm::Value* forwarded = invariantValue(value);
                if (forwarded == nullptr || !forwarded->getType()->isPointerTy())
                    return std::nullopt;
                llvm::APInt step(offset.getBitWidth(), 0);
                const llvm::Value* base = forwarded->stripAndAccumulateConstantOffsets(
                    layout, step, /*AllowNonInbounds=*/false);
                bool overflow = false;
                offset = offset.sadd_ov(step, overflow);
                if (overflow)
                    return std::nullopt;
                if (base == value)
                    break;
                value = base;
            }
            const std::optional<std::int64_t> bytes = offset.trySExtValue();
            if (!bytes)
                return std::nullopt;
            return std::pair{value, *bytes};
        }

        /// A call that waits for the thread whose handle is one of its operands: a join, or a
        /// call to a helper that joins that parameter on every normal return.
        struct JoinSite
        {
            const llvm::CallBase* call = nullptr;
            unsigned operand = 0;
            /// A `pthread_t` travels by value, loaded from the storage its creation wrote; a
            /// `std::thread` is handed over by address.
            bool byValue = false;
            /// The call's result is the join's status: a `pthread_join`, or a helper reporting one.
            bool reportsStatus = false;
            /// Where the join has certainly waited, when its result proves it anywhere. The call
            /// joins the thread either way.
            std::optional<JoinSuccess> success;
            /// The thread, when it is not the operand itself: a call through a pointer-to-member is
            /// handed the object adjusted by the pair, the adjustment being proven nil.
            const llvm::Value* object = nullptr;
            /// Bytes past `handle()` where the handle joined lies, when the call joins a member of
            /// the object it is handed. Sites with a nonzero offset are listed only to the analyses
            /// asking for them: every other one reads `handle()` as the handle itself.
            std::int64_t offset = 0;

            [[nodiscard]] const llvm::Value& handle() const
            {
                return object != nullptr ? *object : *call->getArgOperand(operand);
            }
        };

        struct CountedLoop
        {
            const llvm::Value* induction = nullptr;
            const llvm::Value* begin = nullptr;
            const llvm::Value* end = nullptr;
            llvm::CmpInst::Predicate predicate = llvm::CmpInst::BAD_ICMP_PREDICATE;
            /// The store raising the counter by one, once a round.
            const llvm::StoreInst* increment = nullptr;
        };

        /// A bound of a slot range: a known index, or a value that only equals itself.
        struct SlotBound
        {
            std::optional<std::int64_t> index;
            const llvm::Value* value = nullptr;
        };

        /// The slots of one handle array that a spawn fills or a join loop visits: the handles
        /// `stride` bytes apart from `origin`, the byte offset of index 0 in `array`, for the
        /// indices in `[begin, end)` as a loop comparing with `predicate` runs over them.
        struct SlotRange
        {
            const llvm::Value* array = nullptr;
            std::int64_t origin = 0;
            std::int64_t stride = 0;
            SlotBound begin;
            SlotBound end;
            llvm::CmpInst::Predicate predicate = llvm::CmpInst::BAD_ICMP_PREDICATE;
        };

        /// The slots each spawn of a function fills, where its handle address names them, by the
        /// call storing the handle: the creation, or the move of a thread started into a local.
        using SpawnFills = std::unordered_map<const llvm::CallBase*, SlotRange>;

        bool atMost(const SlotBound& lower, const SlotBound& upper)
        {
            if (lower.index.has_value() && upper.index.has_value())
                return *lower.index <= *upper.index;
            return lower.value != nullptr && lower.value == upper.value;
        }

        /// Whether every slot `filled` names is one `visited` names.
        bool covers(const SlotRange& visited, const SlotRange& filled)
        {
            // A single slot (stride 0) is covered when the loop's grid of known bounds passes it.
            if (filled.stride == 0)
            {
                const auto shift = llvm::checkedSub(filled.origin, visited.origin);
                if (visited.array != filled.array || visited.stride <= 0 || !shift ||
                    *shift % visited.stride != 0 || !visited.begin.index || !visited.end.index)
                    return false;
                const std::int64_t slot = *shift / visited.stride;
                return *visited.begin.index <= slot && slot < *visited.end.index;
            }
            // Two loops bounded by one value run alike only when they compare with it alike: a
            // negative `n` stops a signed loop at once and sends an unsigned one far past it. A
            // count, filled by no comparison, holds only indices its spawn stored at.
            const bool known =
                visited.begin.index && visited.end.index && filled.begin.index && filled.end.index;
            const bool alike = known || visited.predicate == filled.predicate ||
                               filled.predicate == llvm::CmpInst::BAD_ICMP_PREDICATE;
            return visited.array == filled.array && visited.origin == filled.origin &&
                   visited.stride == filled.stride && alike &&
                   atMost(visited.begin, filled.begin) && atMost(filled.end, visited.end);
        }

        /// The byte offsets of the first and the last handle `range` names, when its bounds are
        /// known and it names any.
        std::optional<std::pair<std::int64_t, std::int64_t>> knownSpan(const SlotRange& range)
        {
            if (!range.begin.index || !range.end.index || *range.begin.index >= *range.end.index)
                return std::nullopt;
            const auto first = llvm::checkedMulAdd(range.stride, *range.begin.index, range.origin);
            const auto last = llvm::checkedMulAdd(range.stride, *range.end.index - 1, range.origin);
            if (!first || !last)
                return std::nullopt;
            return std::pair{*first, *last};
        }

        /// Whether two ranges of known bounds name no slot in common. Laid out on one grid, two
        /// handles meet only where they start together.
        bool disjoint(const SlotRange& first, const SlotRange& second)
        {
            const auto a = knownSpan(first);
            const auto b = knownSpan(second);
            // Handles of one array start apart unless they are the same handle.
            if (first.stride == 0 || second.stride == 0)
                return first.array == second.array && a && b &&
                       (a->second < b->first || b->second < a->first);
            const auto shift = llvm::checkedSub(first.origin, second.origin);
            return first.array == second.array && first.stride > 0 &&
                   first.stride == second.stride && shift && *shift % first.stride == 0 && a && b &&
                   (a->second < b->first || b->second < a->first);
        }

        /// Whether `other` stores its handle only into slots `writer` never fills.
        bool fillsApart(const llvm::CallBase& writer, const llvm::CallBase& other,
                        const SpawnFills* fills)
        {
            if (fills == nullptr)
                return false;
            const auto mine = fills->find(&writer);
            const auto theirs = fills->find(&other);
            return mine != fills->end() && theirs != fills->end() &&
                   disjoint(mine->second, theirs->second);
        }

        bool reachesNormalReturn(const llvm::BasicBlock* start)
        {
            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
            std::vector<const llvm::BasicBlock*> pending{start};
            while (!pending.empty())
            {
                const auto* block = pending.back();
                pending.pop_back();
                if (!visited.insert(block).second)
                    continue;
                if (llvm::isa<llvm::ReturnInst>(block->getTerminator()))
                    return true;
                for (const auto* successor : llvm::successors(block))
                    pending.push_back(successor);
            }
            return false;
        }

        bool noEarlyNormalExit(const llvm::Loop& loop)
        {
            for (const auto* block : loop.blocks())
                if (block != loop.getHeader())
                    for (const auto* successor : llvm::successors(block))
                        if (!loop.contains(successor) && reachesNormalReturn(successor))
                            return false;
            return true;
        }

        /// Blocks from which a normal return is reachable. The others only unwind, and a proof
        /// about normal returns says nothing about them (see `completionCoversReturns`).
        llvm::SmallPtrSet<const llvm::BasicBlock*, 32>
        blocksReachingNormalReturn(const llvm::Function& function)
        {
            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> reaching;
            std::vector<const llvm::BasicBlock*> pending;
            for (const auto& block : function)
                if (llvm::isa<llvm::ReturnInst>(block.getTerminator()))
                    pending.push_back(&block);
            while (!pending.empty())
            {
                const auto* block = pending.back();
                pending.pop_back();
                if (!reaching.insert(block).second)
                    continue;
                for (const auto* predecessor : llvm::predecessors(block))
                    pending.push_back(predecessor);
            }
            return reaching;
        }

        /// A local only ever loaded from and stored into: whatever it holds, nothing but its own
        /// loads hands on.
        bool isPointerLocal(const llvm::Value& value)
        {
            if (!llvm::isa<llvm::AllocaInst>(value))
                return false;
            for (const llvm::User* user : value.users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store != nullptr ? store->getPointerOperand() != &value
                                     : !llvm::isa<llvm::LoadInst>(user) &&
                                           !llvm::isa<llvm::DbgInfoIntrinsic>(user) &&
                                           !llvm::isa<llvm::LifetimeIntrinsic>(user))
                    return false;
            }
            return true;
        }

        /// Whether a demangled scope is the standard class `name`, whatever its template
        /// arguments, in `std` or in an inline namespace of it such as libc++'s `std::__1`.
        bool isStdClass(std::string_view scope, std::string_view name)
        {
            if (!scope.starts_with("std::"))
                return false;
            scope.remove_prefix(std::string_view("std::").size());
            if (scope.starts_with("__"))
            {
                const std::size_t inner = scope.find("::");
                if (inner == std::string_view::npos)
                    return false;
                scope.remove_prefix(inner + 2);
            }
            return scope.starts_with(name) &&
                   (scope.size() == name.size() || scope[name.size()] == '<');
        }

        /// Whether `call` builds an empty `std::thread`: its default constructor, the only one
        /// taking nothing but the object. It starts no thread and keeps no pointer to the object.
        bool buildsEmptyThread(const llvm::CallBase& call)
        {
            const llvm::Function* callee = call.getCalledFunction();
            if (callee == nullptr || call.arg_size() != 1 || !callee->getName().contains("thread"))
                return false;
            const std::optional<DemangledName> name = demangleFunction(callee->getName());
            return name.has_value() && name->base == "thread" &&
                   isStdClass(name->context, "thread");
        }

        /// A join a function makes through a pointer-to-member it is handed: `pair`, in the terms of
        /// the function, names the member `call` invokes on the thread. Whether that joins the
        /// thread depends on each caller's pair.
        struct MemberCondition
        {
            HeldPair pair;
            MemberCall call;

            friend bool operator==(const MemberCondition&, const MemberCondition&) = default;
        };

        /// The parameter a function invokes a member on, on every normal return, through a
        /// pointer-to-member it is handed or names itself. Whether that joins the parameter is told
        /// where the pair is resolved, at each call.
        struct MemberJoin
        {
            unsigned object = 0;
            MemberCondition member;

            friend bool operator==(const MemberJoin&, const MemberJoin&) = default;
        };

        /// The members each function of the unit invokes on a parameter through a pointer-to-member.
        /// They stay in the unit: unlike `JoiningHelpers`, what they join may depend on the caller,
        /// and no other unit is told about them.
        struct MemberJoins
        {
            const HeldMemberAnalysis& held;
            const JoiningHelpers& helpers;
            /// The functions not established to start no thread. A member among them joins nothing
            /// a caller can rely on: the analyses that follow a call do not follow one through a
            /// pointer-to-member, so a thread it starts would go unseen once its join is known.
            const std::unordered_set<const llvm::Function*>& possibleStarters;
            std::unordered_map<const llvm::Function*, std::vector<MemberJoin>> byFunction;
        };

        /// What a join is proven to do to the storage of the thread it waits for: nothing it does
        /// before its joins may change the storage, or the proof is refused. Past its last join it
        /// may still change it, which matters to a caller making another join after it: a write
        /// there may precede that join.
        enum class JoinProof
        {
            Refused,
            Unchanged,
            ChangedAfter,
        };

        JoinProof waitsForUnchangedHandle(const JoinSite& join, llvm::ArrayRef<unsigned> aliases,
                                          llvm::ArrayRef<const llvm::CallBase*> callers,
                                          const MemberJoins& members,
                                          LlvmFunctionAnalysisProvider& analyses,
                                          const ConcurrencySymbolClassifier& classifier);

        /// Whether `point` may run after `writer` and before `join`: `writer` may reach it, and it
        /// may reach `join`.
        bool between(const llvm::Instruction& writer, const llvm::Instruction& point,
                     const llvm::Instruction& join, const llvm::DominatorTree& dominators)
        {
            return llvm::isPotentiallyReachable(&writer, &point, nullptr, &dominators) &&
                   llvm::isPotentiallyReachable(&point, &join, nullptr, &dominators);
        }

        /// The handle `writer` stores into a local is still there when `join` reads it: nothing
        /// writes the local or exposes its address between them, another spawn storing into it
        /// only where `fills` shows it never meets `writer`'s slots. Where the handle lies in the
        /// local is not proven, so a write anywhere in it counts.
        ///
        /// Thread lifecycle calls keep their meaning: joins read the handle; creations, moves of a
        /// started thread and default constructions store one, `writer` among them; a
        /// std::thread destroyed past the join reads it. Any other call handed the local is judged
        /// from what its effects say it does there, and from where it runs: one that may write the
        /// local may not run between `writer` and `join` on any path, and one that may keep its
        /// address may not run before `join` on any path, since anything may write through the
        /// address later. A call or a store before `writer` that runs again before `join`, as in a
        /// loop, is between them. A pointer any of these calls returns may be the local's address:
        /// it may only be read through, compared, or handed to a join or a destructor.
        ///
        /// Two cases keep the refusal any such call met before effects were read: the local
        /// handed to a thread a creation starts, which may change the handle unseen, and a join
        /// made by a function not proven to wait for the thread it is handed unchanged.
        bool arrayStorageUnchanged(const llvm::CallBase& writer, const JoinSite& join,
                                   const SpawnFills* fills, const llvm::DominatorTree& dominators,
                                   const ConcurrencySymbolClassifier& classifier,
                                   const MemberJoins& members,
                                   LlvmFunctionAnalysisProvider& analyses)
        {
            const llvm::Value* base = llvm::getUnderlyingObject(writer.getArgOperand(0));
            if (!llvm::isa<llvm::AllocaInst>(base))
                return false;
            // The array's address may be kept in locals only loaded from (a range-for's cursor,
            // an aggregate initializer's cleanup cursor). A pointer read back from one is followed
            // too, but may only be read through, compared, or handed to a join or a destructor:
            // the second walk below does not see through those locals.
            std::vector<std::pair<const llvm::CallBase*, unsigned>> judged;
            std::vector<unsigned> joinAliases;
            bool handedToThread = false;
            std::vector<std::pair<const llvm::Value*, bool>> pointers{{base, false}};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!pointers.empty())
            {
                const auto [pointer, kept] = pointers.back();
                pointers.pop_back();
                if (!seen.insert(pointer).second)
                    continue;
                for (const llvm::Use& use : pointer->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (llvm::isa<llvm::GetElementPtrInst>(user) ||
                        llvm::isa<llvm::BitCastInst>(user) || llvm::isa<llvm::PHINode>(user) ||
                        llvm::isa<llvm::SelectInst>(user))
                        pointers.emplace_back(user, kept);
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getValueOperand() == pointer)
                        {
                            if (!isPointerLocal(*store->getPointerOperand()))
                                return false;
                            for (const auto* reader : store->getPointerOperand()->users())
                                if (llvm::isa<llvm::LoadInst>(reader))
                                    pointers.emplace_back(reader, true);
                        }
                        else if (kept)
                            return false;
                    }
                    else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(user))
                    {
                        const auto kind = classifier.classify(*call);
                        const bool reads = call == join.call || kind == CallKind::PThreadJoin ||
                                           kind == CallKind::StdThreadJoin ||
                                           kind == CallKind::StdThreadDtor ||
                                           llvm::isa<llvm::IntrinsicInst>(call);
                        const bool starts =
                            kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor;
                        const bool stores = call == &writer || starts || buildsEmptyThread(*call) ||
                                            (fills != nullptr && fills->contains(call));
                        // A creation handed the local as anything but the handle it fills gives
                        // the thread it starts the local's address.
                        if (starts &&
                            (!call->isArgOperand(&use) || call->getArgOperandNo(&use) != 0))
                            handedToThread = true;
                        // The join may be handed the local through more operands than its handle.
                        if (call == join.call && call->isArgOperand(&use))
                            joinAliases.push_back(call->getArgOperandNo(&use));
                        // A pointer the call returns may be the local's address, as a copy the
                        // second walk does not see through.
                        if (call->getType()->isPointerTy())
                            pointers.emplace_back(call, true);
                        if (reads)
                            continue;
                        if (kept)
                            return false;
                        if (!stores)
                        {
                            if (!call->isArgOperand(&use))
                                return false;
                            judged.emplace_back(call, call->getArgOperandNo(&use));
                        }
                    }
                    else if (!llvm::isa<llvm::LoadInst>(user) && !llvm::isa<llvm::ICmpInst>(user))
                        return false;
                }
            }
            if (!judged.empty() && handedToThread)
                return false;
            // A join made through a call handed the storage waits for the thread the handle holds
            // only when that call leaves the handle unchanged until its own joins, whether or not
            // anything else is handed the storage; one handed the handle's value cannot change it.
            // A change the join makes past its own joins follows every one of them, the join of the
            // handle proven among them.
            const CallKind joinKind = classifier.classify(*join.call);
            const bool callHandedStorage = joinKind != CallKind::PThreadJoin &&
                                           joinKind != CallKind::StdThreadJoin &&
                                           !joinAliases.empty();
            if ((!judged.empty() || callHandedStorage) &&
                waitsForUnchangedHandle(join, joinAliases, {}, members, analyses, classifier) ==
                    JoinProof::Refused)
                return false;
            for (const auto& [call, argument] : judged)
            {
                const ArgumentEffect effect = members.held.effectOn(*call, argument);
                if (effect.keeps &&
                    llvm::isPotentiallyReachable(call, join.call, nullptr, &dominators))
                    return false;
                if (effect.writes && between(writer, *call, *join.call, dominators))
                    return false;
            }
            // A std::thread destroyed on the unwind path of its join is on no normal return.
            const auto reaching = blocksReachingNormalReturn(*writer.getFunction());
            for (const auto& block : *writer.getFunction())
            {
                if (!reaching.contains(&block))
                    continue;
                for (const auto& instruction : block)
                {
                    if (&instruction == &writer || &instruction == join.call)
                        continue;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (llvm::getUnderlyingObject(store->getPointerOperand()) == base)
                        {
                            if (between(writer, instruction, *join.call, dominators))
                                return false;
                        }
                        else if (store->getValueOperand()->getType()->isPointerTy() &&
                                 llvm::getUnderlyingObject(store->getValueOperand()) == base &&
                                 !isPointerLocal(*store->getPointerOperand()) &&
                                 !dominators.dominates(&instruction, &writer))
                            return false;
                        continue;
                    }
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || dominators.dominates(&instruction, &writer) ||
                        llvm::any_of(judged,
                                     [&](const auto& entry) { return entry.first == call; }))
                        continue;
                    const auto kind = classifier.classify(*call);
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(call) || kind == CallKind::PThreadJoin ||
                        kind == CallKind::StdThreadJoin ||
                        (kind == CallKind::StdThreadDtor && dominators.dominates(join.call, call)))
                        continue;
                    for (const auto& argument : call->args())
                        if (argument->getType()->isPointerTy() &&
                            llvm::getUnderlyingObject(argument.get()) == base &&
                            !(argument.getOperandNo() == 0 && fillsApart(writer, *call, fills)))
                            return false;
                }
            }
            return true;
        }

        std::string calledName(const llvm::CallBase& call)
        {
            const auto* function = call.getCalledFunction();
            if (!function)
                return {};
            std::string name = llvm::demangle(function->getName().str());
            for (std::size_t pos = name.find("[abi:"); pos != std::string::npos;
                 pos = name.find("[abi:"))
            {
                const auto end = name.find(']', pos);
                if (end == std::string::npos)
                    break;
                name.erase(pos, end - pos + 1);
            }
            return name;
        }

        /// The standard sequences the vector proofs read alike: each appends at its end with
        /// `push_back` or `emplace_back` without moving an element out, and walks every element from
        /// `begin()` to `end()`. Their members are known by name.
        constexpr std::string_view kSequences[] = {"vector", "deque", "list"};

        /// How `vectorMethod` names the constructor and the destructor of any of the sequences.
        constexpr std::string_view kConstructor = "vector";
        constexpr std::string_view kDestructor = "~vector";

        /// The sequence `function` is a member of, if any. Only a mangled name holding the class's
        /// own name can be one of its members; the check spares demangling every other function.
        std::optional<std::string_view> sequenceOf(const llvm::Function& function)
        {
            const llvm::StringRef mangled = function.getName();
            if (llvm::none_of(kSequences, [&](std::string_view sequence)
                              { return mangled.contains(sequence); }))
                return std::nullopt;
            const std::optional<DemangledName> name = demangleFunction(mangled);
            if (!name.has_value())
                return std::nullopt;
            for (const std::string_view sequence : kSequences)
                if (isStdClass(name->context, sequence))
                    return sequence;
            return std::nullopt;
        }

        /// Whether `call` calls the member `method` of one of the sequences, read from the
        /// callee's scope and name alone: a member returning `void`, such as `push_back`, or a
        /// member template, such as `emplace_back` before C++17, has a demangled name starting with
        /// its return type. `kConstructor` and `kDestructor` name the sequence's own.
        bool vectorMethod(const llvm::CallBase& call, std::string_view method)
        {
            const llvm::Function* callee = call.getCalledFunction();
            const std::optional<std::string_view> sequence =
                callee != nullptr ? sequenceOf(*callee) : std::nullopt;
            if (!sequence)
                return false;
            const std::optional<DemangledName> name = demangleFunction(callee->getName());
            if (method == kConstructor)
                return name->base == *sequence;
            if (method == kDestructor)
                return name->base == "~" + std::string(*sequence);
            return name->base == method;
        }

        /// The iterators of the sequences, in libc++ and libstdc++.
        constexpr std::string_view kSequenceIterators[] = {
            "std::__1::__wrap_iter<",      "__gnu_cxx::__normal_iterator<",
            "std::__1::__deque_iterator<", "std::_Deque_iterator<",
            "std::__1::__list_iterator<",  "std::_List_iterator<"};

        bool iteratorMethod(const llvm::CallBase& call, std::string_view method)
        {
            const std::string name = calledName(call);
            return llvm::any_of(kSequenceIterators, [&](std::string_view iterator)
                                { return name.find(iterator) != std::string::npos; }) &&
                   name.find("operator" + std::string(method)) != std::string::npos;
        }

        /// The argument a member call is made on: past the slot a returned object is built in.
        unsigned thisIndex(const llvm::CallBase& call)
        {
            return call.paramHasAttr(0, llvm::Attribute::StructRet) ? 1 : 0;
        }

        /// Whether `call` reads the element an iterator designates: `*it`, or `it->` before a
        /// member call such as `it->join()`. Both yield the same element.
        bool dereferencesIterator(const llvm::CallBase& call)
        {
            return iteratorMethod(call, "*") || iteratorMethod(call, "->");
        }

        /// Strip an iterator object's zero-offset ABI wrapper without conflating array
        /// elements. References copied to local slots are followed only with one store.
        const llvm::Value* object(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (seen.insert(value).second)
            {
                value = value->stripPointerCasts();
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
                {
                    if (!gep->hasAllZeroIndices())
                        return value;
                    value = gep->getPointerOperand();
                }
                else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                {
                    const auto* store = uniqueStore(load->getPointerOperand());
                    if (!store)
                        return value;
                    value = store->getValueOperand();
                }
                else
                    return value;
            }
            return nullptr;
        }

        /// Where a vector lies in this function's frame: the object holding it, a local or the
        /// object a parameter points to, and its offset in that object. A vector that is itself
        /// the object lies at offset 0; a vector member of it, at the member's offset.
        struct ContainerPlace
        {
            const llvm::Value* object = nullptr;
            std::int64_t offset = 0;

            friend bool operator==(const ContainerPlace&, const ContainerPlace&) = default;
        };

        /// The place `value` points to, through casts, constant offsets and locals stored once.
        /// Nothing when an offset is not constant, or the object is neither a local nor the object
        /// a parameter points to.
        std::optional<ContainerPlace> framePlaceOf(const llvm::Value* value,
                                                   const llvm::DataLayout& layout)
        {
            std::int64_t offset = 0;
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (value != nullptr && seen.insert(value).second)
            {
                value = value->stripPointerCasts();
                if (llvm::isa<llvm::AllocaInst, llvm::Argument>(value))
                    return ContainerPlace{.object = value, .offset = offset};
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(value))
                {
                    llvm::APInt step(layout.getIndexTypeSizeInBits(gep->getType()), 0);
                    if (!gep->accumulateConstantOffset(layout, step))
                        return std::nullopt;
                    offset += step.getSExtValue();
                    value = gep->getPointerOperand();
                }
                else if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                {
                    const auto* store = uniqueStore(load->getPointerOperand());
                    if (store == nullptr)
                        return std::nullopt;
                    value = store->getValueOperand();
                }
                else
                    return std::nullopt;
            }
            return std::nullopt;
        }

        /// The place `value` points to, when its object is a local. The object a parameter points
        /// to may be reached through other pointers the caller holds.
        std::optional<ContainerPlace> placeOf(const llvm::Value* value,
                                              const llvm::DataLayout& layout)
        {
            std::optional<ContainerPlace> place = framePlaceOf(value, layout);
            if (place && !llvm::isa<llvm::AllocaInst>(place->object))
                return std::nullopt;
            return place;
        }

        /// Where a loop reads one end of the vector range it traverses: the store setting its
        /// iterator from the vector's `begin()` or `end()`, once and before the loop, that read,
        /// and the vector, as a value and as a place.
        struct RangeEnd
        {
            /// The store setting the iterator, or the call building it in place.
            const llvm::Instruction* store = nullptr;
            const llvm::CallBase* read = nullptr;
            const llvm::Value* container = nullptr;
            ContainerPlace place;
        };

        std::optional<RangeEnd> vectorRangeEnd(const llvm::Value* iterator, const llvm::Loop& loop,
                                               std::string_view method,
                                               const llvm::DominatorTree& dominators)
        {
            iterator = object(iterator);
            if (!iterator)
                return std::nullopt;
            std::optional<RangeEnd> result;
            for (const auto& block : *loop.getHeader()->getParent())
                for (const auto& instruction : block)
                {
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                    const auto* built = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    const llvm::CallBase* call = nullptr;
                    if (store != nullptr && object(store->getPointerOperand()) == iterator)
                    {
                        const llvm::Value* stored = store->getValueOperand();
                        while (const auto* cast = llvm::dyn_cast<llvm::CastInst>(stored))
                            stored = cast->getOperand(0);
                        call = llvm::dyn_cast<llvm::CallBase>(stored);
                        if (call == nullptr)
                            return std::nullopt;
                    }
                    else if (built != nullptr && thisIndex(*built) == 1 &&
                             object(built->getArgOperand(0)) == iterator)
                        call = built;
                    else
                        continue;
                    const unsigned self = thisIndex(*call);
                    if (call->arg_size() <= self || !vectorMethod(*call, method) ||
                        !dominators.dominates(&instruction, loop.getHeader()->getTerminator()) ||
                        result)
                        return std::nullopt;
                    const std::optional<ContainerPlace> place =
                        framePlaceOf(call->getArgOperand(self), call->getModule()->getDataLayout());
                    if (!place)
                        return std::nullopt;
                    result =
                        RangeEnd{&instruction, call, object(call->getArgOperand(self)), *place};
                }
            if (!result || !result->container)
                return std::nullopt;
            return result;
        }

        const llvm::CallBase* joinedDereference(const JoinSite& join)
        {
            const llvm::Value* value = &join.handle();
            llvm::SmallPtrSet<const llvm::Value*, 8> seen;
            while (seen.insert(value).second)
            {
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
                {
                    const auto* store = uniqueStore(load->getPointerOperand());
                    value = store ? store->getValueOperand() : load->getPointerOperand();
                }
                else
                    return llvm::dyn_cast<llvm::CallBase>(value);
            }
            return nullptr;
        }

        /// The block holding `loop`'s test: its header, or, when the header ends in an invoked
        /// comparison, as with an iterator whose comparison may throw, the block the comparison
        /// returns to, entered from the header alone. A comparison that throws leaves the loop
        /// without the test's exit.
        const llvm::BasicBlock* loopTest(const llvm::Loop& loop)
        {
            const llvm::BasicBlock* header = loop.getHeader();
            if (llvm::isa<llvm::BranchInst>(header->getTerminator()))
                return header;
            const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(header->getTerminator());
            const llvm::BasicBlock* test = invoke != nullptr ? invoke->getNormalDest() : nullptr;
            if (test == nullptr || !loop.contains(test) || test->getSinglePredecessor() != header ||
                !llvm::isa<llvm::BranchInst>(test->getTerminator()))
                return nullptr;
            return test;
        }

        /// The iterator a loop advances to join each element, and the value it is compared with to
        /// stop: both as objects the loop reads.
        struct TraversedRange
        {
            const llvm::Value* iterator = nullptr;
            const llvm::Value* end = nullptr;
        };

        /// What a call handed the iterator a range walk advances, or the end it stops at, as its
        /// argument `index` does to it: a comparison reads either, the iterator's own `*` and `->`
        /// read it and its `++` steps it, and a member returning an iterator in place builds it.
        /// Anything else may move it.
        enum class RangeStep
        {
            Read,
            Step,
            Build,
            Unknown,
        };

        RangeStep rangeStep(const llvm::CallBase& call, unsigned index, bool walked)
        {
            if (iteratorMethod(call, "==") || iteratorMethod(call, "!="))
                return RangeStep::Read;
            if (index == 0 && thisIndex(call) == 1)
                return RangeStep::Build;
            if (walked && index == 0 && dereferencesIterator(call))
                return RangeStep::Read;
            if (walked && index == 0 && iteratorMethod(call, "++"))
                return RangeStep::Step;
            return RangeStep::Unknown;
        }

        /// How `loop` walks a range to join each element: an iterator advanced by one `++` a
        /// round, serving only the join, through `*` or `->`, and the loop's test, which leaves the
        /// loop once the iterator equals the end. Nothing else is handed the iterator or the end.
        std::optional<TraversedRange> traversedRange(const JoinSite& join, const llvm::Loop& loop,
                                                     const llvm::DominatorTree& dominators)
        {
            const auto* dereference = joinedDereference(join);
            if (!dereference || dereference->arg_size() != 1 || !dereferencesIterator(*dereference))
                return std::nullopt;
            const llvm::Value* iterator = object(dereference->getArgOperand(0));
            const llvm::BasicBlock* test = loopTest(loop);
            const auto* branch =
                test != nullptr ? llvm::dyn_cast<llvm::BranchInst>(test->getTerminator()) : nullptr;
            if (!iterator || !branch || !branch->isConditional() ||
                !loop.contains(branch->getSuccessor(0)) || loop.contains(branch->getSuccessor(1)))
                return std::nullopt;
            const llvm::Value* condition = branch->getCondition();
            bool inverted = false;
            if (const auto* negate = llvm::dyn_cast<llvm::BinaryOperator>(condition))
                if (negate->getOpcode() == llvm::Instruction::Xor &&
                    llvm::isa<llvm::ConstantInt>(negate->getOperand(1)) &&
                    llvm::cast<llvm::ConstantInt>(negate->getOperand(1))->isOne())
                {
                    inverted = true;
                    condition = negate->getOperand(0);
                }
            const auto* compare = llvm::dyn_cast<llvm::CallBase>(condition);
            if (!compare || compare->arg_size() != 2 ||
                !iteratorMethod(*compare, inverted ? "==" : "!=") ||
                object(compare->getArgOperand(0)) != iterator)
                return std::nullopt;
            // Every call handed the iterator or the end, through any of its arguments, must be one
            // of the walk's own steps: one moving either otherwise could skip elements.
            const llvm::Value* end = object(compare->getArgOperand(1));
            if (end == nullptr)
                return std::nullopt;
            unsigned increments = 0;
            for (const auto& block : *loop.getHeader()->getParent())
                for (const auto& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;
                    for (unsigned index = 0; index < call->arg_size(); ++index)
                    {
                        const llvm::Value* used = object(call->getArgOperand(index));
                        if (used != iterator && used != end)
                            continue;
                        const RangeStep step = rangeStep(*call, index, used == iterator);
                        if (step == RangeStep::Unknown)
                            return std::nullopt;
                        if (step != RangeStep::Step)
                            continue;
                        if (!loop.contains(call) ||
                            !dominators.dominates(call, loop.getLoopLatch()->getTerminator()))
                            return std::nullopt;
                        ++increments;
                    }
                }
            if (increments != 1)
                return std::nullopt;
            return TraversedRange{.iterator = iterator, .end = compare->getArgOperand(1)};
        }

        /// The members whose iterators bound a sequence's whole range, one way or the other.
        struct WholeRange
        {
            std::string_view first;
            std::string_view last;
        };
        constexpr WholeRange kWholeRanges[] = {{.first = "begin", .last = "end"},
                                               {.first = "rbegin", .last = "rend"}};

        /// The two ends of the vector range `loop` reads to join each element, when it traverses
        /// the whole range: an iterator from `begin()` to `end()`, or from `rbegin()` to `rend()`,
        /// both read before the first round, walked as `traversedRange` checks it.
        std::optional<std::pair<RangeEnd, RangeEnd>>
        wholeRangeJoin(const JoinSite& join, const llvm::Loop& loop,
                       const llvm::DominatorTree& dominators)
        {
            const std::optional<TraversedRange> range = traversedRange(join, loop, dominators);
            if (!range)
                return std::nullopt;
            for (const WholeRange& whole : kWholeRanges)
            {
                const std::optional<RangeEnd> begin =
                    vectorRangeEnd(range->iterator, loop, whole.first, dominators);
                const std::optional<RangeEnd> end =
                    vectorRangeEnd(range->end, loop, whole.last, dominators);
                if (begin && end && end->place == begin->place)
                    return std::pair{*begin, *end};
            }
            return std::nullopt;
        }

        /// Whether `access` reads the element of the vector at `container` at the index `count`
        /// runs over.
        bool accessesInductionElement(const llvm::CallBase& access, const ContainerPlace& container,
                                      const CountedLoop& count)
        {
            if (access.arg_size() != 2 || !vectorMethod(access, "operator[]") ||
                framePlaceOf(access.getArgOperand(0), access.getModule()->getDataLayout()) !=
                    container)
                return false;
            const auto* index =
                llvm::dyn_cast<llvm::LoadInst>(stripIntegerCasts(access.getArgOperand(1)));
            return index && index->getPointerOperand() == count.induction;
        }

        bool vectorJoinRange(const llvm::CallBase& create, const JoinSite& join,
                             const llvm::Loop& first, const llvm::Loop& second,
                             const CountedLoop& count, const llvm::DominatorTree& dominators)
        {
            const auto* access = llvm::dyn_cast<llvm::CallBase>(create.getArgOperand(0));
            const auto* zero = llvm::dyn_cast<llvm::ConstantInt>(count.begin);
            if (!access || access->arg_empty() || !zero || !zero->isZero())
                return false;
            const llvm::Value* container = object(access->getArgOperand(0));
            const auto* local = llvm::dyn_cast_or_null<llvm::AllocaInst>(container);
            if (local == nullptr ||
                !accessesInductionElement(*access, ContainerPlace{.object = local}, count))
                return false;
            const auto range = wholeRangeJoin(join, second, dominators);
            if (!range || range->first.container != container)
                return false;
            bool constructed = false;
            for (const auto& block : *create.getFunction())
                for (const auto& instruction : block)
                {
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        const auto* target =
                            llvm::dyn_cast<llvm::CallBase>(store->getPointerOperand());
                        if (target && target->arg_size() > 0 &&
                            object(target->getArgOperand(0)) == container &&
                            !dominators.dominates(store, &create))
                            return false;
                    }
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        if (call->arg_empty())
                            continue;
                        for (unsigned index = 1; index < call->arg_size(); ++index)
                            if (object(call->getArgOperand(index)) == container)
                                return false;
                        if (object(call->getArgOperand(0)) != container)
                            continue;
                        if (vectorMethod(*call, kConstructor) && call->arg_size() >= 2 &&
                            invariantValue(call->getArgOperand(1)) == count.end &&
                            dominators.dominates(call, first.getHeader()->getTerminator()))
                            constructed = true;
                        else if (vectorMethod(*call, kDestructor))
                        {
                            if (dominators.dominates(call, join.call))
                                return false;
                        }
                        else if (!vectorMethod(*call, "operator[]") &&
                                 !vectorMethod(*call, "begin") && !vectorMethod(*call, "end"))
                            return false;
                    }
                }
            return constructed;
        }

        /// Whether `store` writes back into `slot` the value read from it plus one.
        bool raisesByOne(const llvm::StoreInst& store, const llvm::Value& slot)
        {
            const auto* add = llvm::dyn_cast<llvm::BinaryOperator>(store.getValueOperand());
            if (add == nullptr || add->getOpcode() != llvm::Instruction::Add ||
                store.getPointerOperand() != &slot)
                return false;
            const auto* old = llvm::dyn_cast<llvm::LoadInst>(add->getOperand(0));
            const auto* step = llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(1));
            return old != nullptr && old->getPointerOperand() == &slot && step != nullptr &&
                   step->isOne();
        }

        std::optional<CountedLoop> countedLoop(const llvm::Loop& loop,
                                               const llvm::DominatorTree& dominators)
        {
            const auto* branch =
                llvm::dyn_cast<llvm::BranchInst>(loop.getHeader()->getTerminator());
            const auto* latch = loop.getLoopLatch();
            if (branch == nullptr || !branch->isConditional() || latch == nullptr ||
                !loop.contains(branch->getSuccessor(0)) || loop.contains(branch->getSuccessor(1)))
                return std::nullopt;
            const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
            if (compare == nullptr || (compare->getPredicate() != llvm::CmpInst::ICMP_SLT &&
                                       compare->getPredicate() != llvm::CmpInst::ICMP_ULT))
                return std::nullopt;
            const auto* counter = llvm::dyn_cast<llvm::LoadInst>(compare->getOperand(0));
            if (counter == nullptr || !llvm::isa<llvm::AllocaInst>(counter->getPointerOperand()))
                return std::nullopt;
            const llvm::Value* slot = counter->getPointerOperand();
            const llvm::StoreInst* initial = nullptr;
            const llvm::StoreInst* increment = nullptr;
            for (const llvm::User* user : slot->users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand() != slot)
                        return std::nullopt;
                    auto*& target = loop.contains(store) ? increment : initial;
                    if (target != nullptr)
                        return std::nullopt;
                    target = store;
                }
                else if (!llvm::isa<llvm::LoadInst>(user) &&
                         !llvm::isa<llvm::DbgInfoIntrinsic>(user))
                    return std::nullopt;
            }
            if (initial == nullptr || increment == nullptr ||
                !dominators.dominates(initial, loop.getHeader()->getTerminator()) ||
                !dominators.dominates(increment, latch->getTerminator()) ||
                !raisesByOne(*increment, *slot))
                return std::nullopt;
            const llvm::Value* begin = invariantValue(initial->getValueOperand());
            const llvm::Value* end = invariantValue(compare->getOperand(1));
            // Reject mutable bounds, including memory changed indirectly inside either loop. A
            // local written more than once passes: only a count whose writes all precede the loop
            // can match it (`countedSlots`).
            const auto* read = llvm::dyn_cast_or_null<llvm::LoadInst>(end);
            if (begin == nullptr || end == nullptr ||
                (read != nullptr && !llvm::isa<llvm::AllocaInst>(read->getPointerOperand())))
                return std::nullopt;
            return CountedLoop{slot, begin, end, compare->getPredicate(), increment};
        }

        const llvm::Value* handleAddress(const llvm::Value& handle, bool byValue)
        {
            const llvm::Value* value = &handle;
            if (byValue)
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                if (load == nullptr)
                    return nullptr;
                value = load->getPointerOperand();
            }
            return value->stripPointerCasts();
        }

        /// A handle address as `object + offset + scale * index`, `index` being the one value
        /// that varies, null when none does.
        struct SlotAddress
        {
            const llvm::Value* object = nullptr;
            std::int64_t offset = 0;
            const llvm::Value* index = nullptr;
            std::int64_t scale = 0;
        };

        /// Reads `pointer` through its chain of element addresses; the index of any of them
        /// becomes a byte offset, so two paths to one element read alike.
        std::optional<SlotAddress> slotAddress(const llvm::Value* pointer,
                                               const llvm::DataLayout& layout)
        {
            SlotAddress address;
            pointer = pointer->stripPointerCasts();
            while (const auto* element = llvm::dyn_cast<llvm::GEPOperator>(pointer))
            {
                const unsigned width = layout.getIndexSizeInBits(element->getPointerAddressSpace());
                llvm::SmallMapVector<llvm::Value*, llvm::APInt, 4> variables;
                llvm::APInt constant(width, 0);
                if (width > 64 || !element->collectOffset(layout, width, variables, constant) ||
                    variables.size() > (address.index == nullptr ? 1U : 0U))
                    return std::nullopt;
                if (!variables.empty())
                {
                    address.index = variables.front().first;
                    address.scale = variables.front().second.getSExtValue();
                }
                const auto offset = llvm::checkedAdd(address.offset, constant.getSExtValue());
                if (!offset)
                    return std::nullopt;
                address.offset = *offset;
                pointer = element->getPointerOperand()->stripPointerCasts();
            }
            address.object = pointer;
            return address;
        }

        SlotBound slotBound(const llvm::Value* value)
        {
            // A negative constant is left to identity: signed and unsigned loops read it apart.
            if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value);
                constant != nullptr && !constant->isNegative() &&
                constant->getValue().getActiveBits() < 64)
                return {.index = constant->getSExtValue()};
            return {.value = value};
        }

        /// The value a local counting rounds ends with: set once to a constant before a counted
        /// loop of constant bounds, and raised by one exactly once in each of that loop's rounds,
        /// which no early exit cuts short. Nothing else writes it, so past the loop it holds its
        /// first value plus the number of rounds.
        std::optional<std::int64_t> countAfterRounds(const llvm::AllocaInst& local,
                                                     const llvm::LoopInfo& loops,
                                                     const llvm::DominatorTree& dominators)
        {
            const llvm::StoreInst* initial = nullptr;
            const llvm::StoreInst* raise = nullptr;
            for (const llvm::User* user : local.users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr)
                {
                    if (!llvm::isa<llvm::LoadInst>(user) &&
                        !llvm::isa<llvm::DbgInfoIntrinsic>(user))
                        return std::nullopt;
                    continue;
                }
                if (store->getPointerOperand() != &local)
                    return std::nullopt;
                auto*& target = loops.getLoopFor(store->getParent()) == nullptr ? initial : raise;
                if (target != nullptr)
                    return std::nullopt;
                target = store;
            }
            if (initial == nullptr || raise == nullptr || !raisesByOne(*raise, local))
                return std::nullopt;
            const llvm::Loop* loop = loops.getLoopFor(raise->getParent());
            const std::optional<CountedLoop> rounds = countedLoop(*loop, dominators);
            const SlotBound first = slotBound(initial->getValueOperand());
            if (loop->getParentLoop() != nullptr || !rounds || !noEarlyNormalExit(*loop) ||
                !dominators.dominates(initial, loop->getHeader()) ||
                !dominators.dominates(raise, loop->getLoopLatch()->getTerminator()) || !first.index)
                return std::nullopt;
            const SlotBound begin = slotBound(rounds->begin);
            const SlotBound end = slotBound(rounds->end);
            if (!begin.index || !end.index)
                return std::nullopt;
            return llvm::checkedAdd(*first.index,
                                    std::max<std::int64_t>(*end.index - *begin.index, 0));
        }

        /// A bound read from a local written more than once: the value a count of that local ends
        /// with. It is known when the local counts rounds, and names the local otherwise.
        SlotBound countBound(const llvm::AllocaInst& local, const llvm::LoopInfo& loops,
                             const llvm::DominatorTree& dominators)
        {
            if (const std::optional<std::int64_t> final =
                    countAfterRounds(local, loops, dominators))
                return {.index = *final};
            return {.value = &local};
        }

        /// The slots `pointer` names over the rounds of a counted loop: its only varying index
        /// must be the counter, read in the round before the counter is raised. Read after,
        /// it names the next round's slot, one past the range.
        std::optional<SlotRange> countedSlots(const llvm::Value* pointer, const CountedLoop& count,
                                              const llvm::LoopInfo& loops,
                                              const llvm::DominatorTree& dominators,
                                              const llvm::DataLayout& layout)
        {
            const std::optional<SlotAddress> address =
                pointer != nullptr ? slotAddress(pointer, layout) : std::nullopt;
            if (!address || address->index == nullptr || address->scale <= 0)
                return std::nullopt;
            const auto* read = llvm::dyn_cast<llvm::LoadInst>(stripIntegerCasts(address->index));
            if (read == nullptr || read->getPointerOperand() != count.induction ||
                !dominators.dominates(read, count.increment))
                return std::nullopt;
            const auto* readEnd = llvm::dyn_cast<llvm::LoadInst>(count.end);
            const auto* local = readEnd != nullptr
                                    ? llvm::dyn_cast<llvm::AllocaInst>(readEnd->getPointerOperand())
                                    : nullptr;
            return SlotRange{.array = address->object,
                             .origin = address->offset,
                             .stride = address->scale,
                             .begin = slotBound(count.begin),
                             .end = local != nullptr ? countBound(*local, loops, dominators)
                                                     : slotBound(count.end),
                             .predicate = count.predicate};
        }

        /// Whether every path from `from` to the end of `loop`'s round passes one of `raises`. A
        /// path leaving the loop elsewhere only unwinds (`noEarlyNormalExit`).
        bool raisedOnEveryPath(const JoinSuccess& from, const llvm::Loop& loop,
                               const llvm::SmallPtrSetImpl<const llvm::Instruction*>& raises)
        {
            // The first block is entered past `from`'s call, or at the successor of its branch.
            const llvm::Instruction* start = from.after;
            const llvm::BasicBlock* first = from.successor;
            if (const auto* invoke = llvm::dyn_cast_or_null<llvm::InvokeInst>(start))
            {
                first = invoke->getNormalDest();
                start = nullptr;
            }
            else if (start != nullptr)
                first = start->getParent();
            std::vector<std::pair<const llvm::BasicBlock*, const llvm::Instruction*>> pending{
                {first, start}};
            llvm::SmallPtrSet<const llvm::BasicBlock*, 16> visited;
            while (!pending.empty())
            {
                const auto [block, after] = pending.back();
                pending.pop_back();
                if (!loop.contains(block) || (after == nullptr && !visited.insert(block).second))
                    continue;
                bool raised = false;
                for (const llvm::Instruction& instruction : *block)
                    raised = raised || ((after == nullptr || after->comesBefore(&instruction)) &&
                                        raises.contains(&instruction));
                if (raised)
                    continue;
                if (block == loop.getLoopLatch())
                    return false;
                for (const llvm::BasicBlock* successor : llvm::successors(block))
                    pending.emplace_back(successor, nullptr);
            }
            return true;
        }

        /// The slots a spawn fills when a local counts the handles stored so far (#157): the spawn
        /// stores at `array[count]`, and the count, set once before the loop and raised by one in
        /// it, is raised past that slot before the next round wherever the spawn succeeded. Each
        /// thread started then keeps its own slot below the count's final value; a failed spawn's
        /// slot is used again. `writer` stores the handle: `create`, or the move that takes it.
        std::optional<SlotRange> slotsUpToCount(const llvm::CallBase& create,
                                                const llvm::CallBase& writer, CallKind kind,
                                                const llvm::Loop& loop, const llvm::LoopInfo& loops,
                                                const llvm::DominatorTree& dominators,
                                                const llvm::DataLayout& layout)
        {
            const std::optional<SlotAddress> address = slotAddress(writer.getArgOperand(0), layout);
            if (!address || address->index == nullptr || address->scale <= 0)
                return std::nullopt;
            const auto* read = llvm::dyn_cast<llvm::LoadInst>(stripIntegerCasts(address->index));
            const auto* count = read != nullptr
                                    ? llvm::dyn_cast<llvm::AllocaInst>(read->getPointerOperand())
                                    : nullptr;
            if (count == nullptr)
                return std::nullopt;
            const llvm::StoreInst* initial = nullptr;
            llvm::SmallPtrSet<const llvm::Instruction*, 4> raises;
            for (const llvm::User* user : count->users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr)
                {
                    if (!llvm::isa<llvm::LoadInst>(user) &&
                        !llvm::isa<llvm::DbgInfoIntrinsic>(user))
                        return std::nullopt;
                }
                else if (store->getPointerOperand() != count || (!loop.contains(store) && initial))
                    return std::nullopt;
                else if (!loop.contains(store))
                    initial = store;
                else if (raisesByOne(*store, *count))
                    raises.insert(store);
                else
                    return std::nullopt;
            }
            const llvm::Value* begin =
                initial != nullptr ? invariantValue(initial->getValueOperand()) : nullptr;
            if (begin == nullptr || llvm::isa<llvm::LoadInst>(begin) || raises.empty() ||
                !dominators.dominates(initial, loop.getHeader()))
                return std::nullopt;

            // The count leaves the slot behind before the handle is stored, or on every path from
            // the spawn's success. `pthread_create` reports success as `pthread_join` does, and a
            // `std::thread` constructor throws as `join` does.
            bool leftBehind = false;
            for (const llvm::Instruction* raise : raises)
                leftBehind = leftBehind || (dominators.dominates(read, raise) &&
                                            dominators.dominates(raise, &writer));
            const std::optional<JoinSuccess> success =
                joinSuccessOf(create, kind == CallKind::StdThreadCtor ? CallKind::StdThreadJoin
                                                                      : CallKind::PThreadJoin);
            if (!leftBehind &&
                !raisedOnEveryPath(success.value_or(JoinSuccess{.after = &create}), loop, raises))
                return std::nullopt;
            return SlotRange{.array = address->object,
                             .origin = address->offset,
                             .stride = address->scale,
                             .begin = slotBound(begin),
                             .end = countBound(*count, loops, dominators)};
        }

        /// The slots `writer` stores the handle of the thread `create` starts into over the run,
        /// when its address names them.
        std::optional<SlotRange> slotsFilled(const llvm::CallBase& create,
                                             const llvm::CallBase& writer, CallKind kind,
                                             const llvm::LoopInfo& loops,
                                             const llvm::DominatorTree& dominators)
        {
            const llvm::Loop* loop = loops.getLoopFor(create.getParent());
            const llvm::DataLayout& layout = create.getModule()->getDataLayout();
            // A creation outside any loop runs at most once, and fills the one slot its constant
            // address names.
            if (loop == nullptr)
            {
                const std::optional<SlotAddress> address =
                    slotAddress(writer.getArgOperand(0), layout);
                if (!address || address->index != nullptr)
                    return std::nullopt;
                return SlotRange{.array = address->object,
                                 .origin = address->offset,
                                 .stride = 0,
                                 .begin = {.index = 0},
                                 .end = {.index = 1}};
            }
            // A spawn loop whose bound may change fills a range no join loop can be matched with.
            if (const std::optional<CountedLoop> count = countedLoop(*loop, dominators);
                count && !llvm::isa<llvm::LoadInst>(count->end))
                if (std::optional<SlotRange> slots =
                        countedSlots(writer.getArgOperand(0), *count, loops, dominators, layout))
                    return slots;
            return slotsUpToCount(create, writer, kind, *loop, loops, dominators, layout);
        }

        /// The value a load reads from a local written once, followed through such locals: the
        /// pointer a range-for keeps in its element reference, or the iteration pointer itself.
        const llvm::Value* throughSingleStoreLocals(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 8> seen;
            while (seen.insert(value).second)
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                if (load == nullptr || !llvm::isa<llvm::AllocaInst>(load->getPointerOperand()))
                    return value;
                const llvm::StoreInst* store = uniqueStore(load->getPointerOperand());
                if (store == nullptr)
                    return value;
                value = store->getValueOperand();
            }
            return value;
        }

        /// `slotAddress` of `pointer`, its object followed through locals written once: a
        /// range-for keeps the array's address in such a local before taking its elements.
        std::optional<SlotAddress> slotAddressThroughLocals(const llvm::Value* pointer,
                                                            const llvm::DataLayout& layout)
        {
            std::optional<SlotAddress> address =
                slotAddress(throughSingleStoreLocals(pointer), layout);
            for (unsigned step = 0; address && step < 4; ++step)
            {
                const llvm::Value* object = throughSingleStoreLocals(address->object);
                if (object == address->object)
                    return address;
                const std::optional<SlotAddress> inner = slotAddress(object, layout);
                const auto offset =
                    inner ? llvm::checkedAdd(inner->offset, address->offset) : std::nullopt;
                if (!inner || !offset || (inner->index != nullptr && address->index != nullptr))
                    return std::nullopt;
                address = SlotAddress{.object = inner->object,
                                      .offset = *offset,
                                      .index = address->index ? address->index : inner->index,
                                      .scale = address->index ? address->scale : inner->scale};
            }
            return address;
        }

        /// The slots a loop visits by moving a pointer over a handle array, as a range-for over an
        /// array does: `p != end`, `p` a local set once before the loop to an element of the
        /// array and moved by one element at the end of every round, `end` an element of the same
        /// array past it. `element` must be the pointer the round reads, before it moves.
        std::optional<SlotRange> pointerSlots(const llvm::Value* element, const llvm::Loop& loop,
                                              const llvm::DominatorTree& dominators,
                                              const llvm::DataLayout& layout)
        {
            const auto* branch =
                llvm::dyn_cast<llvm::BranchInst>(loop.getHeader()->getTerminator());
            const auto* latch = loop.getLoopLatch();
            if (branch == nullptr || !branch->isConditional() || latch == nullptr ||
                !loop.contains(branch->getSuccessor(0)) || loop.contains(branch->getSuccessor(1)))
                return std::nullopt;
            const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
            if (compare == nullptr || compare->getPredicate() != llvm::CmpInst::ICMP_NE)
                return std::nullopt;
            const auto* current = llvm::dyn_cast<llvm::LoadInst>(compare->getOperand(0));
            if (current == nullptr || !llvm::isa<llvm::AllocaInst>(current->getPointerOperand()))
                return std::nullopt;
            const llvm::Value* slot = current->getPointerOperand();
            const llvm::StoreInst* initial = nullptr;
            const llvm::StoreInst* advance = nullptr;
            for (const llvm::User* user : slot->users())
            {
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (store == nullptr)
                {
                    if (!llvm::isa<llvm::LoadInst>(user) &&
                        !llvm::isa<llvm::DbgInfoIntrinsic>(user))
                        return std::nullopt;
                    continue;
                }
                if (store->getPointerOperand() != slot)
                    return std::nullopt;
                auto*& target = loop.contains(store) ? advance : initial;
                if (target != nullptr)
                    return std::nullopt;
                target = store;
            }
            // The advance need not run every round: every round joins the cursor's element, and
            // the loop ends only once the cursor has moved over every element in turn.
            if (initial == nullptr || advance == nullptr ||
                !dominators.dominates(initial, loop.getHeader()->getTerminator()))
                return std::nullopt;
            // One element further each round.
            const auto* step = llvm::dyn_cast<llvm::GetElementPtrInst>(advance->getValueOperand());
            const auto* moved = step != nullptr
                                    ? llvm::dyn_cast<llvm::LoadInst>(step->getPointerOperand())
                                    : nullptr;
            const auto* one = step != nullptr && step->getNumIndices() == 1
                                  ? llvm::dyn_cast<llvm::ConstantInt>(step->getOperand(1))
                                  : nullptr;
            if (moved == nullptr || moved->getPointerOperand() != slot || one == nullptr ||
                !one->isOne())
                return std::nullopt;
            const std::int64_t stride =
                static_cast<std::int64_t>(layout.getTypeAllocSize(step->getSourceElementType()));
            const std::optional<SlotAddress> first =
                slotAddressThroughLocals(initial->getValueOperand(), layout);
            const std::optional<SlotAddress> last =
                slotAddressThroughLocals(compare->getOperand(1), layout);
            if (stride <= 0 || !first || !last || first->index != nullptr ||
                last->index != nullptr || first->object != last->object)
                return std::nullopt;
            const auto span = llvm::checkedSub(last->offset, first->offset);
            if (!span || *span < 0 || *span % stride != 0)
                return std::nullopt;
            // The round must read the element before the pointer moves.
            const auto* read = llvm::dyn_cast<llvm::LoadInst>(throughSingleStoreLocals(element));
            if (read == nullptr || read->getPointerOperand() != slot ||
                !dominators.dominates(read, advance))
                return std::nullopt;
            return SlotRange{.array = first->object,
                             .origin = first->offset,
                             .stride = stride,
                             .begin = {.index = 0},
                             .end = {.index = *span / stride}};
        }

        std::optional<ThreadCompletion>
        loopCompletion(const llvm::CallBase& create, const llvm::CallBase& writer,
                       const JoinSite& join, const SpawnFills& fills, const llvm::LoopInfo& loops,
                       const llvm::DominatorTree& dominators,
                       const ConcurrencySymbolClassifier& classifier, const MemberJoins& members,
                       LlvmFunctionAnalysisProvider& analyses)
        {
            const llvm::Loop* first = loops.getLoopFor(create.getParent());
            const llvm::Loop* second = loops.getLoopFor(join.call->getParent());
            const auto filled = fills.find(&writer);
            // A creation outside any loop fills one slot. That it comes before the join loop is
            // `completionCoversReturns`'s to check: every path from it must leave that loop.
            const bool once = first == nullptr && filled != fills.end() &&
                              filled->second.stride == 0 && second != nullptr;
            if ((first == nullptr && !once) || second == nullptr || first == second ||
                (first != nullptr && first->getLoopLatch() == nullptr) ||
                second->getLoopLatch() == nullptr ||
                (first != nullptr &&
                 !dominators.dominates(&create, first->getLoopLatch()->getTerminator())) ||
                !dominators.dominates(join.call, second->getLoopLatch()->getTerminator()) ||
                (first != nullptr && !noEarlyNormalExit(*first)) || !noEarlyNormalExit(*second))
                return std::nullopt;
            const auto a = first != nullptr ? countedLoop(*first, dominators) : std::nullopt;
            const auto b = countedLoop(*second, dominators);
            // The join loop joins every thread the spawn started when it visits every slot the
            // spawn filled, and no other spawn stores over them in between.
            const llvm::DataLayout& layout = create.getModule()->getDataLayout();
            const llvm::Value* joined = handleAddress(join.handle(), join.byValue);
            std::optional<SlotRange> visited =
                b && joined ? countedSlots(joined, *b, loops, dominators, layout) : std::nullopt;
            if (!visited && joined)
                visited = pointerSlots(joined, *second, dominators, layout);
            const bool indexed = filled != fills.end() && visited &&
                                 covers(*visited, filled->second) &&
                                 arrayStorageUnchanged(writer, join, &fills, dominators, classifier,
                                                       members, analyses);
            if (!indexed && !(a && vectorJoinRange(create, join, *first, *second, *a, dominators)))
                return std::nullopt;
            const auto* firstBranch =
                first != nullptr
                    ? llvm::dyn_cast<llvm::BranchInst>(first->getHeader()->getTerminator())
                    : nullptr;
            const auto* secondBranch =
                llvm::dyn_cast<llvm::BranchInst>(second->getHeader()->getTerminator());
            const llvm::BasicBlock* firstExit =
                firstBranch ? firstBranch->getSuccessor(1) : nullptr;
            const llvm::BasicBlock* secondExit =
                secondBranch ? secondBranch->getSuccessor(1) : nullptr;
            if (secondExit == nullptr ||
                (!once &&
                 (firstExit == nullptr || !dominators.dominates(firstExit, second->getHeader()))))
                return std::nullopt;
            const llvm::Instruction& barrier = *secondExit->getFirstNonPHIOrDbgOrLifetime();
            if (!completionCoversReturns(create, barrier))
                return std::nullopt;
            // Every round makes its join; all the threads have ended past the loop only when each
            // round goes on past its join's success.
            ThreadCompletion completion;
            if (join.success.has_value() &&
                join.success->covers(*second->getLoopLatch()->getTerminator(), dominators))
            {
                completion.ended =
                    JoinSuccess{.branch = second->getHeader(), .successor = secondExit};
                // Both slots read at their loop's counter: the thread a round joins is the one
                // the spawn loop's round of the same counter value started. A join loop with a
                // counter tests it, so its slots were read at that counter (`countedSlots`).
                if (indexed && a && b &&
                    countedSlots(writer.getArgOperand(0), *a, loops, dominators, layout))
                    completion.roundJoin = RoundJoin{.loop = second, .success = *join.success};
            }
            return completion;
        }

        /// Whether every path from `start` to `target` goes past `through` first. Along an invoke's
        /// unwind edge, `through` has not gone past: the call did not complete.
        bool passesThrough(const llvm::Instruction& start, const llvm::Instruction& through,
                           const llvm::Instruction& target)
        {
            // A block to walk, and the instruction past which to walk it: none for a whole block.
            std::vector<std::pair<const llvm::BasicBlock*, const llvm::Instruction*>> pending{
                {start.getParent(), &start}};
            llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
            while (!pending.empty())
            {
                const auto [block, from] = pending.back();
                pending.pop_back();
                if (from == nullptr && !visited.insert(block).second)
                    continue;
                auto ahead = [&](const llvm::Instruction& instruction)
                {
                    return instruction.getParent() == block &&
                           (from == nullptr || from->comesBefore(&instruction));
                };
                const bool reachesThrough = ahead(through);
                if (ahead(target) && (!reachesThrough || target.comesBefore(&through)))
                    return false;
                if (reachesThrough)
                {
                    if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&through))
                        pending.emplace_back(invoke->getUnwindDest(), nullptr);
                    continue;
                }
                for (const auto* successor : llvm::successors(block))
                    pending.emplace_back(successor, nullptr);
            }
            return true;
        }

        /// A user of a value, or of one of its copies, with the copy it uses.
        struct CopyUse
        {
            const llvm::User* user = nullptr;
            const llvm::Value* copy = nullptr;
        };

        /// The users of `value` and of its copies: the loads of the locals it is stored in, locals
        /// that store alone writes and nothing but loads reads. A store anywhere else is a use,
        /// where the value leaves this function's hands.
        std::vector<CopyUse> usersThroughLocals(const llvm::Value& value)
        {
            std::vector<CopyUse> uses;
            std::vector<const llvm::Value*> copies{&value};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!copies.empty())
            {
                const llvm::Value* copy = copies.back();
                copies.pop_back();
                if (!seen.insert(copy).second)
                    continue;
                for (const llvm::User* user : copy->users())
                {
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    if (store == nullptr)
                    {
                        uses.push_back(CopyUse{user, copy});
                        continue;
                    }
                    const llvm::Value* slot = store->getPointerOperand();
                    if (store->getValueOperand() != copy || !llvm::isa<llvm::AllocaInst>(slot) ||
                        uniqueStore(slot) != store)
                    {
                        uses.push_back(CopyUse{user, copy});
                        continue;
                    }
                    for (const llvm::User* reader : slot->users())
                        if (llvm::isa<llvm::LoadInst>(reader))
                            copies.push_back(reader);
                }
            }
            return uses;
        }

        /// How many calls deep the code an object is handed to is read.
        constexpr unsigned kMaxCalleeDepth = 4;

        /// Whether `function` is a member of one of the sequences: what it does to its sequence is
        /// known by its name, never read from its body.
        bool memberOfSequence(const llvm::Function& function)
        {
            return sequenceOf(function).has_value();
        }

        /// The uses a vector may have: as `this` of a member of `std::vector` that `methods`
        /// names, or of one of `reads`.
        struct VectorUses
        {
            llvm::ArrayRef<std::string_view> methods;
            llvm::ArrayRef<const llvm::Instruction*> reads;

            bool operator()(const CopyUse& use) const
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(use.user);
                if (call == nullptr)
                    return false;
                if (llvm::isa<llvm::IntrinsicInst>(call))
                    return true;
                for (unsigned index = 0; index < call->arg_size(); ++index)
                    if (index != thisIndex(*call) && call->getArgOperand(index) == use.copy)
                        return false;
                for (const std::string_view method : methods)
                    if (vectorMethod(*call, method))
                        return true;
                return llvm::is_contained(reads, call);
            }
        };

        constexpr std::string_view kBuildingMethods[] = {kConstructor, kDestructor};
        constexpr std::string_view kAppendingMethods[] = {kConstructor, "reserve", "push_back",
                                                          "emplace_back", kDestructor};

        bool never(const llvm::Instruction&)
        {
            return false;
        }

        /// Whether the vector at `container` is only built and destroyed by the code a pointer
        /// `pointer` to its object reaches, `pointer` lying at `offset` in that object: another
        /// member's own uses are free, and a constructor, destructor or other function the
        /// object is handed to is read for what it does to the vector in turn. At the vector
        /// itself, `atVector` judges each use. Anything else may reach it, and refuses.
        bool usesOnlyAsAllowed(const llvm::Value& pointer, std::int64_t offset,
                               const ContainerPlace& container, const llvm::DataLayout& layout,
                               llvm::function_ref<bool(const CopyUse&)> atVector,
                               llvm::function_ref<bool(const llvm::Instruction&)> later,
                               unsigned depth)
        {
            for (const CopyUse& use : usersThroughLocals(pointer))
            {
                const auto* instruction = llvm::dyn_cast<llvm::Instruction>(use.user);
                if (instruction != nullptr && later(*instruction))
                    continue;
                if (const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(use.user);
                    gep != nullptr && gep->getPointerOperand() == use.copy)
                {
                    llvm::APInt step(layout.getIndexTypeSizeInBits(gep->getType()), 0);
                    if (!gep->accumulateConstantOffset(layout, step))
                        return false;
                    const std::int64_t start = offset + step.getSExtValue();
                    const llvm::Type* member = gep->getResultElementType();
                    // A member that does not hold the vector's first byte does not hold it.
                    if (member->isSized() &&
                        (container.offset < start ||
                         container.offset >=
                             start + static_cast<std::int64_t>(
                                         layout.getTypeAllocSize(const_cast<llvm::Type*>(member))
                                             .getFixedValue())))
                        continue;
                    if (!usesOnlyAsAllowed(*gep, start, container, layout, atVector, later, depth))
                        return false;
                    continue;
                }
                // Handing the object back to the caller, which reads the call's result in turn.
                if (llvm::isa<llvm::ReturnInst>(use.user))
                    continue;
                // At the vector's own address, a call to one of its members uses the vector; any
                // other function may be handed the object holding it, and is read below.
                const auto* call = llvm::dyn_cast<llvm::CallBase>(use.user);
                const llvm::Function* callee =
                    call != nullptr ? call->getCalledFunction() : nullptr;
                if (offset == container.offset &&
                    (callee == nullptr || callee->isDeclaration() ||
                     llvm::isa<llvm::IntrinsicInst>(call) || memberOfSequence(*callee)))
                {
                    if (!atVector(use))
                        return false;
                    continue;
                }
                // Marking the object's lifetime or its debug location leaves it unchanged.
                if (const auto* intrinsic = llvm::dyn_cast_or_null<llvm::IntrinsicInst>(call);
                    intrinsic != nullptr && (intrinsic->isLifetimeStartOrEnd() ||
                                             llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic)))
                    continue;
                if (callee == nullptr || callee->isDeclaration() || depth == 0)
                    return false;
                for (unsigned index = 0; index < call->arg_size(); ++index)
                {
                    if (call->getArgOperand(index) != use.copy)
                        continue;
                    if (index >= callee->arg_size() ||
                        !usesOnlyAsAllowed(*callee->getArg(index), offset, container, layout,
                                           VectorUses{.methods = kBuildingMethods}, never,
                                           depth - 1))
                        return false;
                }
                // The callee may hand the object back, as a constructor returning `this` does.
                if (call->getType()->isPointerTy() &&
                    !usesOnlyAsAllowed(*call, offset, container, layout, atVector, later, depth))
                    return false;
            }
            return true;
        }

        /// Whether the vector of `container` is only built, reserved, appended to and destroyed,
        /// and read by `reads` alone: nothing may take a thread out of it, nor join, detach or
        /// move one. It is used as `this` only. Code its object is handed to may only build or
        /// destroy it. Any use `later` accepts is left free: it can only run once every thread
        /// the proof covers has been joined.
        bool onlyAppendedAndReadBy(const ContainerPlace& container,
                                   llvm::ArrayRef<const llvm::Instruction*> reads,
                                   llvm::function_ref<bool(const llvm::Instruction&)> later,
                                   const llvm::DataLayout& layout)
        {
            return usesOnlyAsAllowed(*container.object, 0, container, layout,
                                     VectorUses{.methods = kAppendingMethods, .reads = reads},
                                     later, kMaxCalleeDepth);
        }

        /// Whether the element `element` reads serves `uses` alone, through the locals it is kept
        /// in: nothing else takes its thread.
        bool servesOnly(const llvm::Value& element, llvm::ArrayRef<const llvm::Instruction*> uses)
        {
            for (const CopyUse& use : usersThroughLocals(element))
            {
                if (!llvm::is_contained(uses, use.user))
                    return false;
            }
            return true;
        }

        /// How a loop reads the vector whose elements it joins: the vector, the loop's calls
        /// reading it, and the last of them before the first round, by which every thread to be
        /// joined must be in the vector.
        struct VectorTraversal
        {
            ContainerPlace container;
            std::vector<const llvm::Instruction*> reads;
            const llvm::Instruction* lastRead = nullptr;
            /// For an index below a constant rather than the vector's size, that constant: the
            /// loop reads the whole vector only if it holds that many threads.
            std::optional<std::int64_t> size;
        };

        /// The member a loop emptying a sequence reads an end through, and the one removing it.
        struct PoppedEnd
        {
            std::string_view read;
            std::string_view pop;
        };
        constexpr PoppedEnd kPoppedEnds[] = {{.read = "back", .pop = "pop_back"},
                                             {.read = "front", .pop = "pop_front"}};

        /// Whether `call` is handed the object holding the vector at `container`, through any of
        /// its arguments, but as the sequence a standard sequence's member works on when that
        /// sequence lies at another offset: two sequences of one object are distinct members, and
        /// such a member reaches nothing beyond its own.
        bool handsObjectOf(const llvm::CallBase& call, const ContainerPlace& container)
        {
            const llvm::DataLayout& layout = call.getModule()->getDataLayout();
            const llvm::Function* callee = call.getCalledFunction();
            const bool sequenceMember = callee != nullptr && memberOfSequence(*callee);
            for (unsigned index = 0; index < call.arg_size(); ++index)
            {
                const std::optional<ContainerPlace> place =
                    framePlaceOf(call.getArgOperand(index), layout);
                if (!place || place->object != container.object)
                    continue;
                if (sequenceMember && index == thisIndex(call) && place->offset != container.offset)
                    continue;
                return true;
            }
            return false;
        }

        /// How `loop` empties the vector `join` takes its elements from: its test leaves the loop
        /// once `empty()` is true, and each round joins the element `back()` reads, then removes
        /// it with `pop_back()` past the join's success, or `front()` and `pop_front()`. Nothing
        /// else in the loop is handed the vector. At the test's exit, every thread the vector held
        /// when the loop started was joined before it was removed.
        std::optional<VectorTraversal> poppedUntilEmpty(const JoinSite& join,
                                                        const llvm::Loop& loop,
                                                        const llvm::DominatorTree& dominators)
        {
            const llvm::CallBase* element = joinedDereference(join);
            const llvm::BasicBlock* test = loopTest(loop);
            const llvm::BasicBlock* preheader = loop.getLoopPreheader();
            const auto* branch =
                test != nullptr ? llvm::dyn_cast<llvm::BranchInst>(test->getTerminator()) : nullptr;
            if (element == nullptr || element->arg_size() != 1 || preheader == nullptr ||
                branch == nullptr || !branch->isConditional() ||
                !loop.contains(branch->getSuccessor(0)) || loop.contains(branch->getSuccessor(1)))
                return std::nullopt;
            const llvm::DataLayout& layout = element->getModule()->getDataLayout();
            const std::optional<ContainerPlace> container =
                framePlaceOf(element->getArgOperand(0), layout);
            // The loop goes on while the vector is not empty.
            const auto* negate = llvm::dyn_cast<llvm::BinaryOperator>(branch->getCondition());
            const auto* one = negate != nullptr && negate->getOpcode() == llvm::Instruction::Xor
                                  ? llvm::dyn_cast<llvm::ConstantInt>(negate->getOperand(1))
                                  : nullptr;
            const auto* empty = one != nullptr && one->isOne()
                                    ? llvm::dyn_cast<llvm::CallBase>(negate->getOperand(0))
                                    : nullptr;
            if (!container || empty == nullptr || empty->arg_size() != 1 ||
                !vectorMethod(*empty, "empty") ||
                framePlaceOf(empty->getArgOperand(0), layout) != container)
                return std::nullopt;
            for (const PoppedEnd& end : kPoppedEnds)
            {
                if (!vectorMethod(*element, end.read))
                    continue;
                const llvm::CallBase* pop = nullptr;
                for (const llvm::BasicBlock* block : loop.blocks())
                    for (const llvm::Instruction& instruction : *block)
                    {
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        if (call == nullptr || call == empty || call == element ||
                            !handsObjectOf(*call, *container))
                            continue;
                        if (pop != nullptr || call->arg_size() != 1 ||
                            !vectorMethod(*call, end.pop) ||
                            framePlaceOf(call->getArgOperand(0), layout) != container)
                            return std::nullopt;
                        pop = call;
                    }
                if (pop == nullptr || !join.success->covers(*pop, dominators) ||
                    !dominators.dominates(pop, loop.getLoopLatch()->getTerminator()))
                    return std::nullopt;
                return VectorTraversal{.container = *container,
                                       .reads = {empty, element, pop},
                                       .lastRead = preheader->getTerminator()};
            }
            return std::nullopt;
        }

        /// How `loop` traverses the whole vector `join` takes its elements from: popping each until
        /// it is empty, as `poppedUntilEmpty` checks it, an iterator from `begin()` to `end()`, as
        /// `wholeRangeJoin` checks it, or an index from 0 below the vector's `size()`, read before
        /// the loop or at each test, one more each round, or below a constant the vector's size must
        /// then be.
        std::optional<VectorTraversal> wholeVectorJoin(const JoinSite& join, const llvm::Loop& loop,
                                                       const llvm::DominatorTree& dominators)
        {
            if (std::optional<VectorTraversal> popped = poppedUntilEmpty(join, loop, dominators))
                return popped;
            if (const auto range = wholeRangeJoin(join, loop, dominators))
                return VectorTraversal{.container = range->first.place,
                                       .reads = {range->first.read, range->second.read},
                                       .lastRead = range->second.store};
            const std::optional<CountedLoop> count = countedLoop(loop, dominators);
            const auto* zero = count ? llvm::dyn_cast<llvm::ConstantInt>(count->begin) : nullptr;
            const auto* size = count ? llvm::dyn_cast<llvm::CallBase>(count->end) : nullptr;
            const llvm::CallBase* element = joinedDereference(join);
            if (const auto* bound = count ? llvm::dyn_cast<llvm::ConstantInt>(count->end) : nullptr)
            {
                const llvm::BasicBlock* preheader = loop.getLoopPreheader();
                const std::optional<ContainerPlace> container =
                    element != nullptr && element->arg_size() == 2
                        ? framePlaceOf(element->getArgOperand(0),
                                       element->getModule()->getDataLayout())
                        : std::nullopt;
                if (!zero || !zero->isZero() || bound->isNegative() || preheader == nullptr ||
                    !container || !accessesInductionElement(*element, *container, *count))
                    return std::nullopt;
                return VectorTraversal{.container = *container,
                                       .reads = {element},
                                       .lastRead = preheader->getTerminator(),
                                       .size = bound->getSExtValue()};
            }
            const std::optional<ContainerPlace> container =
                size && size->arg_size() == 1
                    ? framePlaceOf(size->getArgOperand(0), size->getModule()->getDataLayout())
                    : std::nullopt;
            if (!zero || !zero->isZero() || !container || !vectorMethod(*size, "size") ||
                !element || !accessesInductionElement(*element, *container, *count))
                return std::nullopt;
            return VectorTraversal{
                .container = *container, .reads = {size, element}, .lastRead = size};
        }

        /// A vector a function reaches through a parameter: the one at `offset` in the object its
        /// parameter `index` points to.
        struct ParameterVector
        {
            unsigned index = 0;
            std::int64_t offset = 0;

            friend bool operator==(const ParameterVector&, const ParameterVector&) = default;
        };

        /// Past this many, a function's joins through a pointer-to-member are no longer recorded:
        /// a recursion stepping the pair's offset would otherwise grow them without end.
        constexpr std::size_t kMaxMemberJoins = 8;

        /// An invocation through a pointer-to-member that `call` makes, itself or through the
        /// function it calls: on `object`, its operand `operand`, with the pair in the terms of the
        /// function making `call`.
        struct MemberInvocation
        {
            unsigned operand = 0;
            const llvm::Value* object = nullptr;
            MemberCondition member;
        };

        std::vector<MemberInvocation> memberInvocations(const llvm::CallBase& call,
                                                        const MemberJoins& members)
        {
            std::vector<MemberInvocation> found;
            if (const std::optional<MemberCall> direct = members.held.memberCall(call))
            {
                if (const std::optional<HeldPair> pair = members.held.pairOf(*direct))
                    found.push_back(MemberInvocation{
                        .object = direct->object,
                        .member = MemberCondition{.pair = *pair, .call = *direct}});
                return found;
            }
            const auto summary = members.byFunction.find(call.getCalledFunction());
            if (summary == members.byFunction.end())
                return found;
            for (const MemberJoin& join : summary->second)
                if (join.object < call.arg_size())
                    if (const std::optional<HeldPair> pair =
                            members.held.atCall(join.member.pair, call))
                        found.push_back(MemberInvocation{
                            .operand = join.object,
                            .object = call.getArgOperand(join.object),
                            .member = MemberCondition{.pair = *pair, .call = join.member.call}});
            return found;
        }

        bool constantPair(const HeldPair& pair)
        {
            for (const WordSource& word : pair)
                if (word.kind != WordSource::Kind::Constant)
                    return false;
            return true;
        }

        /// Whether calling `function` on an object joins the thread that object is:
        /// `std::thread::join`, or a function joining its first parameter on every normal return.
        bool isThreadJoin(const llvm::Function& function)
        {
            const std::optional<DemangledName> name = demangleFunction(function.getName());
            return name && !name->constructor && name->base == "join" &&
                   isStdClass(name->context, "thread");
        }

        /// The offsets, past the address of the object `function` is called on, of the handles it
        /// joins on every normal return: 0 for `std::thread::join`, and each offset at which it
        /// joins its first parameter.
        llvm::SmallVector<std::int64_t, 2> objectJoinOffsets(const llvm::Function& function,
                                                             const JoiningHelpers& helpers)
        {
            if (isThreadJoin(function))
                return {0};
            llvm::SmallVector<std::int64_t, 2> offsets;
            if (const auto helper = helpers.find(&function); helper != helpers.end())
                for (const JoinedParameter& parameter : helper->second)
                    if (parameter.index == 0 && parameter.ended &&
                        !llvm::is_contained(offsets, parameter.offset))
                        offsets.push_back(parameter.offset);
            return offsets;
        }

        /// Whether calling `function` joins the handle at the address of the object it is called
        /// on.
        bool joinsItsObject(const llvm::Function& function, const JoiningHelpers& helpers)
        {
            return llvm::is_contained(objectJoinOffsets(function, helpers), 0);
        }

        /// Whether the member `member` names, with its pair resolved at `call`, joins its object.
        bool joinsThrough(const MemberCondition& member, const llvm::CallBase& call,
                          const MemberJoins& members)
        {
            const std::optional<HeldPair> pair = members.held.atCall(member.pair, call);
            const llvm::Function* called =
                pair && constantPair(*pair) ? members.held.calledWith(member.call, *pair) : nullptr;
            return called != nullptr && !members.possibleStarters.contains(called) &&
                   joinsItsObject(*called, members.helpers);
        }

        /// The joins `function` makes through a pointer-to-member whose pair depends on its own
        /// parameters: each join site, with the condition its callers must meet.
        std::vector<std::pair<JoinSite, MemberCondition>>
        conditionalMemberSites(const llvm::Function& function, const MemberJoins& members)
        {
            std::vector<std::pair<JoinSite, MemberCondition>> sites;
            for (const llvm::Instruction& instruction : llvm::instructions(function))
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    for (const MemberInvocation& invocation : memberInvocations(*call, members))
                        if (!constantPair(invocation.member.pair))
                            sites.emplace_back(JoinSite{.call = call,
                                                        .operand = invocation.operand,
                                                        .success = JoinSuccess{.after = call},
                                                        .object = invocation.object},
                                               invocation.member);
            return sites;
        }

        /// Records, to a fixed point, the parameters each function invokes a member on, on every
        /// normal return, through a pointer-to-member. A pair already constant is recorded too: a
        /// wrapper fixing the member is then resolved at its own callers, as a join written in
        /// place is. A record is only ever added, and each function holds a bounded number of them.
        void collectMemberJoins(const llvm::Module& module, MemberJoins& members)
        {
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (const llvm::Function& function : module)
                {
                    if (function.isDeclaration())
                        continue;
                    const llvm::Instruction& entry = function.getEntryBlock().front();
                    for (const llvm::Instruction& instruction : llvm::instructions(function))
                    {
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        if (call == nullptr)
                            continue;
                        for (const MemberInvocation& invocation : memberInvocations(*call, members))
                        {
                            const std::optional<unsigned> object =
                                members.held.parameterOf(*invocation.object);
                            if (!object ||
                                !completionCoversReturns(entry, JoinSuccess{.after = call}))
                                continue;
                            std::vector<MemberJoin>& joins = members.byFunction[&function];
                            const MemberJoin join{.object = *object, .member = invocation.member};
                            if (joins.size() < kMaxMemberJoins && !llvm::is_contained(joins, join))
                            {
                                joins.push_back(join);
                                changed = true;
                            }
                        }
                    }
                }
            }
        }

        /// A range a function joins every thread of on every normal return: from the iterator its
        /// parameter `first` holds to the one its parameter `last` holds.
        struct JoinedRange
        {
            unsigned first = 0;
            unsigned last = 0;
            /// The range is joined only where the pointer-to-member a caller hands names a member
            /// joining its object.
            std::optional<MemberCondition> member;

            friend bool operator==(const JoinedRange&, const JoinedRange&) = default;
        };

        using ParameterVectors =
            std::unordered_map<const llvm::Function*, std::vector<ParameterVector>>;

        /// What each function this module defines does to the threads of vectors it is handed: the
        /// vectors and ranges it joins every thread of on every normal return, and the vectors it
        /// fills with every thread it leaves running.
        struct VectorHelpers
        {
            ParameterVectors vectors;
            std::unordered_map<const llvm::Function*, std::vector<JoinedRange>> ranges;
            ParameterVectors fillers;
            /// What resolves the condition of a range joined through a pointer-to-member.
            const MemberJoins* members = nullptr;
        };

        /// Whether no argument of `call` but the one at `kept` reaches `object`: through any other,
        /// the function called could take a thread out of the vector unseen. Each other argument
        /// is a constant, or resolves to another object of the frame. One whose place is unknown
        /// may be the vector itself, as the reference a function handed it returns, which the
        /// caller's check that the vector never escapes follows up to this call.
        bool reachedOnlyThrough(const llvm::CallBase& call, const llvm::Value& object,
                                unsigned kept)
        {
            const llvm::DataLayout& layout = call.getModule()->getDataLayout();
            for (unsigned index = 0; index < call.arg_size(); ++index)
            {
                const llvm::Value* argument = call.getArgOperand(index);
                if (index == kept || llvm::isa<llvm::ConstantData>(argument))
                    continue;
                const std::optional<ContainerPlace> other = framePlaceOf(argument, layout);
                if (!other || other->object == &object)
                    return false;
            }
            return true;
        }

        /// The vectors `call` hands, through one argument alone, to a function `summaries` records
        /// for them.
        std::vector<ContainerPlace> vectorsHandedTo(const llvm::CallBase& call,
                                                    const ParameterVectors& summaries)
        {
            std::vector<ContainerPlace> joined;
            const auto found = summaries.find(call.getCalledFunction());
            if (found == summaries.end())
                return joined;
            for (const ParameterVector& vector : found->second)
            {
                if (vector.index >= call.arg_size())
                    continue;
                std::optional<ContainerPlace> place = framePlaceOf(
                    call.getArgOperand(vector.index), call.getModule()->getDataLayout());
                if (!place || !reachedOnlyThrough(call, *place->object, vector.index))
                    continue;
                place->offset += vector.offset;
                joined.push_back(*place);
            }
            return joined;
        }

        /// The vectors `call` hands to a function joining every thread of them.
        std::vector<ContainerPlace> joinedVectorsOf(const llvm::CallBase& call,
                                                    const VectorHelpers& joining)
        {
            return vectorsHandedTo(call, joining.vectors);
        }

        bool joinsVector(const llvm::Instruction& instruction, const ContainerPlace& container,
                         const VectorHelpers& joining)
        {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            return call != nullptr &&
                   llvm::is_contained(joinedVectorsOf(*call, joining), container);
        }

        /// Whether `instruction` hands the vector at `container` to a function joining every thread
        /// of it or filling it: neither takes a thread out of it.
        bool handsOn(const llvm::Instruction& instruction, const ContainerPlace& container,
                     const VectorHelpers& helpers);

        /// Whether `instruction` hands the vector at `container` to a function filling it with
        /// every thread it leaves running: only appending to it.
        bool fillsVector(const llvm::Instruction& instruction, const ContainerPlace& container,
                         const VectorHelpers& helpers)
        {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            return call != nullptr &&
                   llvm::is_contained(vectorsHandedTo(*call, helpers.fillers), container);
        }

        bool handsOn(const llvm::Instruction& instruction, const ContainerPlace& container,
                     const VectorHelpers& helpers)
        {
            return joinsVector(instruction, container, helpers) ||
                   fillsVector(instruction, container, helpers);
        }

        /// Whether the vector at `container` holds exactly `size` threads once the loop inserting
        /// them is over: this function builds it empty, and inserts into it with one call alone,
        /// in a counted loop of `size` rounds, outside any other loop, without an early exit. The
        /// call runs once in every round, and a round it throws in does not go on.
        bool holdsExactly(const ContainerPlace& container, std::int64_t size,
                          const llvm::Function& function, const llvm::LoopInfo& loops,
                          const llvm::DominatorTree& dominators, const VectorHelpers& helpers)
        {
            const llvm::DataLayout& layout = function.getParent()->getDataLayout();
            const llvm::CallBase* insertion = nullptr;
            unsigned builds = 0;
            bool builtEmpty = false;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    // A call filling the vector adds threads this count does not see.
                    if (call != nullptr && fillsVector(*call, container, helpers))
                        return false;
                    if (call == nullptr || call->arg_empty() ||
                        placeOf(call->getArgOperand(0), layout) != container)
                        continue;
                    if (vectorMethod(*call, kConstructor))
                    {
                        ++builds;
                        builtEmpty = call->arg_size() == 1;
                    }
                    else if (vectorMethod(*call, "push_back") ||
                             vectorMethod(*call, "emplace_back"))
                    {
                        if (insertion != nullptr)
                            return false;
                        insertion = call;
                    }
                }
            if (builds != 1 || !builtEmpty || insertion == nullptr)
                return false;
            const llvm::Loop* loop = loops.getLoopFor(insertion->getParent());
            if (loop == nullptr || loop->getParentLoop() != nullptr ||
                loop->getLoopLatch() == nullptr || !noEarlyNormalExit(*loop) ||
                !dominators.dominates(insertion, loop->getLoopLatch()->getTerminator()))
                return false;
            if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(insertion);
                invoke != nullptr &&
                llvm::isPotentiallyReachable(invoke->getUnwindDest(), loop->getHeader(), nullptr,
                                             &dominators, &loops))
                return false;
            const std::optional<CountedLoop> rounds = countedLoop(*loop, dominators);
            const auto* begin = rounds ? llvm::dyn_cast<llvm::ConstantInt>(rounds->begin) : nullptr;
            const auto* end = rounds ? llvm::dyn_cast<llvm::ConstantInt>(rounds->end) : nullptr;
            return begin != nullptr && end != nullptr &&
                   end->getSExtValue() - begin->getSExtValue() == size;
        }

        /// The loop `join` stands in when each round goes on only past the join's success, and the
        /// element it joins serves the join alone.
        const llvm::Loop* joiningLoop(const JoinSite& join, const llvm::LoopInfo& loops,
                                      const llvm::DominatorTree& dominators)
        {
            const llvm::Loop* loop = loops.getLoopFor(join.call->getParent());
            const llvm::CallBase* element = joinedDereference(join);
            if (loop == nullptr || loop->getLoopLatch() == nullptr || loopTest(*loop) == nullptr ||
                !join.success.has_value() ||
                !join.success->covers(*loop->getLoopLatch()->getTerminator(), dominators) ||
                element == nullptr || !servesOnly(*element, {join.call}))
                return nullptr;
            return loop;
        }

        /// The edge `loop`'s test exits by, for a loop with a test: a break entering its block too,
        /// having skipped rounds, leaves that block uncovered.
        JoinSuccess loopExit(const llvm::Loop& loop)
        {
            const llvm::BasicBlock* test = loopTest(loop);
            return JoinSuccess{
                .branch = test,
                .successor = llvm::cast<llvm::BranchInst>(test->getTerminator())->getSuccessor(1),
            };
        }

        /// How a loop joins every thread of a vector: how it traverses the vector, and the edge its
        /// test exits by, past which every thread the vector held when the loop last read it
        /// before starting has been joined.
        struct VectorCompletion
        {
            VectorTraversal traversal;
            JoinSuccess exit;
        };

        /// How `join`, in a loop, joins every thread of the vector it takes its elements from: the
        /// loop takes each element of the whole vector and joins it, each round going on only past
        /// its join's success. Nothing but that loop reads the vector before leaving it, and each
        /// element it takes serves the join alone: no thread left the vector unjoined.
        std::optional<VectorCompletion> wholeVectorCompletion(const JoinSite& join,
                                                              const llvm::LoopInfo& loops,
                                                              const llvm::DominatorTree& dominators,
                                                              const VectorHelpers& helpers)
        {
            const llvm::Loop* loop = joiningLoop(join, loops, dominators);
            std::optional<VectorTraversal> traversal =
                loop != nullptr ? wholeVectorJoin(join, *loop, dominators) : std::nullopt;
            if (!traversal)
                return std::nullopt;
            // A use of the vector past the loop's exit finds every thread joined; a call filling
            // it only appends to it.
            const JoinSuccess exit = loopExit(*loop);
            const ContainerPlace& container = traversal->container;
            auto free = [&](const llvm::Instruction& use)
            { return exit.covers(use, dominators) || fillsVector(use, container, helpers); };
            if (!onlyAppendedAndReadBy(container, traversal->reads, free,
                                       join.call->getModule()->getDataLayout()))
                return std::nullopt;
            return VectorCompletion{.traversal = std::move(*traversal), .exit = exit};
        }

        /// Threads moved into the vector at `container`: started by `start`, a thread's start or a
        /// call filling the vector, and moved in by `insertion`, the call itself for the latter.
        struct MovedIn
        {
            const llvm::CallBase* start = nullptr;
            const llvm::CallBase* insertion = nullptr;
            ContainerPlace container;
        };

        /// Whether every thread `create` starts is in the vector once `insertion` has run, at
        /// `point`: every path from the start to the point goes past the insertion, unless both
        /// are one call, starting its threads into the vector before it returns.
        bool insertedBy(const llvm::CallBase& create, const llvm::CallBase& insertion,
                        const llvm::Instruction& point)
        {
            return &create == &insertion || passesThrough(create, insertion, point);
        }

        /// Where the threads `create` starts have been joined, `insertion` having moved them into
        /// the vector at `container`, as `join` proves: its loop joins every thread the vector held
        /// when the loop last read it before starting. Every path from `create` to that last read
        /// goes past `insertion`, which may not run once the read is made: the threads are among
        /// those joined.
        std::optional<JoinSuccess> insertedThreadsCompletion(
            const llvm::CallBase& create, const llvm::CallBase& insertion,
            const ContainerPlace& container, const JoinSite& join, const llvm::LoopInfo& loops,
            const llvm::DominatorTree& dominators, const VectorHelpers& helpers)
        {
            const std::optional<VectorCompletion> completion =
                wholeVectorCompletion(join, loops, dominators, helpers);
            if (!completion || completion->traversal.container != container ||
                (completion->traversal.size &&
                 !holdsExactly(container, *completion->traversal.size, *insertion.getFunction(),
                               loops, dominators, helpers)))
                return std::nullopt;
            // A thread `insertion` adds is read by the loop before its test can exit: every path
            // from the insertion to that test goes past the loop's last read first, as in a later
            // round of a loop around both, where a new thread is inserted and joined anew.
            if (!passesThrough(insertion, *completion->traversal.lastRead,
                               *completion->exit.branch->getTerminator()) ||
                !insertedBy(create, insertion, *completion->traversal.lastRead))
                return std::nullopt;
            return completion->exit;
        }

        /// Where the thread `create` started has been joined, `insertion` having moved it into a
        /// vector, when `join` joins the element `back()` or `front()` reads and goes on only past
        /// its join's success. That element is the thread `insertion` moved in:
        /// - `back()`, the last element: every path from `insertion` to any insertion into the
        ///   vector, itself in a later round included, reads the element first;
        /// - `front()`, the first: this function builds the vector empty, every path from that
        ///   build to any other insertion goes past `insertion` first, and `insertion` runs again
        ///   only once the vector has been built again.
        /// The thread is in the vector when the element is read, the element serves the join alone,
        /// and nothing else reads the vector nor takes a thread out of it before the join.
        std::optional<JoinSuccess> accessedElementCompletion(const llvm::CallBase& create,
                                                             const llvm::CallBase& insertion,
                                                             const JoinSite& join,
                                                             const llvm::DominatorTree& dominators)
        {
            const llvm::DataLayout& layout = insertion.getModule()->getDataLayout();
            const std::optional<ContainerPlace> container =
                placeOf(insertion.getArgOperand(0), layout);
            const llvm::CallBase* element = joinedDereference(join);
            if (!container || !join.success.has_value() || element == nullptr ||
                element->arg_size() != 1 ||
                placeOf(element->getArgOperand(0), layout) != container ||
                !servesOnly(*element, {join.call}) || !insertedBy(create, insertion, *element))
                return std::nullopt;
            const bool last = vectorMethod(*element, "back");
            if (!last && !vectorMethod(*element, "front"))
                return std::nullopt;

            std::vector<const llvm::CallBase*> builds;
            std::vector<const llvm::CallBase*> insertions;
            for (const llvm::BasicBlock& block : *insertion.getFunction())
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_empty() ||
                        placeOf(call->getArgOperand(0), layout) != container)
                        continue;
                    if (vectorMethod(*call, kConstructor))
                        builds.push_back(call);
                    else if (vectorMethod(*call, "push_back") ||
                             vectorMethod(*call, "emplace_back"))
                        insertions.push_back(call);
                }
            const bool builtEmptyOnce = builds.size() == 1 && builds.front()->arg_size() == 1;
            if (!last && !builtEmptyOnce)
                return std::nullopt;
            for (const llvm::CallBase* other : insertions)
            {
                if (last && !passesThrough(insertion, *element, *other))
                    return std::nullopt;
                if (!last && other == &insertion &&
                    !passesThrough(insertion, *builds.front(), insertion))
                    return std::nullopt;
                if (!last && other != &insertion &&
                    !passesThrough(*builds.front(), insertion, *other))
                    return std::nullopt;
            }

            auto pastJoin = [&](const llvm::Instruction& use)
            { return join.success->covers(use, dominators); };
            if (!onlyAppendedAndReadBy(*container, {element}, pastJoin, layout))
                return std::nullopt;
            return join.success;
        }

        /// Whether `call` alone takes `value`, directly or once cast, as an iterator passed by value
        /// is.
        bool takenOnlyBy(const llvm::Value& value, const llvm::CallBase& call)
        {
            for (const llvm::User* user : value.users())
            {
                const auto* cast = llvm::dyn_cast<llvm::CastInst>(user);
                if (user != &call && (cast == nullptr || !takenOnlyBy(*cast, call)))
                    return false;
            }
            return true;
        }

        /// The one store into the iterator object `iterator`, through its zero-offset fields, when
        /// nothing else changes or copies it. Its other uses are, with `passedTo`, loads whose value
        /// that call alone takes, the iterator passed by value; without, the iterator's own `++`,
        /// `*`, `->`, `==` and `!=`.
        const llvm::StoreInst* soleStore(const llvm::Value& iterator,
                                         const llvm::CallBase* passedTo)
        {
            const llvm::StoreInst* found = nullptr;
            std::vector<const llvm::Value*> pointers{&iterator};
            while (!pointers.empty())
            {
                const llvm::Value* pointer = pointers.back();
                pointers.pop_back();
                for (const llvm::User* user : pointer->users())
                {
                    const auto* gep = llvm::dyn_cast<llvm::GEPOperator>(user);
                    const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                    const auto* load = llvm::dyn_cast<llvm::LoadInst>(user);
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (gep != nullptr && gep->hasAllZeroIndices())
                        pointers.push_back(gep);
                    else if (store != nullptr && store->getPointerOperand() == pointer &&
                             found == nullptr)
                        found = store;
                    else if (load != nullptr && passedTo != nullptr &&
                             takenOnlyBy(*load, *passedTo))
                        continue;
                    else if (call == nullptr || passedTo != nullptr ||
                             (!dereferencesIterator(*call) && !iteratorMethod(*call, "++") &&
                              !iteratorMethod(*call, "==") && !iteratorMethod(*call, "!=")))
                        return nullptr;
                }
            }
            return found;
        }

        /// `value` once the casts between a pointer and the integer it is passed as are undone.
        const llvm::Value* uncast(const llvm::Value* value)
        {
            while (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                value = cast->getOperand(0);
            return value;
        }

        /// The parameter whose value the iterator object `iterator` receives, once, before `loop`
        /// starts, when the parameter serves that store alone and the iterator is only walked:
        /// the range end is the caller's.
        const llvm::Argument* storedParameter(const llvm::Value* iterator, const llvm::Loop& loop,
                                              const llvm::DominatorTree& dominators)
        {
            iterator = object(iterator);
            const llvm::StoreInst* store =
                iterator != nullptr && llvm::isa<llvm::AllocaInst>(iterator)
                    ? soleStore(*iterator, nullptr)
                    : nullptr;
            if (store == nullptr || !dominators.dominates(store, loop.getHeader()->getTerminator()))
                return nullptr;
            const llvm::Value* value = store->getValueOperand();
            while (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
            {
                if (!cast->hasOneUse())
                    return nullptr;
                value = cast->getOperand(0);
            }
            const auto* parameter = llvm::dyn_cast<llvm::Argument>(value);
            return parameter != nullptr && parameter->hasOneUse() ? parameter : nullptr;
        }

        /// The range of its parameters a function joins every thread of, as `join` proves it: in a
        /// loop, it walks an iterator from the one parameter to the other, as `traversedRange`
        /// checks it, joining each element.
        std::optional<std::pair<JoinedRange, JoinSuccess>>
        parameterRangeJoin(const JoinSite& join, const llvm::LoopInfo& loops,
                           const llvm::DominatorTree& dominators)
        {
            const llvm::Loop* loop = joiningLoop(join, loops, dominators);
            const std::optional<TraversedRange> range =
                loop != nullptr ? traversedRange(join, *loop, dominators) : std::nullopt;
            if (!range)
                return std::nullopt;
            const llvm::Argument* first = storedParameter(range->iterator, *loop, dominators);
            const llvm::Argument* last = storedParameter(range->end, *loop, dominators);
            if (first == nullptr || last == nullptr || first == last)
                return std::nullopt;
            return std::pair{JoinedRange{.first = first->getArgNo(), .last = last->getArgNo()},
                             loopExit(*loop)};
        }

        /// The end of a vector range `call` is handed as the argument `value`: an iterator read
        /// from the vector's `method`, `begin()` or `end()`, stored once into a local the call
        /// alone loads, before the call.
        std::optional<RangeEnd> passedRangeEnd(const llvm::Value* value, std::string_view method,
                                               const llvm::CallBase& call,
                                               const llvm::DominatorTree& dominators)
        {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(uncast(value));
            const llvm::Value* iterator =
                load != nullptr ? object(load->getPointerOperand()) : nullptr;
            const llvm::StoreInst* store =
                iterator != nullptr && llvm::isa<llvm::AllocaInst>(iterator)
                    ? soleStore(*iterator, &call)
                    : nullptr;
            const auto* read =
                store != nullptr ? llvm::dyn_cast<llvm::CallBase>(uncast(store->getValueOperand()))
                                 : nullptr;
            if (read == nullptr || read->arg_empty() || !vectorMethod(*read, method) ||
                !dominators.dominates(store, &call))
                return std::nullopt;
            const std::optional<ContainerPlace> place =
                framePlaceOf(read->getArgOperand(0), call.getModule()->getDataLayout());
            if (!place)
                return std::nullopt;
            return RangeEnd{store, read, object(read->getArgOperand(0)), *place};
        }

        /// A vector a call hands over to a function joining every thread of it, and the calls
        /// reading it for that: none when the call hands the vector itself, `begin()` and `end()`
        /// when it hands its whole range.
        struct HandOff
        {
            ContainerPlace container;
            std::vector<const llvm::Instruction*> reads;
        };

        std::vector<HandOff> handOffsOf(const llvm::CallBase& call, const VectorHelpers& joining,
                                        const llvm::DominatorTree& dominators)
        {
            std::vector<HandOff> handOffs;
            for (const ContainerPlace& container : joinedVectorsOf(call, joining))
                handOffs.push_back(HandOff{.container = container});
            const auto found = joining.ranges.find(call.getCalledFunction());
            if (found == joining.ranges.end())
                return handOffs;
            for (const JoinedRange& range : found->second)
            {
                if (range.first >= call.arg_size() || range.last >= call.arg_size() ||
                    (range.member && (joining.members == nullptr ||
                                      !joinsThrough(*range.member, call, *joining.members))))
                    continue;
                const std::optional<RangeEnd> begin =
                    passedRangeEnd(call.getArgOperand(range.first), "begin", call, dominators);
                const std::optional<RangeEnd> end =
                    passedRangeEnd(call.getArgOperand(range.last), "end", call, dominators);
                // Handing the vector itself along with its range is a use of it the caller's
                // check refuses: unlike a call handing the vector over, this one is not left free.
                if (begin && end && begin->place == end->place)
                    handOffs.push_back(
                        HandOff{.container = begin->place, .reads = {begin->read, end->read}});
            }
            return handOffs;
        }

        /// Where every thread the vector `handOff` names holds when `call` reads it has been joined:
        /// past the call's return. Nothing but those reads and calls handing the vector itself to a
        /// function joining it reads the vector before: no thread left it unjoined.
        std::optional<JoinSuccess> handedVectorCompletion(const llvm::CallBase& call,
                                                          const HandOff& handOff,
                                                          const VectorHelpers& joining,
                                                          const llvm::DominatorTree& dominators)
        {
            const JoinSuccess returned{.after = &call};
            const ContainerPlace& container = handOff.container;
            auto free = [&](const llvm::Instruction& use)
            { return returned.covers(use, dominators) || handsOn(use, container, joining); };
            if (!onlyAppendedAndReadBy(container, handOff.reads, free,
                                       call.getModule()->getDataLayout()))
                return std::nullopt;
            return returned;
        }

        /// Where the thread `create` started has been joined, `insertion` having moved it into the
        /// vector `call` hands over as `handOff`: past the call's return. Every path from `create`
        /// to the call, and to each of its reads, goes past `insertion`, which may not run between
        /// a read and the call; `create` may not run once the call is made: every thread it starts
        /// is in the vector each time the call reads it. `insertion` running again past the call
        /// could only move in the emptied local.
        std::optional<JoinSuccess>
        handedThreadsCompletion(const llvm::CallBase& create, const llvm::CallBase& insertion,
                                const ContainerPlace& container, const llvm::CallBase& call,
                                const HandOff& handOff, const VectorHelpers& joining,
                                const llvm::DominatorTree& dominators)
        {
            if (container != handOff.container ||
                llvm::isPotentiallyReachable(&call, &create, nullptr, &dominators) ||
                !insertedBy(create, insertion, call))
                return std::nullopt;
            for (const llvm::Instruction* read : handOff.reads)
                if (!insertedBy(create, insertion, *read) || !passesThrough(insertion, *read, call))
                    return std::nullopt;
            return handedVectorCompletion(call, handOff, joining, dominators);
        }

        /// The call moving the thread `create` starts out of a local, when `create` starts it into
        /// a local only to be moved: a temporary, or a local handed over with `std::move`, which
        /// clang folds away. The call takes the local as its second operand, the value it moves.
        /// Nothing else may use the local but its own destruction: the local still holds the thread
        /// `create` started when the call moves it out, and is empty afterwards.
        const llvm::CallBase* moveTaking(const llvm::CallBase& create,
                                         const ConcurrencySymbolClassifier& classifier)
        {
            const auto* local = llvm::dyn_cast<llvm::AllocaInst>(object(create.getArgOperand(0)));
            if (local == nullptr)
                return nullptr;
            const llvm::CallBase* move = nullptr;
            for (const llvm::Use& use : local->uses())
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(use.getUser());
                if (call == nullptr)
                    return nullptr;
                if (use.getOperandNo() == 0 &&
                    (call == &create || classifier.classify(*call) == CallKind::StdThreadDtor))
                    continue;
                if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call);
                    intrinsic != nullptr && (intrinsic->isLifetimeStartOrEnd() ||
                                             llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic)))
                    continue;
                if (move != nullptr || use.getOperandNo() != 1)
                    return nullptr;
                move = call;
            }
            return move;
        }

        /// The `push_back` or `emplace_back` moving the thread `create` starts into a vector.
        const llvm::CallBase* insertionTaking(const llvm::CallBase& create,
                                              const ConcurrencySymbolClassifier& classifier)
        {
            const llvm::CallBase* move = moveTaking(create, classifier);
            return move != nullptr &&
                           (vectorMethod(*move, "push_back") || vectorMethod(*move, "emplace_back"))
                       ? move
                       : nullptr;
        }

        /// The call storing the handle of the thread `create` starts: `create`, or the
        /// `std::thread` move taking it from the local `create` starts it into, as the assignment
        /// in `threads[i] = std::thread(worker)` does. The move runs once after every start that
        /// reaches a normal return: a thread left in the local is destroyed there while joinable,
        /// which ends the program.
        const llvm::CallBase& handleWriter(const llvm::CallBase& create,
                                           const ConcurrencySymbolClassifier& classifier)
        {
            const llvm::CallBase* move = moveTaking(create, classifier);
            return move != nullptr && classifier.classify(*move) == CallKind::StdThreadMove
                       ? *move
                       : create;
        }

        /// The joins `function` makes. One joining a member of the object its operand points to,
        /// at a nonzero offset, is listed only when `atOffsets` asks for it.
        std::vector<JoinSite> joinSites(const llvm::Function& function,
                                        const ConcurrencySymbolClassifier& classifier,
                                        const JoiningHelpers& helpers,
                                        const MemberJoins* members = nullptr,
                                        bool atOffsets = false)
        {
            std::vector<JoinSite> sites;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_empty())
                        continue;
                    const auto kind = classifier.classify(*call);
                    if (kind == CallKind::PThreadJoin || kind == CallKind::StdThreadJoin)
                    {
                        sites.push_back({.call = call,
                                         .operand = 0,
                                         .byValue = kind == CallKind::PThreadJoin,
                                         .reportsStatus = kind == CallKind::PThreadJoin,
                                         .success = joinSuccessOf(*call, kind)});
                        continue;
                    }
                    // A call through a pointer-to-member joins as the member it resolves to does: on
                    // its normal return, the join has.
                    if (members != nullptr)
                        for (const MemberInvocation& invocation :
                             memberInvocations(*call, *members))
                            if (constantPair(invocation.member.pair))
                                if (const llvm::Function* called = members->held.calledWith(
                                        invocation.member.call, invocation.member.pair);
                                    called != nullptr &&
                                    !members->possibleStarters.contains(called))
                                    for (const std::int64_t offset :
                                         objectJoinOffsets(*called, members->helpers))
                                        if (offset == 0 || atOffsets)
                                            sites.push_back({.call = call,
                                                             .operand = invocation.operand,
                                                             .success = JoinSuccess{.after = call},
                                                             .object = invocation.object,
                                                             .offset = offset});
                    const auto helper = helpers.find(call->getCalledFunction());
                    if (helper == helpers.end())
                        continue;
                    for (const JoinedParameter& parameter : helper->second)
                    {
                        if (parameter.index >= call->arg_size() ||
                            (parameter.offset != 0 && !atOffsets))
                            continue;
                        std::optional<JoinSuccess> success;
                        if (parameter.ended)
                            success = JoinSuccess{.after = call};
                        else if (parameter.reported)
                            success = joinSuccessOf(*call, CallKind::PThreadJoin);
                        sites.push_back({.call = call,
                                         .operand = parameter.index,
                                         .byValue = parameter.byValue,
                                         .reportsStatus = parameter.reported,
                                         .success = success,
                                         .offset = parameter.offset});
                    }
                }
            return sites;
        }

        /// Past this many calls deep, a chain of functions handing a thread on to the one joining
        /// it is not followed: the join it ends in is not trusted.
        constexpr std::size_t kMaxJoinDepth = 4;

        /// Past this many pointers into what a parameter points to, its uses are not followed.
        constexpr std::size_t kMaxParameterPointers = 256;

        /// The function `member` invokes, its pair being in the terms of the function the last of
        /// `calls` calls: the pair is carried through each call, from the last to the first, until
        /// it is constant. Null when it is not by the first.
        const llvm::Function* memberThrough(const MemberCondition& member,
                                            llvm::ArrayRef<const llvm::CallBase*> calls,
                                            const MemberJoins& members)
        {
            std::optional<HeldPair> pair = member.pair;
            for (auto call = calls.rbegin(); call != calls.rend() && pair && !constantPair(*pair);
                 ++call)
                pair = members.held.atCall(*pair, **call);
            return pair && constantPair(*pair) ? members.held.calledWith(member.call, *pair)
                                               : nullptr;
        }

        /// Whether a call to `function`, the last of `callers`, waits for the thread its
        /// parameters `parameters` hand it, each pointing into that thread's storage, as the
        /// thread was when the call was made. It must join it, and every join it makes on it must
        /// itself wait so; before any of them, nothing it does may write the storage through any
        /// of those parameters or let its address escape. A join it makes that may still change
        /// the storage past its own joins counts as such a change wherever it may precede another
        /// join, its own next round included. "Before" and "past" follow the control flow,
        /// exception edges included: a change on a path through a landing pad that rejoins a
        /// normal return is placed as any other. A pointer a call handed one of them returns is
        /// followed as they are. A use it cannot follow, a pair it is handed that its callers do
        /// not resolve, or a declaration refuses. Anything left past every join is told apart:
        /// the joins a caller makes after this call are not covered by it.
        JoinProof joinsWithoutChanging(const llvm::Function& function,
                                       llvm::ArrayRef<unsigned> parameters,
                                       llvm::ArrayRef<const llvm::CallBase*> callers,
                                       const MemberJoins& members,
                                       LlvmFunctionAnalysisProvider& analyses,
                                       const ConcurrencySymbolClassifier& classifier)
        {
            if (function.isDeclaration())
                return JoinProof::Refused;
            // The pointers into the storage, through the slots unoptimized code keeps them in,
            // and what each of their uses does.
            std::vector<const llvm::Value*> pending;
            for (const unsigned parameter : parameters)
            {
                if (parameter >= function.arg_size())
                    return JoinProof::Refused;
                pending.push_back(function.getArg(parameter));
            }
            llvm::SmallPtrSet<const llvm::Value*, 16> pointers;
            std::vector<std::pair<const llvm::CallBase*, unsigned>> calls;
            std::vector<const llvm::Instruction*> changes;
            while (!pending.empty())
            {
                const llvm::Value* pointer = pending.back();
                pending.pop_back();
                if (!pointers.insert(pointer).second)
                    continue;
                if (pointers.size() > kMaxParameterPointers)
                    return JoinProof::Refused;
                for (const llvm::Use& use : pointer->uses())
                {
                    const llvm::User* user = use.getUser();
                    if (llvm::isa<llvm::GEPOperator, llvm::BitCastOperator, llvm::PHINode,
                                  llvm::SelectInst>(user))
                        pending.push_back(user);
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (use.getOperandNo() != store->getPointerOperandIndex() &&
                            isPointerLocal(*store->getPointerOperand()))
                        {
                            for (const llvm::User* reader : store->getPointerOperand()->users())
                                if (llvm::isa<llvm::LoadInst>(reader))
                                    pending.push_back(reader);
                        }
                        else
                            changes.push_back(store);
                    }
                    else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                             call != nullptr && call->isArgOperand(&use))
                    {
                        calls.emplace_back(call, call->getArgOperandNo(&use));
                        // A pointer the call returns may be the parameter.
                        if (call->getType()->isPointerTy())
                            pending.push_back(call);
                    }
                    else if (!llvm::isa<llvm::LoadInst, llvm::ICmpInst>(user))
                        return JoinProof::Refused;
                }
            }
            const auto handedTo = [&](const JoinSite& site)
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(&site.handle());
                return pointers.contains(&site.handle()) ||
                       (site.byValue && load != nullptr &&
                        pointers.contains(load->getPointerOperand()));
            };
            // The operands of a call handed a pointer into the storage.
            const auto aliasesAt = [&](const llvm::CallBase& call)
            {
                std::vector<unsigned> aliases;
                for (const llvm::Use& argument : call.args())
                    if (pointers.contains(argument.get()))
                        aliases.push_back(call.getArgOperandNo(&argument));
                return aliases;
            };
            std::vector<const llvm::CallBase*> joins;
            // The joins that may still change the storage past their own joins.
            std::vector<const llvm::CallBase*> changingJoins;
            const auto record = [&](const llvm::CallBase& call, JoinProof proof)
            {
                joins.push_back(&call);
                if (proof == JoinProof::ChangedAfter)
                    changingJoins.push_back(&call);
                return proof != JoinProof::Refused;
            };
            for (const JoinSite& site :
                 joinSites(function, classifier, members.helpers, &members, /*atOffsets=*/true))
            {
                if (!handedTo(site))
                    continue;
                if (!record(*site.call,
                            waitsForUnchangedHandle(site, aliasesAt(*site.call), callers, members,
                                                    analyses, classifier)))
                    return JoinProof::Refused;
            }
            // A pair the function is handed names what its callers resolve it to.
            for (const llvm::Instruction& instruction : llvm::instructions(function))
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    for (const MemberInvocation& invocation : memberInvocations(*call, members))
                    {
                        if (constantPair(invocation.member.pair) ||
                            !pointers.contains(invocation.object))
                            continue;
                        if (!record(*call,
                                    waitsForUnchangedHandle(JoinSite{.call = call,
                                                                     .operand = invocation.operand,
                                                                     .object = invocation.object},
                                                            aliasesAt(*call), callers, members,
                                                            analyses, classifier)))
                            return JoinProof::Refused;
                    }
            if (joins.empty())
                return JoinProof::Refused;
            const llvm::DominatorTree& dominators = analyses.getDominatorTree(function);
            const auto beforeAJoin = [&](const llvm::Instruction& point)
            {
                return llvm::any_of(
                    joins, [&](const llvm::CallBase* join)
                    { return llvm::isPotentiallyReachable(&point, join, nullptr, &dominators); });
            };
            // A join changing the storage past its own joins does so before any other join it may
            // reach, and in a loop before its own next round.
            const llvm::LoopInfo& loops = analyses.getLoopInfo(function);
            for (const llvm::CallBase* changing : changingJoins)
            {
                if (loops.getLoopFor(changing->getParent()) != nullptr)
                    return JoinProof::Refused;
                for (const llvm::CallBase* join : joins)
                    if (join != changing &&
                        llvm::isPotentiallyReachable(changing, join, nullptr, &dominators))
                        return JoinProof::Refused;
            }
            bool changedAfter = !changingJoins.empty();
            for (const auto& [call, argument] : calls)
            {
                if (llvm::is_contained(joins, call))
                    continue;
                const ArgumentEffect effect = members.held.effectOn(*call, argument);
                if (!effect.keeps && !effect.writes)
                    continue;
                if (beforeAJoin(*call))
                    return JoinProof::Refused;
                changedAfter = true;
            }
            for (const llvm::Instruction* change : changes)
            {
                if (beforeAJoin(*change))
                    return JoinProof::Refused;
                changedAfter = true;
            }
            return changedAfter ? JoinProof::ChangedAfter : JoinProof::Unchanged;
        }

        /// Whether `join`, made in the function the last of `callers` calls or, without any, in the
        /// one whose storage is proven, waits for the thread its handle holds when it is made. A
        /// join call does. A call through a pointer-to-member does when the member its pair names
        /// there is std::thread::join, or joins its object unchanged; a call to any other function
        /// does when that function joins the parameter unchanged. `aliases` are the operands of
        /// `join` handed a pointer into the thread's storage besides its handle: what the function
        /// does through each of them is proven too. A join call changes nothing but the handle it
        /// empties, which then holds no thread, unless another of its operands points into the
        /// storage: `pthread_join` writes the thread's result there, which refuses.
        JoinProof waitsForUnchangedHandle(const JoinSite& join, llvm::ArrayRef<unsigned> aliases,
                                          llvm::ArrayRef<const llvm::CallBase*> callers,
                                          const MemberJoins& members,
                                          LlvmFunctionAnalysisProvider& analyses,
                                          const ConcurrencySymbolClassifier& classifier)
        {
            const CallKind kind = classifier.classify(*join.call);
            if (kind == CallKind::PThreadJoin || kind == CallKind::StdThreadJoin)
                return llvm::all_of(aliases,
                                    [&](unsigned operand) { return operand == join.operand; })
                           ? JoinProof::Unchanged
                           : JoinProof::Refused;
            if (callers.size() >= kMaxJoinDepth)
                return JoinProof::Refused;
            std::vector<const llvm::CallBase*> through(callers.begin(), callers.end());
            through.push_back(join.call);
            std::vector<unsigned> handed(aliases.begin(), aliases.end());
            if (!llvm::is_contained(handed, join.operand))
                handed.push_back(join.operand);
            if (const std::optional<MemberCall> direct = members.held.memberCall(*join.call))
            {
                const std::optional<HeldPair> pair = members.held.pairOf(*direct);
                const llvm::Function* member =
                    pair ? memberThrough(MemberCondition{.pair = *pair, .call = *direct}, callers,
                                         members)
                         : nullptr;
                if (member == nullptr)
                    return JoinProof::Refused;
                if (isThreadJoin(*member))
                    return JoinProof::Unchanged;
                return joinsWithoutChanging(*member, handed, through, members, analyses,
                                            classifier);
            }
            const llvm::Function* callee = join.call->getCalledFunction();
            if (callee == nullptr)
                return JoinProof::Refused;
            return joinsWithoutChanging(*callee, handed, through, members, analyses, classifier);
        }

        /// Records that `function` joins every thread of the vector at `container`, past
        /// `completion`, when the vector lies in an object a parameter points to and the
        /// completion covers every normal return. Whether the record is new.
        bool recordParameterVector(VectorHelpers& joining, const llvm::Function& function,
                                   const ContainerPlace& container, const JoinSuccess& completion)
        {
            const auto* parameter = llvm::dyn_cast<llvm::Argument>(container.object);
            if (parameter == nullptr ||
                !completionCoversReturns(function.getEntryBlock().front(), completion))
                return false;
            std::vector<ParameterVector>& joined = joining.vectors[&function];
            const ParameterVector vector{.index = parameter->getArgNo(),
                                         .offset = container.offset};
            if (llvm::is_contained(joined, vector))
                return false;
            joined.push_back(vector);
            return true;
        }

        bool startsThread(const llvm::CallBase& call, const ConcurrencySymbolClassifier& classifier)
        {
            const CallKind kind = classifier.classify(call);
            return kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor;
        }

        using Starters = std::unordered_set<const llvm::Function*>;

        /// Whether `call` starts a thread, or calls directly a function among `starters`.
        bool startsDirectly(const llvm::CallBase& call, const Starters& starters,
                            const ConcurrencySymbolClassifier& classifier)
        {
            return startsThread(call, classifier) || starters.contains(call.getCalledFunction());
        }

        /// Whether `call` may start a thread, `starters` being the functions that may. A call
        /// through a function pointer reaches the function the pointer certainly holds; one whose
        /// callee is unknown, or only declared, since the classifier reads a direct call only, may
        /// start a thread. A declared function called directly starts what the classifier tells
        /// of it, as everywhere else in the analysis.
        bool mayStart(const llvm::CallBase& call, const Starters& starters,
                      const ConcurrencySymbolClassifier& classifier)
        {
            if (startsThread(call, classifier))
                return true;
            if (const llvm::Function* called = classifier.directCallee(call))
                return starters.contains(called);
            const llvm::Function* called = resolveFunctionValue(*call.getCalledOperand());
            return called == nullptr || called->isDeclaration() || starters.contains(called);
        }

        using StartTest = bool (*)(const llvm::CallBase&, const Starters&,
                                   const ConcurrencySymbolClassifier&);

        /// The functions this module defines that make a call `starts` accepts, `starts` being
        /// handed the functions found so far.
        Starters startersBy(const llvm::Module& module,
                            const ConcurrencySymbolClassifier& classifier, StartTest starts)
        {
            Starters starters;
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (const llvm::Function& function : module)
                {
                    if (function.isDeclaration() || starters.contains(&function))
                        continue;
                    for (const llvm::Instruction& instruction : llvm::instructions(function))
                    {
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        if (call != nullptr && starts(*call, starters, classifier))
                        {
                            starters.insert(&function);
                            changed = true;
                            break;
                        }
                    }
                }
            }
            return starters;
        }

        /// The functions this module defines that may start a thread, themselves or through the
        /// functions they call: what a call can bring running threads from.
        Starters threadStarters(const llvm::Module& module,
                                const ConcurrencySymbolClassifier& classifier)
        {
            return startersBy(module, classifier, startsDirectly);
        }

        /// The functions this module defines that are not established to start no thread. Not
        /// starting one known to `threadStarters`, which follows direct calls only, is not enough:
        /// a function absent from these starts none whatever form its calls take.
        Starters possibleThreadStarters(const llvm::Module& module,
                                        const ConcurrencySymbolClassifier& classifier)
        {
            return startersBy(module, classifier, mayStart);
        }

        /// Whether every thread `function` may start, itself or through the functions it calls, is
        /// started into the object its parameter `index` points to: each start, and each call that
        /// may start one, is handed that object first. A `std::thread` constructor that does starts
        /// no thread but the one it builds; one whose body may start another, as the copy of an
        /// argument may, does not.
        bool startsOnlyInto(const llvm::Function& function, unsigned index,
                            const std::unordered_set<const llvm::Function*>& starters,
                            const ConcurrencySymbolClassifier& classifier,
                            std::set<std::pair<const llvm::Function*, unsigned>>& visiting)
        {
            if (index >= function.arg_size() || !visiting.emplace(&function, index).second)
                return false;
            const llvm::Argument* target = function.getArg(index);
            for (const llvm::Instruction& instruction : llvm::instructions(function))
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call == nullptr)
                    continue;
                const llvm::Function* callee = call->getCalledFunction();
                if (!startsThread(*call, classifier) && !starters.contains(callee))
                    continue;
                if (call->arg_empty() || object(call->getArgOperand(0)) != target)
                    return false;
                if (starters.contains(callee) &&
                    !startsOnlyInto(*callee, 0, starters, classifier, visiting))
                    return false;
            }
            return true;
        }

        bool startsOnlyInto(const llvm::Function& function, unsigned index,
                            const std::unordered_set<const llvm::Function*>& starters,
                            const ConcurrencySymbolClassifier& classifier)
        {
            std::set<std::pair<const llvm::Function*, unsigned>> visiting;
            return startsOnlyInto(function, index, starters, classifier, visiting);
        }

        /// Whether `instruction` may run more than once in a call to its function: a path leads
        /// from it back to it.
        bool mayRepeat(const llvm::Instruction& instruction)
        {
            for (const llvm::BasicBlock* successor : llvm::successors(instruction.getParent()))
                if (llvm::isPotentiallyReachable(&successor->front(), &instruction))
                    return true;
            return false;
        }

        /// Whether a call to `function` starts at most one thread, through a `std::thread`
        /// construction whose constructor starts no thread but the one it builds: it starts no
        /// thread otherwise, and the constructions and the calls that may start one exclude each
        /// other, none reachable from another or from itself, each of those calls starting at most
        /// one thread in turn. Nothing rests on the namespace of the functions on the way: an
        /// allocator, a specialization of a standard template or the copy of an argument starting
        /// a thread of its own, or a second construction, fails the proof.
        bool startsAtMostOneThread(const llvm::Function& function,
                                   const std::unordered_set<const llvm::Function*>& starters,
                                   const ConcurrencySymbolClassifier& classifier,
                                   std::unordered_map<const llvm::Function*, bool>& known)
        {
            if (const auto result = known.find(&function); result != known.end())
                return result->second;
            // A recursion is refused: its depth is unknown.
            known.emplace(&function, false);
            std::vector<const llvm::CallBase*> starts;
            for (const llvm::Instruction& instruction : llvm::instructions(function))
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                if (call == nullptr)
                    continue;
                const llvm::Function* callee = call->getCalledFunction();
                if (startsThread(*call, classifier))
                {
                    if (classifier.classify(*call) != CallKind::StdThreadCtor ||
                        (starters.contains(callee) &&
                         !startsOnlyInto(*callee, 0, starters, classifier)))
                        return false;
                }
                else if (!starters.contains(callee))
                    continue;
                else if (!startsAtMostOneThread(*callee, starters, classifier, known))
                    return false;
                starts.push_back(call);
            }
            for (const llvm::CallBase* start : starts)
            {
                if (mayRepeat(*start))
                    return false;
                for (const llvm::CallBase* other : starts)
                    if (other != start && llvm::isPotentiallyReachable(start, other))
                        return false;
            }
            return known[&function] = true;
        }

        /// Whether `call` is `emplace_back` on a sequence of `std::thread` that starts a thread, as
        /// `starters` tells: handed what the new thread runs, it builds that thread in place, so the
        /// thread is in the sequence from its start. One handed a thread already started, as
        /// `push_back` hands its argument on, only moves it in. The call must start no other
        /// thread: an allocator, or the copy of an argument, may start threads the sequence never
        /// holds, which its join leaves running. The one construction it runs builds the element,
        /// as the allocator requirements have `construct` build it from the arguments.
        bool emplacesThread(const llvm::CallBase& call,
                            const std::unordered_set<const llvm::Function*>& starters,
                            const ConcurrencySymbolClassifier& classifier)
        {
            std::unordered_map<const llvm::Function*, bool> known;
            if (!vectorMethod(call, "emplace_back") ||
                !starters.contains(call.getCalledFunction()) ||
                !startsAtMostOneThread(*call.getCalledFunction(), starters, classifier, known))
                return false;
            const std::optional<DemangledName> name =
                demangleFunction(call.getCalledFunction()->getName());
            const std::size_t open = name->context.find('<');
            const std::size_t close = name->context.find(',', open);
            return open != std::string::npos && close != std::string::npos &&
                   isStdClass(std::string_view(name->context).substr(open + 1, close - open - 1),
                              "thread");
        }

        /// The vector `function` fills with every thread it leaves running, when there is one and
        /// a parameter reaches it: each thread it starts is moved into that vector on every path to
        /// a normal return, every other call that may start a thread fills that vector too, and
        /// the vector is only built and appended to. A call to it then leaves running no thread
        /// but those in the vector it hands over.
        std::optional<ContainerPlace>
        filledVector(const llvm::Function& function, const ConcurrencySymbolClassifier& classifier,
                     const VectorHelpers& helpers,
                     const std::unordered_set<const llvm::Function*>& starters)
        {
            const llvm::DataLayout& layout = function.getParent()->getDataLayout();
            std::optional<ContainerPlace> filled;
            for (const llvm::Instruction& instruction : llvm::instructions(function))
            {
                const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                std::optional<ContainerPlace> place;
                if (call != nullptr && startsThread(*call, classifier))
                {
                    const llvm::CallBase* insertion =
                        call->arg_empty() ? nullptr : insertionTaking(*call, classifier);
                    if (insertion == nullptr ||
                        !completionCoversReturns(*call, JoinSuccess{.after = insertion}))
                        return std::nullopt;
                    place = framePlaceOf(insertion->getArgOperand(0), layout);
                }
                // Building the thread in place starts it in the vector: no path can miss the
                // insertion.
                else if (call != nullptr && emplacesThread(*call, starters, classifier))
                    place = framePlaceOf(call->getArgOperand(0), layout);
                else if (call != nullptr && starters.contains(call->getCalledFunction()))
                {
                    const std::vector<ContainerPlace> places =
                        vectorsHandedTo(*call, helpers.fillers);
                    if (places.size() == 1)
                        place = places.front();
                }
                else
                    continue;
                if (!place || (filled && *filled != *place))
                    return std::nullopt;
                filled = place;
            }
            if (!filled || !llvm::isa<llvm::Argument>(filled->object))
                return std::nullopt;
            const ContainerPlace& container = *filled;
            auto fills = [&](const llvm::Instruction& use)
            { return fillsVector(use, container, helpers); };
            if (!onlyAppendedAndReadBy(container, {}, fills, layout))
                return std::nullopt;
            return filled;
        }

        /// What each function this module defines does to the threads of vectors it is handed.
        /// Fillers come first: a function joining a vector may fill it before. The calls are
        /// iterated to a fixed point, which grows only: a destructor handing its object to the one
        /// doing the work, or a helper handing its vector to another filling it, is found whatever
        /// order the module lists them in.
        VectorHelpers collectVectorHelpers(const llvm::Module& module,
                                           const ConcurrencySymbolClassifier& classifier,
                                           LlvmFunctionAnalysisProvider& analyses,
                                           const JoiningHelpers& helpers,
                                           const MemberJoins& members)
        {
            VectorHelpers joining;
            joining.members = &members;
            const std::unordered_set<const llvm::Function*> starters =
                threadStarters(module, classifier);
            bool filling = true;
            while (filling)
            {
                filling = false;
                for (const llvm::Function& function : module)
                {
                    if (function.isDeclaration() || joining.fillers.contains(&function))
                        continue;
                    const std::optional<ContainerPlace> filled =
                        filledVector(function, classifier, joining, starters);
                    if (!filled)
                        continue;
                    joining.fillers[&function].push_back(ParameterVector{
                        .index = llvm::cast<llvm::Argument>(filled->object)->getArgNo(),
                        .offset = filled->offset});
                    filling = true;
                }
            }
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;
                const llvm::Instruction& entry = function.getEntryBlock().front();
                for (const JoinSite& join : joinSites(function, classifier, helpers, &members))
                {
                    const llvm::LoopInfo& loops = analyses.getLoopInfo(function);
                    const llvm::DominatorTree& dominators = analyses.getDominatorTree(function);
                    const std::optional<VectorCompletion> completion =
                        wholeVectorCompletion(join, loops, dominators, joining);
                    // A loop below a constant joins the whole vector only if it holds that many
                    // threads, which the caller alone knows.
                    if (completion && !completion->traversal.size)
                        recordParameterVector(joining, function, completion->traversal.container,
                                              completion->exit);
                    const auto range = parameterRangeJoin(join, loops, dominators);
                    if (range && completionCoversReturns(entry, range->second) &&
                        !llvm::is_contained(joining.ranges[&function], range->first))
                        joining.ranges[&function].push_back(range->first);
                }
                // A range joined through a pointer-to-member the function is handed is joined
                // only where its caller's pair names a member joining its object.
                for (const auto& [join, member] : conditionalMemberSites(function, members))
                {
                    auto range = parameterRangeJoin(join, analyses.getLoopInfo(function),
                                                    analyses.getDominatorTree(function));
                    if (!range || !completionCoversReturns(entry, range->second))
                        continue;
                    range->first.member = member;
                    if (!llvm::is_contained(joining.ranges[&function], range->first))
                        joining.ranges[&function].push_back(range->first);
                }
            }
            bool changed = !joining.vectors.empty() || !joining.ranges.empty();
            while (changed)
            {
                changed = false;
                for (const llvm::Function& function : module)
                    for (const llvm::BasicBlock& block : function)
                        for (const llvm::Instruction& instruction : block)
                        {
                            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                            if (call == nullptr)
                                continue;
                            const llvm::DominatorTree& dominators =
                                analyses.getDominatorTree(function);
                            for (const HandOff& handOff : handOffsOf(*call, joining, dominators))
                                if (const std::optional<JoinSuccess> returned =
                                        handedVectorCompletion(*call, handOff, joining, dominators))
                                    changed = recordParameterVector(joining, function,
                                                                    handOff.container, *returned) ||
                                              changed;
                        }
            }
            return joining;
        }

    } // namespace

    std::unordered_set<const llvm::Function*>
    selfContainedThreadConstructors(const llvm::Module& module,
                                    const ConcurrencySymbolClassifier& classifier)
    {
        const std::unordered_set<const llvm::Function*> starters =
            threadStarters(module, classifier);
        std::unordered_set<const llvm::Function*> constructors;
        for (const llvm::Function& function : module)
            for (const llvm::Instruction& instruction : llvm::instructions(function))
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    call != nullptr && classifier.classify(*call) == CallKind::StdThreadCtor &&
                    starters.contains(call->getCalledFunction()) &&
                    startsOnlyInto(*call->getCalledFunction(), 0, starters, classifier))
                    constructors.insert(call->getCalledFunction());
        return constructors;
    }

    bool JoinSuccess::covers(const llvm::Instruction& point,
                             const llvm::DominatorTree& dominators) const
    {
        if (after != nullptr)
            return after != &point && dominators.dominates(after, &point);
        return dominators.dominates(llvm::BasicBlockEdge(branch, successor), point.getParent());
    }

    std::optional<JoinSuccess> joinSuccessOf(const llvm::CallBase& join, CallKind kind)
    {
        if (kind == CallKind::StdThreadJoin)
            return JoinSuccess{.after = &join};

        // A read proves success only by testing the result for a branch, which gives that
        // branch's edge; any other read proves nothing, and a result never read counts as
        // success, as an unused one does. A local keeping the result is followed to its loads
        // after the store in the same block: a load elsewhere may read an earlier call's.
        std::optional<JoinSuccess> success;
        bool readOtherwise = false;
        auto read = [&](const llvm::User& user, const llvm::Value& result)
        {
            if (std::optional<JoinSuccess> tested = successTestedBy(user, result))
                success = tested;
            else
                readOtherwise = true;
        };

        for (const llvm::User* user : join.users())
        {
            const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
            const llvm::AllocaInst* local = store != nullptr && store->getValueOperand() == &join
                                                ? localWrittenOnlyBy(*store)
                                                : nullptr;
            if (local == nullptr)
            {
                read(*user, join);
                continue;
            }
            for (const llvm::User* reader : local->users())
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(reader);
                if (load == nullptr)
                    continue;
                if (load->getParent() != store->getParent() || !store->comesBefore(load))
                    readOtherwise = true;
                else
                    for (const llvm::User* use : load->users())
                        read(*use, *load);
            }
        }

        if (success.has_value())
            return success;
        if (!readOtherwise)
            return JoinSuccess{.after = &join};
        return std::nullopt;
    }

    bool returnsResultOf(const llvm::Instruction& call)
    {
        for (const llvm::BasicBlock& block : *call.getFunction())
        {
            const auto* exit = llvm::dyn_cast<llvm::ReturnInst>(block.getTerminator());
            if (exit != nullptr && exit->getReturnValue() != &call &&
                !readsBack(exit->getReturnValue(), call))
                return false;
        }
        return true;
    }

    std::optional<LoopCounter> loopCounter(const llvm::Loop& loop,
                                           const llvm::DominatorTree& dominators)
    {
        const std::optional<CountedLoop> counted = countedLoop(loop, dominators);
        if (!counted.has_value())
            return std::nullopt;
        return LoopCounter{.variable = counted->induction, .increment = counted->increment};
    }

    bool completionCoversReturns(const llvm::Instruction& start,
                                 const llvm::Instruction& completion)
    {
        return completionCoversReturns(start, JoinSuccess{.after = &completion});
    }

    bool completionCoversReturns(const llvm::Instruction& start, const JoinSuccess& completion)
    {
        return completedOnEveryPath(start, {completion});
    }

    bool completedOnEveryPath(const llvm::Instruction& start,
                              const std::vector<JoinSuccess>& completions,
                              const llvm::Instruction* point)
    {
        // A completion past an instruction cuts off what follows it, successors included, but for
        // an invoke's unwind edge: the call did not complete along it. One past a branch cuts off
        // that edge only.
        llvm::SmallPtrSet<const llvm::BasicBlock*, 32> entered;
        std::vector<const llvm::BasicBlock*> pending;
        // Whether a path walking `block` from `first` reaches `point`, or a return when there is
        // no point, before any completion. Successors it may go on to are queued.
        auto reaches = [&](const llvm::BasicBlock& block, llvm::BasicBlock::const_iterator first)
        {
            for (auto instruction = first; instruction != block.end(); ++instruction)
            {
                if (&*instruction == point)
                    return true;
                for (const JoinSuccess& completion : completions)
                {
                    if (completion.after != &*instruction)
                        continue;
                    if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&*instruction);
                        invoke != nullptr && entered.insert(invoke->getUnwindDest()).second)
                        pending.push_back(invoke->getUnwindDest());
                    return false;
                }
            }
            if (point == nullptr && llvm::isa<llvm::ReturnInst>(block.getTerminator()))
                return true;
            for (const llvm::BasicBlock* successor : llvm::successors(&block))
            {
                bool cutOff = false;
                for (const JoinSuccess& completion : completions)
                {
                    cutOff = cutOff ||
                             (completion.branch == &block && completion.successor == successor);
                }
                if (!cutOff && entered.insert(successor).second)
                    pending.push_back(successor);
            }
            return false;
        };

        if (reaches(*start.getParent(), start.getIterator()))
            return false;
        while (!pending.empty())
        {
            const llvm::BasicBlock* block = pending.back();
            pending.pop_back();
            if (reaches(*block, block->begin()))
                return false;
        }
        return true;
    }

    /// Past this many, a function's joined parameters are no longer recorded: each distinct
    /// offset is an entry of its own, and a recursion stepping the offset would grow them without
    /// end. A join left out is only a join not known.
    constexpr std::size_t kMaxJoinedParameters = 8;

    /// A function joins a parameter when a join of it, in place or through another joining
    /// helper, stands on every path from the entry to a normal return. The parameter is
    /// followed only through a local copy written once and the constant in-bounds offsets the
    /// data layout computes, so the handle joined lies where the caller's argument, that offset
    /// added, says. Whether the thread has also ended there, or the function hands the join's
    /// status to its caller, is summarized beside it. Iterated to a fixed point: a helper calling
    /// a helper is found whatever order the module lists them in.
    JoiningHelpers collectJoiningHelpers(const llvm::Module& module,
                                         const ConcurrencySymbolClassifier& classifier,
                                         JoiningHelpers known)
    {
        JoiningHelpers helpers = std::move(known);
        const llvm::DataLayout& layout = module.getDataLayout();
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;
                const llvm::Instruction& entry = function.getEntryBlock().front();
                for (const JoinSite& site :
                     joinSites(function, classifier, helpers, nullptr, /*atOffsets=*/true))
                {
                    // A handle travelling by value is the parameter itself, or is read from a place
                    // a parameter points to: that place is then the handle joined, as for one
                    // handed by address. One handed by address may lie at a constant offset into
                    // the object a parameter points to, the offset of the helper the site calls
                    // added.
                    const llvm::Argument* parameter = nullptr;
                    std::int64_t offset = 0;
                    bool byValue = site.byValue;
                    const llvm::Value* address = &site.handle();
                    if (site.byValue)
                    {
                        const llvm::Value* read = invariantValue(&site.handle());
                        parameter = llvm::dyn_cast_or_null<llvm::Argument>(read);
                        byValue = parameter != nullptr;
                        address = parameter == nullptr && read != nullptr
                                      ? handleAddress(*read, /*byValue=*/true)
                                      : nullptr;
                    }
                    if (parameter == nullptr && address != nullptr)
                        if (const auto place = constantOffsetFrom(*address, site.offset, layout))
                        {
                            parameter = llvm::dyn_cast<llvm::Argument>(place->first);
                            offset = place->second;
                        }
                    if (parameter == nullptr || !completionCoversReturns(entry, *site.call))
                        continue;
                    const bool ended =
                        site.success.has_value() && completionCoversReturns(entry, *site.success);
                    const bool reported = site.reportsStatus && returnsResultOf(*site.call);
                    std::vector<JoinedParameter>& joined = helpers[&function];
                    JoinedParameter* summary = nullptr;
                    for (JoinedParameter& existing : joined)
                    {
                        if (existing.index == parameter->getArgNo() && existing.offset == offset)
                            summary = &existing;
                    }
                    if (summary == nullptr)
                    {
                        // A recursion stepping its parameter's offset would add entries without
                        // end: past the bound, a join is not summarised.
                        if (joined.size() >= kMaxJoinedParameters)
                            continue;
                        joined.push_back({.index = parameter->getArgNo(),
                                          .byValue = byValue,
                                          .offset = offset,
                                          .ended = ended,
                                          .reported = reported});
                        changed = true;
                    }
                    else if ((ended && !summary->ended) || (reported && !summary->reported))
                    {
                        summary->ended = summary->ended || ended;
                        summary->reported = summary->reported || reported;
                        changed = true;
                    }
                }
            }
        }
        return helpers;
    }
    ThreadCompletionMap collectThreadCompletions(const llvm::Module& module,
                                                 const ConcurrencySymbolClassifier& classifier,
                                                 LlvmFunctionAnalysisProvider& analyses,
                                                 const JoiningHelpers& helpers)
    {
        ThreadCompletionMap result;
        const HeldMemberAnalysis held(module);
        const std::unordered_set<const llvm::Function*> possibleStarters =
            possibleThreadStarters(module, classifier);
        MemberJoins members{.held = held,
                            .helpers = helpers,
                            .possibleStarters = possibleStarters,
                            .byFunction = {}};
        collectMemberJoins(module, members);
        const VectorHelpers joining =
            collectVectorHelpers(module, classifier, analyses, helpers, members);
        const std::unordered_set<const llvm::Function*> starters =
            threadStarters(module, classifier);
        for (const llvm::Function& function : module)
        {
            std::vector<const llvm::CallBase*> creates;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        const auto kind = classifier.classify(*call);
                        if (kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor)
                            creates.push_back(call);
                    }
            std::vector<const llvm::CallBase*> fillCalls;
            std::vector<const llvm::CallBase*> emplaces;
            for (const llvm::Instruction& instruction : llvm::instructions(function))
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                {
                    if (joining.fillers.contains(call->getCalledFunction()))
                        fillCalls.push_back(call);
                    else if (emplacesThread(*call, starters, classifier))
                        emplaces.push_back(call);
                }
            if (creates.empty() && fillCalls.empty() && emplaces.empty())
                continue;
            // Only the match of a creation to its join below reads a join of an object's member at
            // a nonzero offset: every other proof reads a site's operand as the handle itself.
            const std::vector<JoinSite> allJoins =
                joinSites(function, classifier, helpers, &members, /*atOffsets=*/true);
            std::vector<JoinSite> joins;
            llvm::copy_if(allJoins, std::back_inserter(joins),
                          [](const JoinSite& join) { return join.offset == 0; });
            std::vector<const llvm::CallBase*> handOffs;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        call != nullptr && (joining.vectors.contains(call->getCalledFunction()) ||
                                            joining.ranges.contains(call->getCalledFunction())))
                        handOffs.push_back(call);
            if (allJoins.empty() && handOffs.empty())
                continue;
            const auto& dominators = analyses.getDominatorTree(function);
            const auto& loops = analyses.getLoopInfo(function);
            SpawnFills fills;
            for (const auto* create : creates)
            {
                if (create->arg_empty())
                    continue;
                const llvm::CallBase& writer = handleWriter(*create, classifier);
                if (std::optional<SlotRange> filled = slotsFilled(
                        *create, writer, classifier.classify(*create), loops, dominators))
                    fills.emplace(&writer, *filled);
            }
            // A thread moved into a vector is joined through the vector: the local it was started
            // into is empty past the move, and no join below names it. A call filling a local
            // vector with every thread it leaves running both starts them and moves them in.
            const llvm::DataLayout& layout = module.getDataLayout();
            std::vector<MovedIn> movedIn;
            for (const auto* create : creates)
            {
                const llvm::CallBase* insertion =
                    create->arg_empty() ? nullptr : insertionTaking(*create, classifier);
                if (const std::optional<ContainerPlace> container =
                        insertion != nullptr ? placeOf(insertion->getArgOperand(0), layout)
                                             : std::nullopt)
                    movedIn.push_back(
                        MovedIn{.start = create, .insertion = insertion, .container = *container});
            }
            // An `emplace_back` building its thread in place starts it in the sequence: the call
            // is its start and its insertion at once.
            for (const llvm::CallBase* call : emplaces)
                if (const std::optional<ContainerPlace> container =
                        placeOf(call->getArgOperand(0), layout))
                    movedIn.push_back(
                        MovedIn{.start = call, .insertion = call, .container = *container});
            for (const llvm::CallBase* call : fillCalls)
                for (const ContainerPlace& container : vectorsHandedTo(*call, joining.fillers))
                    if (llvm::isa<llvm::AllocaInst>(container.object))
                        movedIn.push_back(
                            MovedIn{.start = call, .insertion = call, .container = container});
            for (const MovedIn& moved : movedIn)
            {
                const llvm::CallBase& start = *moved.start;
                auto proves = [&](const std::optional<JoinSuccess>& ended)
                {
                    if (!ended || !completionCoversReturns(start, *ended))
                        return false;
                    result.emplace(&start, ThreadCompletion{.ended = ended});
                    return true;
                };
                bool proven = false;
                for (const JoinSite& join : joins)
                {
                    proven =
                        proven ||
                        proves(insertedThreadsCompletion(start, *moved.insertion, moved.container,
                                                         join, loops, dominators, joining));
                    // The element back() or front() reads is told among single insertions alone: a
                    // creation moved in, or a thread built in place, but not a call filling the
                    // vector with any number of them.
                    proven = proven || (!llvm::is_contained(fillCalls, &start) &&
                                        proves(accessedElementCompletion(start, *moved.insertion,
                                                                         join, dominators)));
                }
                for (const llvm::CallBase* call : handOffs)
                    for (const HandOff& handOff : handOffsOf(*call, joining, dominators))
                        proven =
                            proven ||
                            proves(handedThreadsCompletion(start, *moved.insertion, moved.container,
                                                           *call, handOff, joining, dominators));
            }
            for (const auto* create : creates)
                for (const JoinSite& join : allJoins)
                {
                    if (create->arg_empty())
                        continue;
                    if (join.offset == 0)
                        if (const std::optional<ThreadCompletion> completion = loopCompletion(
                                *create, handleWriter(*create, classifier), join, fills, loops,
                                dominators, classifier, members, analyses))
                        {
                            result.emplace(create, *completion);
                            break;
                        }
                    if (loops.getLoopFor(create->getParent()) != nullptr)
                        continue;
                    const llvm::CallBase& writer = handleWriter(*create, classifier);
                    const auto first = canonicalStorageGroupId(*writer.getArgOperand(0));
                    const auto second = canonicalStorageGroupId(join.handle());
                    const auto* createAddress = handleAddress(*writer.getArgOperand(0), false);
                    const auto* joinAddress = handleAddress(join.handle(), join.byValue);
                    // Wildcard storage-group indices cannot establish handle identity.
                    bool sameAddress = createAddress && createAddress == joinAddress;
                    const auto* createGep =
                        llvm::dyn_cast_or_null<llvm::GEPOperator>(createAddress);
                    const auto* joinGep = llvm::dyn_cast_or_null<llvm::GEPOperator>(joinAddress);
                    if (createGep && joinGep && createGep->hasAllConstantIndices() &&
                        joinGep->hasAllConstantIndices() && first && first == second)
                        sameAddress = true;
                    // A join names the handle itself only at no offset into what it is handed.
                    // Otherwise, as for any address, the handle is the value the address is a
                    // constant offset from and that offset, the join's own added.
                    const bool named = join.offset == 0 && sameAddress && first && first == second;
                    const auto created = createAddress != nullptr
                                             ? constantOffsetFrom(*createAddress, 0, layout)
                                             : std::nullopt;
                    const auto joined = joinAddress != nullptr
                                            ? constantOffsetFrom(*joinAddress, join.offset, layout)
                                            : std::nullopt;
                    const bool located = created && joined && *created == *joined;
                    // Another creation storing at the handle leaves the join's thread ambiguous. It
                    // is told by the storage group, and by the same location a match may rest on:
                    // a creation writing there through another spelling of the address is no less
                    // one.
                    bool overwritten = false;
                    for (const auto* other : creates)
                    {
                        if (other == create || other->arg_empty())
                            continue;
                        const llvm::Value& stored =
                            *handleWriter(*other, classifier).getArgOperand(0);
                        const auto* storedAddress = handleAddress(stored, false);
                        if (canonicalStorageGroupId(stored) == first ||
                            (created && storedAddress != nullptr &&
                             constantOffsetFrom(*storedAddress, 0, layout) == created))
                            overwritten = true;
                    }
                    if (!overwritten && (named || located) &&
                        arrayStorageUnchanged(writer, join, nullptr, dominators, classifier,
                                              members, analyses) &&
                        dominators.dominates(&writer, join.call) &&
                        completionCoversReturns(*create, *join.call))
                    {
                        ThreadCompletion completion;
                        if (join.success.has_value() &&
                            completionCoversReturns(*create, *join.success))
                            completion.ended = join.success;
                        result.emplace(create, completion);
                        break;
                    }
                }
        }
        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
