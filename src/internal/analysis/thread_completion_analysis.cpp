// SPDX-License-Identifier: Apache-2.0
#include "thread_completion_analysis.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Demangle/Demangle.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/Support/CheckedArithmetic.h>

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
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

            [[nodiscard]] const llvm::Value& handle() const
            {
                return *call->getArgOperand(operand);
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

        /// The slots each spawn of a function fills, where its handle address names them.
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
            // Two loops bounded by one value run alike only when they compare with it alike: a
            // negative `n` stops a signed loop at once and sends an unsigned one far past it.
            const bool known =
                visited.begin.index && visited.end.index && filled.begin.index && filled.end.index;
            return visited.array == filled.array && visited.origin == filled.origin &&
                   visited.stride == filled.stride &&
                   (known || visited.predicate == filled.predicate) &&
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
            const auto shift = llvm::checkedSub(first.origin, second.origin);
            return first.array == second.array && first.stride > 0 &&
                   first.stride == second.stride && shift && *shift % first.stride == 0 && a && b &&
                   (a->second < b->first || b->second < a->first);
        }

        /// Whether `other` stores its handle only into slots `create` never fills.
        bool fillsApart(const llvm::CallBase& create, const llvm::CallBase& other,
                        const SpawnFills* fills)
        {
            if (fills == nullptr)
                return false;
            const auto mine = fills->find(&create);
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

        /// Nothing but `create` and the joins touches the handle array from `create` on. Another
        /// spawn may store into it only where `fills` shows it never meets `create`'s slots.
        bool arrayStorageUnchanged(const llvm::CallBase& create, const llvm::CallBase& join,
                                   const SpawnFills* fills, const llvm::DominatorTree& dominators,
                                   const ConcurrencySymbolClassifier& classifier)
        {
            const llvm::Value* base = llvm::getUnderlyingObject(create.getArgOperand(0));
            if (!llvm::isa<llvm::AllocaInst>(base))
                return false;
            std::vector<const llvm::Value*> pointers{base};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!pointers.empty())
            {
                const auto* pointer = pointers.back();
                pointers.pop_back();
                if (!seen.insert(pointer).second)
                    continue;
                for (const auto* user : pointer->users())
                {
                    if (llvm::isa<llvm::GetElementPtrInst>(user) ||
                        llvm::isa<llvm::BitCastInst>(user))
                        pointers.push_back(user);
                    else if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getValueOperand() == pointer)
                            return false;
                    }
                    else if (const auto* call = llvm::dyn_cast<llvm::CallBase>(user))
                    {
                        const auto kind = classifier.classify(*call);
                        if (call != &join && kind != CallKind::PThreadCreate &&
                            kind != CallKind::PThreadJoin && kind != CallKind::StdThreadCtor &&
                            kind != CallKind::StdThreadJoin && kind != CallKind::StdThreadDtor &&
                            !llvm::isa<llvm::IntrinsicInst>(call))
                            return false;
                    }
                    else if (!llvm::isa<llvm::LoadInst>(user))
                        return false;
                }
            }
            // A std::thread destroyed on the unwind path of its join is on no normal return.
            const auto reaching = blocksReachingNormalReturn(*create.getFunction());
            for (const auto& block : *create.getFunction())
            {
                if (!reaching.contains(&block))
                    continue;
                for (const auto& instruction : block)
                {
                    if (&instruction == &create || &instruction == &join ||
                        dominators.dominates(&instruction, &create))
                        continue;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (llvm::getUnderlyingObject(store->getPointerOperand()) == base ||
                            (store->getValueOperand()->getType()->isPointerTy() &&
                             llvm::getUnderlyingObject(store->getValueOperand()) == base))
                            return false;
                    }
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        const auto kind = classifier.classify(*call);
                        if (llvm::isa<llvm::DbgInfoIntrinsic>(call) ||
                            kind == CallKind::PThreadJoin || kind == CallKind::StdThreadJoin ||
                            (kind == CallKind::StdThreadDtor && dominators.dominates(&join, call)))
                            continue;
                        for (const auto& argument : call->args())
                            if (argument->getType()->isPointerTy() &&
                                llvm::getUnderlyingObject(argument.get()) == base &&
                                !(argument.getOperandNo() == 0 && fillsApart(create, *call, fills)))
                                return false;
                    }
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

        bool vectorMethod(const llvm::CallBase& call, std::string_view method)
        {
            const std::string name = calledName(call);
            return name.starts_with("std::") && name.find("::vector<") != std::string::npos &&
                   name.find(">::" + std::string(method) + "(") != std::string::npos;
        }

        bool iteratorMethod(const llvm::CallBase& call, std::string_view method)
        {
            const std::string name = calledName(call);
            return (name.find("std::__1::__wrap_iter<") != std::string::npos ||
                    name.find("__gnu_cxx::__normal_iterator<") != std::string::npos) &&
                   name.find("operator" + std::string(method)) != std::string::npos;
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

        const llvm::Value* vectorRangeSource(const llvm::Value* iterator, const llvm::Loop& loop,
                                             std::string_view method,
                                             const llvm::DominatorTree& dominators)
        {
            iterator = object(iterator);
            if (!iterator)
                return nullptr;
            const llvm::Value* result = nullptr;
            for (const auto& block : *loop.getHeader()->getParent())
                for (const auto& instruction : block)
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (object(store->getPointerOperand()) != iterator)
                            continue;
                        const llvm::Value* stored = store->getValueOperand();
                        while (const auto* cast = llvm::dyn_cast<llvm::CastInst>(stored))
                            stored = cast->getOperand(0);
                        const auto* call = llvm::dyn_cast<llvm::CallBase>(stored);
                        if (!call || call->arg_empty() || !vectorMethod(*call, method) ||
                            !dominators.dominates(store, loop.getHeader()->getTerminator()) ||
                            result)
                            return nullptr;
                        result = object(call->getArgOperand(0));
                    }
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

        bool vectorJoinRange(const llvm::CallBase& create, const JoinSite& join,
                             const llvm::Loop& first, const llvm::Loop& second,
                             const CountedLoop& count, const llvm::DominatorTree& dominators)
        {
            const auto* access = llvm::dyn_cast<llvm::CallBase>(create.getArgOperand(0));
            const auto* zero = llvm::dyn_cast<llvm::ConstantInt>(count.begin);
            if (!access || access->arg_size() != 2 || !vectorMethod(*access, "operator[]") ||
                !zero || !zero->isZero())
                return false;
            const auto* index =
                llvm::dyn_cast<llvm::LoadInst>(stripIntegerCasts(access->getArgOperand(1)));
            if (!index || index->getPointerOperand() != count.induction)
                return false;
            const llvm::Value* container = object(access->getArgOperand(0));
            if (!llvm::isa_and_nonnull<llvm::AllocaInst>(container))
                return false;
            const auto* dereference = joinedDereference(join);
            if (!dereference || dereference->arg_size() != 1 || !iteratorMethod(*dereference, "*"))
                return false;
            const llvm::Value* iterator = object(dereference->getArgOperand(0));
            if (vectorRangeSource(iterator, second, "begin", dominators) != container)
                return false;
            const auto* branch =
                llvm::dyn_cast<llvm::BranchInst>(second.getHeader()->getTerminator());
            if (!branch || !branch->isConditional() || !second.contains(branch->getSuccessor(0)) ||
                second.contains(branch->getSuccessor(1)))
                return false;
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
                object(compare->getArgOperand(0)) != iterator ||
                vectorRangeSource(compare->getArgOperand(1), second, "end", dominators) !=
                    container)
                return false;
            unsigned increments = 0;
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
                        if (object(call->getArgOperand(0)) == iterator)
                        {
                            if (iteratorMethod(*call, "++") && second.contains(call) &&
                                dominators.dominates(call, second.getLoopLatch()->getTerminator()))
                                ++increments;
                            else if (!iteratorMethod(*call, "*") && !iteratorMethod(*call, "==") &&
                                     !iteratorMethod(*call, "!="))
                                return false;
                        }
                        if (object(call->getArgOperand(0)) != container)
                            continue;
                        if (vectorMethod(*call, "vector") && call->arg_size() >= 2 &&
                            invariantValue(call->getArgOperand(1)) == count.end &&
                            dominators.dominates(call, first.getHeader()->getTerminator()))
                            constructed = true;
                        else if (vectorMethod(*call, "~vector"))
                        {
                            if (dominators.dominates(call, join.call))
                                return false;
                        }
                        else if (!vectorMethod(*call, "operator[]") &&
                                 !vectorMethod(*call, "begin") && !vectorMethod(*call, "end"))
                            return false;
                    }
                }
            return constructed && increments == 1;
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
                !dominators.dominates(increment, latch->getTerminator()))
                return std::nullopt;
            const auto* add = llvm::dyn_cast<llvm::BinaryOperator>(increment->getValueOperand());
            if (add == nullptr || add->getOpcode() != llvm::Instruction::Add)
                return std::nullopt;
            const auto* old = llvm::dyn_cast<llvm::LoadInst>(add->getOperand(0));
            const auto* step = llvm::dyn_cast<llvm::ConstantInt>(add->getOperand(1));
            if (old == nullptr || old->getPointerOperand() != slot || step == nullptr ||
                !step->isOne())
                return std::nullopt;
            const llvm::Value* begin = invariantValue(initial->getValueOperand());
            const llvm::Value* end = invariantValue(compare->getOperand(1));
            // Reject mutable bounds, including memory changed indirectly inside either loop.
            if (begin == nullptr || end == nullptr || llvm::isa<llvm::LoadInst>(end))
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

        /// The slots `pointer` names over the rounds of a counted loop: its only varying index
        /// must be the counter, read in the round before the counter is raised. Read after,
        /// it names the next round's slot, one past the range.
        std::optional<SlotRange> countedSlots(const llvm::Value* pointer, const CountedLoop& count,
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
            return SlotRange{.array = address->object,
                             .origin = address->offset,
                             .stride = address->scale,
                             .begin = slotBound(count.begin),
                             .end = slotBound(count.end),
                             .predicate = count.predicate};
        }

        /// The slots `create` stores its handle into over the run, when its address names them.
        std::optional<SlotRange> slotsFilled(const llvm::CallBase& create,
                                             const llvm::LoopInfo& loops,
                                             const llvm::DominatorTree& dominators)
        {
            const llvm::Loop* loop = loops.getLoopFor(create.getParent());
            const std::optional<CountedLoop> count =
                loop != nullptr ? countedLoop(*loop, dominators) : std::nullopt;
            if (!count)
                return std::nullopt;
            return countedSlots(create.getArgOperand(0), *count, dominators,
                                create.getModule()->getDataLayout());
        }

        std::optional<ThreadCompletion>
        loopCompletion(const llvm::CallBase& create, const JoinSite& join, const SpawnFills& fills,
                       const llvm::LoopInfo& loops, const llvm::DominatorTree& dominators,
                       const ConcurrencySymbolClassifier& classifier)
        {
            const llvm::Loop* first = loops.getLoopFor(create.getParent());
            const llvm::Loop* second = loops.getLoopFor(join.call->getParent());
            if (first == nullptr || second == nullptr || first == second ||
                first->getLoopLatch() == nullptr || second->getLoopLatch() == nullptr ||
                !dominators.dominates(&create, first->getLoopLatch()->getTerminator()) ||
                !dominators.dominates(join.call, second->getLoopLatch()->getTerminator()) ||
                !noEarlyNormalExit(*first) || !noEarlyNormalExit(*second))
                return std::nullopt;
            const auto a = countedLoop(*first, dominators);
            const auto b = countedLoop(*second, dominators);
            // The join loop joins every thread the spawn started when it visits every slot the
            // spawn filled, and no other spawn stores over them in between.
            const auto filled = fills.find(&create);
            const std::optional<SlotRange> visited =
                b ? countedSlots(handleAddress(join.handle(), true), *b, dominators,
                                 create.getModule()->getDataLayout())
                  : std::nullopt;
            const bool indexed =
                filled != fills.end() && visited && covers(*visited, filled->second) &&
                arrayStorageUnchanged(create, *join.call, &fills, dominators, classifier);
            if (!indexed && !(a && vectorJoinRange(create, join, *first, *second, *a, dominators)))
                return std::nullopt;
            const auto* firstBranch =
                llvm::dyn_cast<llvm::BranchInst>(first->getHeader()->getTerminator());
            const auto* secondBranch =
                llvm::dyn_cast<llvm::BranchInst>(second->getHeader()->getTerminator());
            const llvm::BasicBlock* firstExit =
                firstBranch ? firstBranch->getSuccessor(1) : nullptr;
            const llvm::BasicBlock* secondExit =
                secondBranch ? secondBranch->getSuccessor(1) : nullptr;
            if (firstExit == nullptr || secondExit == nullptr ||
                !dominators.dominates(firstExit, second->getHeader()))
                return std::nullopt;
            const llvm::Instruction& barrier = *secondExit->getFirstNonPHIOrDbgOrLifetime();
            if (!completionCoversReturns(create, barrier))
                return std::nullopt;
            // Every round makes its join; all the threads have ended past the loop only when each
            // round goes on past its join's success.
            ThreadCompletion completion;
            if (join.success.has_value() &&
                join.success->covers(*second->getLoopLatch()->getTerminator(), dominators))
                completion.ended = JoinSuccess{.after = &barrier};
            return completion;
        }

        std::vector<JoinSite> joinSites(const llvm::Function& function,
                                        const ConcurrencySymbolClassifier& classifier,
                                        const JoiningHelpers& helpers)
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
                    const auto helper = helpers.find(call->getCalledFunction());
                    if (helper == helpers.end())
                        continue;
                    for (const JoinedParameter& parameter : helper->second)
                    {
                        if (parameter.index >= call->arg_size())
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
                                         .success = success});
                    }
                }
            return sites;
        }

    } // namespace

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
        // A completion past an instruction cuts off what follows it, successors included; one
        // past a branch cuts off that edge only.
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
                    if (completion.after == &*instruction)
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

    /// A function joins a parameter when a join of it, in place or through another joining
    /// helper, stands on every path from the entry to a normal return. The parameter is
    /// followed only through a local copy written once, so the handle joined is the one the
    /// caller passed. Whether the thread has also ended there, or the function hands the join's
    /// status to its caller, is summarized beside it. Iterated to a fixed point: a helper calling
    /// a helper is found whatever order the module lists them in.
    JoiningHelpers collectJoiningHelpers(const llvm::Module& module,
                                         const ConcurrencySymbolClassifier& classifier,
                                         JoiningHelpers known)
    {
        JoiningHelpers helpers = std::move(known);
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;
                const llvm::Instruction& entry = function.getEntryBlock().front();
                for (const JoinSite& site : joinSites(function, classifier, helpers))
                {
                    const auto* parameter =
                        llvm::dyn_cast_or_null<llvm::Argument>(invariantValue(&site.handle()));
                    if (parameter == nullptr || !completionCoversReturns(entry, *site.call))
                        continue;
                    const bool ended =
                        site.success.has_value() && completionCoversReturns(entry, *site.success);
                    const bool reported = site.reportsStatus && returnsResultOf(*site.call);
                    std::vector<JoinedParameter>& joined = helpers[&function];
                    JoinedParameter* summary = nullptr;
                    for (JoinedParameter& existing : joined)
                    {
                        if (existing.index == parameter->getArgNo())
                            summary = &existing;
                    }
                    if (summary == nullptr)
                    {
                        joined.push_back({.index = parameter->getArgNo(),
                                          .byValue = site.byValue,
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
            if (creates.empty())
                continue;
            const std::vector<JoinSite> joins = joinSites(function, classifier, helpers);
            if (joins.empty())
                continue;
            const auto& dominators = analyses.getDominatorTree(function);
            const auto& loops = analyses.getLoopInfo(function);
            SpawnFills fills;
            for (const auto* create : creates)
                if (!create->arg_empty())
                    if (std::optional<SlotRange> filled = slotsFilled(*create, loops, dominators))
                        fills.emplace(create, *filled);
            for (const auto* create : creates)
                for (const JoinSite& join : joins)
                {
                    if (create->arg_empty())
                        continue;
                    if (const std::optional<ThreadCompletion> completion =
                            loopCompletion(*create, join, fills, loops, dominators, classifier))
                    {
                        result.emplace(create, *completion);
                        break;
                    }
                    if (loops.getLoopFor(create->getParent()) != nullptr)
                        continue;
                    const auto first = canonicalStorageGroupId(*create->getArgOperand(0));
                    const auto second = canonicalStorageGroupId(join.handle());
                    const auto* createAddress = handleAddress(*create->getArgOperand(0), false);
                    const auto* joinAddress = handleAddress(join.handle(), join.byValue);
                    // Wildcard storage-group indices cannot establish handle identity.
                    bool sameAddress = createAddress && createAddress == joinAddress;
                    const auto* createGep =
                        llvm::dyn_cast_or_null<llvm::GEPOperator>(createAddress);
                    const auto* joinGep = llvm::dyn_cast_or_null<llvm::GEPOperator>(joinAddress);
                    if (createGep && joinGep && createGep->hasAllConstantIndices() &&
                        joinGep->hasAllConstantIndices() && first && first == second)
                        sameAddress = true;
                    bool overwritten = false;
                    for (const auto* other : creates)
                        if (other != create && !other->arg_empty() &&
                            canonicalStorageGroupId(*other->getArgOperand(0)) == first)
                            overwritten = true;
                    if (!overwritten && sameAddress && first && first == second &&
                        arrayStorageUnchanged(*create, *join.call, nullptr, dominators,
                                              classifier) &&
                        dominators.dominates(create, join.call) &&
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
