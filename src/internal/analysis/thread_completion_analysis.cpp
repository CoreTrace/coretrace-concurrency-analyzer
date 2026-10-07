// SPDX-License-Identifier: Apache-2.0
#include "thread_completion_analysis.hpp"

#include "concurrency_symbol_classifier.hpp"
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

        /// Nothing but `writer` and the joins touches the handle array from `writer` on, `writer`
        /// storing a handle into it: a creation, or the move of a thread a creation started into
        /// a local. Another spawn may store into it only where `fills` shows it never meets
        /// `writer`'s slots.
        bool arrayStorageUnchanged(const llvm::CallBase& writer, const llvm::CallBase& join,
                                   const SpawnFills* fills, const llvm::DominatorTree& dominators,
                                   const ConcurrencySymbolClassifier& classifier)
        {
            const llvm::Value* base = llvm::getUnderlyingObject(writer.getArgOperand(0));
            if (!llvm::isa<llvm::AllocaInst>(base))
                return false;
            // The array's address may be kept in locals only loaded from (a range-for's cursor,
            // an aggregate initializer's cleanup cursor). A pointer read back from one is followed
            // too, but may only be read through, compared, or handed to a join or a destructor:
            // the second walk below does not see through those locals.
            std::vector<std::pair<const llvm::Value*, bool>> pointers{{base, false}};
            llvm::SmallPtrSet<const llvm::Value*, 16> seen;
            while (!pointers.empty())
            {
                const auto [pointer, kept] = pointers.back();
                pointers.pop_back();
                if (!seen.insert(pointer).second)
                    continue;
                for (const auto* user : pointer->users())
                {
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
                        const bool reads = call == &join || kind == CallKind::PThreadJoin ||
                                           kind == CallKind::StdThreadJoin ||
                                           kind == CallKind::StdThreadDtor ||
                                           llvm::isa<llvm::IntrinsicInst>(call);
                        const bool stores =
                            kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor ||
                            buildsEmptyThread(*call) || (fills != nullptr && fills->contains(call));
                        if (!reads && (kept || !stores))
                            return false;
                    }
                    else if (!llvm::isa<llvm::LoadInst>(user) && !llvm::isa<llvm::ICmpInst>(user))
                        return false;
                }
            }
            // A std::thread destroyed on the unwind path of its join is on no normal return.
            const auto reaching = blocksReachingNormalReturn(*writer.getFunction());
            for (const auto& block : *writer.getFunction())
            {
                if (!reaching.contains(&block))
                    continue;
                for (const auto& instruction : block)
                {
                    if (&instruction == &writer || &instruction == &join ||
                        dominators.dominates(&instruction, &writer))
                        continue;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        if (llvm::getUnderlyingObject(store->getPointerOperand()) == base ||
                            (store->getValueOperand()->getType()->isPointerTy() &&
                             llvm::getUnderlyingObject(store->getValueOperand()) == base &&
                             !isPointerLocal(*store->getPointerOperand())))
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
                                !(argument.getOperandNo() == 0 && fillsApart(writer, *call, fills)))
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

        /// Whether `call` calls the member `method` of `std::vector`, read from the callee's scope
        /// and name alone: a member returning `void`, such as `push_back`, or a member template,
        /// such as `emplace_back` before C++17, has a demangled name starting with its return type.
        bool vectorMethod(const llvm::CallBase& call, std::string_view method)
        {
            const llvm::Function* callee = call.getCalledFunction();
            // Only a mangled name holding the class's own name can be one of its members; the
            // check spares demangling every other callee.
            if (callee == nullptr || !callee->getName().contains("vector"))
                return false;
            const std::optional<DemangledName> name = demangleFunction(callee->getName());
            return name.has_value() && name->base == method && isStdClass(name->context, "vector");
        }

        bool iteratorMethod(const llvm::CallBase& call, std::string_view method)
        {
            const std::string name = calledName(call);
            return (name.find("std::__1::__wrap_iter<") != std::string::npos ||
                    name.find("__gnu_cxx::__normal_iterator<") != std::string::npos) &&
                   name.find("operator" + std::string(method)) != std::string::npos;
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
            const llvm::StoreInst* store = nullptr;
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
                            return std::nullopt;
                        const std::optional<ContainerPlace> place = framePlaceOf(
                            call->getArgOperand(0), call->getModule()->getDataLayout());
                        if (!place)
                            return std::nullopt;
                        result = RangeEnd{store, call, object(call->getArgOperand(0)), *place};
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

        /// The two ends of the vector range `loop` reads to join each element, when it traverses
        /// the whole range: an iterator from `begin()` to `end()`, both read before the first
        /// round, advanced by one `++` a round, and serving only the join, through `*` or `->`,
        /// and the loop's test.
        std::optional<std::pair<RangeEnd, RangeEnd>>
        wholeRangeJoin(const JoinSite& join, const llvm::Loop& loop,
                       const llvm::DominatorTree& dominators)
        {
            const auto* dereference = joinedDereference(join);
            if (!dereference || dereference->arg_size() != 1 || !dereferencesIterator(*dereference))
                return std::nullopt;
            const llvm::Value* iterator = object(dereference->getArgOperand(0));
            const std::optional<RangeEnd> begin =
                vectorRangeEnd(iterator, loop, "begin", dominators);
            if (!begin)
                return std::nullopt;
            const auto* branch =
                llvm::dyn_cast<llvm::BranchInst>(loop.getHeader()->getTerminator());
            if (!branch || !branch->isConditional() || !loop.contains(branch->getSuccessor(0)) ||
                loop.contains(branch->getSuccessor(1)))
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
            const std::optional<RangeEnd> end =
                vectorRangeEnd(compare->getArgOperand(1), loop, "end", dominators);
            if (!end || end->place != begin->place)
                return std::nullopt;
            unsigned increments = 0;
            for (const auto& block : *loop.getHeader()->getParent())
                for (const auto& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (!call || call->arg_empty() || object(call->getArgOperand(0)) != iterator)
                        continue;
                    if (iteratorMethod(*call, "++") && loop.contains(call) &&
                        dominators.dominates(call, loop.getLoopLatch()->getTerminator()))
                        ++increments;
                    else if (!dereferencesIterator(*call) && !iteratorMethod(*call, "==") &&
                             !iteratorMethod(*call, "!="))
                        return std::nullopt;
                }
            if (increments != 1)
                return std::nullopt;
            return std::pair{*begin, *end};
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
                       const ConcurrencySymbolClassifier& classifier)
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
            const bool indexed =
                filled != fills.end() && visited && covers(*visited, filled->second) &&
                arrayStorageUnchanged(writer, *join.call, &fills, dominators, classifier);
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

        /// Whether `function` is a member of `std::vector`: what it does to its vector is known by
        /// its name, never read from its body.
        bool memberOfStdVector(const llvm::Function& function)
        {
            if (!function.getName().contains("vector"))
                return false;
            const std::optional<DemangledName> name = demangleFunction(function.getName());
            return name.has_value() && isStdClass(name->context, "vector");
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
                for (unsigned index = 1; index < call->arg_size(); ++index)
                    if (call->getArgOperand(index) == use.copy)
                        return false;
                for (const std::string_view method : methods)
                    if (vectorMethod(*call, method))
                        return true;
                return llvm::is_contained(reads, call);
            }
        };

        constexpr std::string_view kBuildingMethods[] = {"vector", "~vector"};
        constexpr std::string_view kAppendingMethods[] = {"vector", "reserve", "push_back",
                                                          "emplace_back", "~vector"};

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
                     llvm::isa<llvm::IntrinsicInst>(call) || memberOfStdVector(*callee)))
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

        /// How `loop` traverses the whole vector `join` takes its elements from: an iterator from
        /// `begin()` to `end()`, as `wholeRangeJoin` checks it, or an index from 0 below the
        /// vector's `size()`, read before the loop or at each test, one more each round, or below
        /// a constant the vector's size must then be.
        std::optional<VectorTraversal> wholeVectorJoin(const JoinSite& join, const llvm::Loop& loop,
                                                       const llvm::DominatorTree& dominators)
        {
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

        /// Whether the vector at `container` holds exactly `size` threads once the loop inserting
        /// them is over: this function builds it empty, and inserts into it with one call alone,
        /// in a counted loop of `size` rounds, outside any other loop, without an early exit. The
        /// call runs once in every round, and a round it throws in does not go on.
        bool holdsExactly(const ContainerPlace& container, std::int64_t size,
                          const llvm::Function& function, const llvm::LoopInfo& loops,
                          const llvm::DominatorTree& dominators)
        {
            const llvm::DataLayout& layout = function.getParent()->getDataLayout();
            const llvm::CallBase* insertion = nullptr;
            unsigned builds = 0;
            bool builtEmpty = false;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr || call->arg_empty() ||
                        placeOf(call->getArgOperand(0), layout) != container)
                        continue;
                    if (vectorMethod(*call, "vector"))
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
                                                              const llvm::DominatorTree& dominators)
        {
            const llvm::Loop* loop = loops.getLoopFor(join.call->getParent());
            if (loop == nullptr || loop->getLoopLatch() == nullptr || !join.success.has_value() ||
                !join.success->covers(*loop->getLoopLatch()->getTerminator(), dominators))
                return std::nullopt;
            std::optional<VectorTraversal> traversal = wholeVectorJoin(join, *loop, dominators);
            const llvm::CallBase* element = joinedDereference(join);
            if (!traversal || element == nullptr || !servesOnly(*element, {join.call}))
                return std::nullopt;

            // The edge the loop's test exits by: a break entering its block too, having skipped
            // rounds, leaves that block uncovered. A use of the vector past it finds every thread
            // joined.
            const JoinSuccess exit{
                .branch = loop->getHeader(),
                .successor = llvm::cast<llvm::BranchInst>(loop->getHeader()->getTerminator())
                                 ->getSuccessor(1),
            };
            auto pastExit = [&](const llvm::Instruction& use)
            { return exit.covers(use, dominators); };
            if (!onlyAppendedAndReadBy(traversal->container, traversal->reads, pastExit,
                                       join.call->getModule()->getDataLayout()))
                return std::nullopt;
            return VectorCompletion{.traversal = std::move(*traversal), .exit = exit};
        }

        /// Where the thread `create` started has been joined, `insertion` having moved it into a
        /// vector, as `join` proves: its loop joins every thread the vector held when the loop
        /// last read it before starting. Every path from `create` to that last read goes past
        /// `insertion`, which may not run once the read is made: the thread is among those joined.
        std::optional<JoinSuccess> insertedThreadsCompletion(const llvm::CallBase& create,
                                                             const llvm::CallBase& insertion,
                                                             const JoinSite& join,
                                                             const llvm::LoopInfo& loops,
                                                             const llvm::DominatorTree& dominators)
        {
            const std::optional<ContainerPlace> container =
                placeOf(insertion.getArgOperand(0), insertion.getModule()->getDataLayout());
            const std::optional<VectorCompletion> completion =
                wholeVectorCompletion(join, loops, dominators);
            if (!container || !completion || completion->traversal.container != *container ||
                (completion->traversal.size &&
                 !holdsExactly(*container, *completion->traversal.size, *insertion.getFunction(),
                               loops, dominators)))
                return std::nullopt;
            // A thread `insertion` adds is read by the loop before its test can exit: every path
            // from the insertion to that test goes past the loop's last read first, as in a later
            // round of a loop around both, where a new thread is inserted and joined anew.
            if (!passesThrough(insertion, *completion->traversal.lastRead,
                               *completion->exit.branch->getTerminator()) ||
                !passesThrough(create, insertion, *completion->traversal.lastRead))
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
                !servesOnly(*element, {join.call}) || !passesThrough(create, insertion, *element))
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
                    if (vectorMethod(*call, "vector"))
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

        /// A vector a function joins every thread of on every normal return: the one at `offset`
        /// in the object its parameter `index` points to.
        struct JoinedVector
        {
            unsigned index = 0;
            std::int64_t offset = 0;

            friend bool operator==(const JoinedVector&, const JoinedVector&) = default;
        };

        /// The vectors each function this module defines joins every thread of.
        using VectorJoiningHelpers =
            std::unordered_map<const llvm::Function*, std::vector<JoinedVector>>;

        /// The vectors `call` hands to a function joining every thread of them, each through one
        /// argument alone. Any other argument reaching the vector's object would let the function
        /// take a thread out of it unseen; one whose place is unknown does not reach a local the
        /// caller checks never escapes, nor holds at an offset it cannot follow.
        std::vector<ContainerPlace> joinedVectorsOf(const llvm::CallBase& call,
                                                    const VectorJoiningHelpers& joining)
        {
            std::vector<ContainerPlace> joined;
            const auto found = joining.find(call.getCalledFunction());
            if (found == joining.end())
                return joined;
            const llvm::DataLayout& layout = call.getModule()->getDataLayout();
            for (const JoinedVector& vector : found->second)
            {
                if (vector.index >= call.arg_size())
                    continue;
                std::optional<ContainerPlace> place =
                    framePlaceOf(call.getArgOperand(vector.index), layout);
                if (!place)
                    continue;
                bool alone = true;
                for (unsigned index = 0; index < call.arg_size(); ++index)
                {
                    const std::optional<ContainerPlace> other =
                        framePlaceOf(call.getArgOperand(index), layout);
                    if (index != vector.index && other && other->object == place->object)
                        alone = false;
                }
                place->offset += vector.offset;
                if (alone)
                    joined.push_back(*place);
            }
            return joined;
        }

        bool joinsVector(const llvm::Instruction& instruction, const ContainerPlace& container,
                         const VectorJoiningHelpers& joining)
        {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            return call != nullptr &&
                   llvm::is_contained(joinedVectorsOf(*call, joining), container);
        }

        /// Where every thread the vector at `container` holds when `call` is made has been joined:
        /// past the call's return, `call` handing the vector to a function joining every thread of
        /// it. Nothing but such calls reads the vector before: no thread left it unjoined.
        std::optional<JoinSuccess> handedVectorCompletion(const llvm::CallBase& call,
                                                          const ContainerPlace& container,
                                                          const VectorJoiningHelpers& joining,
                                                          const llvm::DominatorTree& dominators)
        {
            if (!joinsVector(call, container, joining))
                return std::nullopt;
            const JoinSuccess returned{.after = &call};
            auto free = [&](const llvm::Instruction& use)
            { return returned.covers(use, dominators) || joinsVector(use, container, joining); };
            if (!onlyAppendedAndReadBy(container, {}, free, call.getModule()->getDataLayout()))
                return std::nullopt;
            return returned;
        }

        /// Where the thread `create` started has been joined, `insertion` having moved it into a
        /// vector `call` hands to a function joining every thread of it: past the call's return.
        /// Every path from `create` to the call goes past `insertion`, and `create` may not run
        /// once the call is made: every thread it starts is in the vector each time the call hands
        /// it over. `insertion` running again past the call could only move in the emptied local.
        std::optional<JoinSuccess> handedThreadsCompletion(const llvm::CallBase& create,
                                                           const llvm::CallBase& insertion,
                                                           const llvm::CallBase& call,
                                                           const VectorJoiningHelpers& joining,
                                                           const llvm::DominatorTree& dominators)
        {
            const std::optional<ContainerPlace> container =
                placeOf(insertion.getArgOperand(0), insertion.getModule()->getDataLayout());
            if (!container || llvm::isPotentiallyReachable(&call, &create, nullptr, &dominators) ||
                !passesThrough(create, insertion, call))
                return std::nullopt;
            return handedVectorCompletion(call, *container, joining, dominators);
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

        /// Records that `function` joins every thread of the vector at `container`, past
        /// `completion`, when the vector lies in an object a parameter points to and the
        /// completion covers every normal return. Whether the record is new.
        bool recordJoinedVector(VectorJoiningHelpers& joining, const llvm::Function& function,
                                const ContainerPlace& container, const JoinSuccess& completion)
        {
            const auto* parameter = llvm::dyn_cast<llvm::Argument>(container.object);
            if (parameter == nullptr ||
                !completionCoversReturns(function.getEntryBlock().front(), completion))
                return false;
            std::vector<JoinedVector>& joined = joining[&function];
            const JoinedVector vector{.index = parameter->getArgNo(), .offset = container.offset};
            if (llvm::is_contained(joined, vector))
                return false;
            joined.push_back(vector);
            return true;
        }

        /// The vectors each function this module defines joins every thread of on every normal
        /// return: through a loop joining each of them, or by handing the vector to a function
        /// already found. The calls are iterated to a fixed point: a destructor handing its object
        /// to the one doing the work is found whatever order the module lists them in.
        VectorJoiningHelpers collectVectorJoiningHelpers(
            const llvm::Module& module, const ConcurrencySymbolClassifier& classifier,
            LlvmFunctionAnalysisProvider& analyses, const JoiningHelpers& helpers)
        {
            VectorJoiningHelpers joining;
            for (const llvm::Function& function : module)
            {
                if (function.isDeclaration())
                    continue;
                for (const JoinSite& join : joinSites(function, classifier, helpers))
                {
                    const std::optional<VectorCompletion> completion = wholeVectorCompletion(
                        join, analyses.getLoopInfo(function), analyses.getDominatorTree(function));
                    // A loop below a constant joins the whole vector only if it holds that many
                    // threads, which the caller alone knows.
                    if (completion && !completion->traversal.size)
                        recordJoinedVector(joining, function, completion->traversal.container,
                                           completion->exit);
                }
            }
            bool changed = !joining.empty();
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
                            for (const ContainerPlace& container : joinedVectorsOf(*call, joining))
                                if (const std::optional<JoinSuccess> returned =
                                        handedVectorCompletion(*call, container, joining,
                                                               analyses.getDominatorTree(function)))
                                    changed = recordJoinedVector(joining, function, container,
                                                                 *returned) ||
                                              changed;
                        }
            }
            return joining;
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
        const VectorJoiningHelpers joining =
            collectVectorJoiningHelpers(module, classifier, analyses, helpers);
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
            std::vector<const llvm::CallBase*> handOffs;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                        call != nullptr && joining.contains(call->getCalledFunction()))
                        handOffs.push_back(call);
            if (joins.empty() && handOffs.empty())
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
            // into is empty past the move, and no join below names it.
            for (const auto* create : creates)
            {
                const llvm::CallBase* insertion =
                    create->arg_empty() ? nullptr : insertionTaking(*create, classifier);
                if (insertion == nullptr)
                    continue;
                auto proves = [&](const std::optional<JoinSuccess>& ended)
                {
                    if (!ended || !completionCoversReturns(*create, *ended))
                        return false;
                    result.emplace(create, ThreadCompletion{.ended = ended});
                    return true;
                };
                bool proven = false;
                for (const JoinSite& join : joins)
                {
                    proven = proven || proves(insertedThreadsCompletion(*create, *insertion, join,
                                                                        loops, dominators));
                    proven = proven || proves(accessedElementCompletion(*create, *insertion, join,
                                                                        dominators));
                }
                for (const llvm::CallBase* call : handOffs)
                    proven = proven || proves(handedThreadsCompletion(*create, *insertion, *call,
                                                                      joining, dominators));
            }
            for (const auto* create : creates)
                for (const JoinSite& join : joins)
                {
                    if (create->arg_empty())
                        continue;
                    if (const std::optional<ThreadCompletion> completion =
                            loopCompletion(*create, handleWriter(*create, classifier), join, fills,
                                           loops, dominators, classifier))
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
