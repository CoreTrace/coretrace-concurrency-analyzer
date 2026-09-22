// SPDX-License-Identifier: Apache-2.0
#include "thread_completion_analysis.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

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

#include <optional>
#include <string>
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

        struct CountedLoop
        {
            const llvm::Value* induction = nullptr;
            const llvm::Value* begin = nullptr;
            const llvm::Value* end = nullptr;
            llvm::CmpInst::Predicate predicate = llvm::CmpInst::BAD_ICMP_PREDICATE;
        };

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

        bool arrayStorageUnchanged(const llvm::CallBase& create, const llvm::CallBase& join,
                                   const llvm::DominatorTree& dominators,
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
                        if (kind != CallKind::PThreadCreate && kind != CallKind::PThreadJoin &&
                            kind != CallKind::StdThreadCtor && kind != CallKind::StdThreadJoin &&
                            kind != CallKind::StdThreadDtor &&
                            !llvm::isa<llvm::IntrinsicInst>(call))
                            return false;
                    }
                    else if (!llvm::isa<llvm::LoadInst>(user))
                        return false;
                }
            }
            for (const auto& block : *create.getFunction())
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
                                llvm::getUnderlyingObject(argument.get()) == base)
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

        const llvm::CallBase* joinedDereference(const llvm::CallBase& join)
        {
            const llvm::Value* value = join.getArgOperand(0);
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

        bool vectorJoinRange(const llvm::CallBase& create, const llvm::CallBase& join,
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
                            if (dominators.dominates(call, &join))
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
            return CountedLoop{slot, begin, end, compare->getPredicate()};
        }

        const llvm::Value* handleAddress(const llvm::CallBase& call, bool join)
        {
            const llvm::Value* value = call.getArgOperand(0);
            if (join)
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
                if (load == nullptr)
                    return nullptr;
                value = load->getPointerOperand();
            }
            return value->stripPointerCasts();
        }

        /// Match the same array element path, with the only varying index being each
        /// loop's induction variable. Equal storage-group strings alone lose that index.
        bool sameIndexedRange(const llvm::Value* left, const llvm::Value* right,
                              const CountedLoop& first, const CountedLoop& second)
        {
            const auto* lhs = llvm::dyn_cast_or_null<llvm::GEPOperator>(left);
            const auto* rhs = llvm::dyn_cast_or_null<llvm::GEPOperator>(right);
            if (lhs == nullptr || rhs == nullptr || lhs->getNumIndices() != rhs->getNumIndices() ||
                lhs->getSourceElementType() != rhs->getSourceElementType() ||
                lhs->getPointerOperand() != rhs->getPointerOperand())
                return false;
            bool indexed = false;
            auto rightIndex = rhs->idx_begin();
            for (const llvm::Use& index : lhs->indices())
            {
                const llvm::Value* a = stripIntegerCasts(index.get());
                const llvm::Value* b = stripIntegerCasts(rightIndex->get());
                ++rightIndex;
                if (llvm::isa<llvm::ConstantInt>(a) && a == b)
                    continue;
                const auto* al = llvm::dyn_cast<llvm::LoadInst>(a);
                const auto* bl = llvm::dyn_cast<llvm::LoadInst>(b);
                if (indexed || al == nullptr || bl == nullptr ||
                    al->getPointerOperand() != first.induction ||
                    bl->getPointerOperand() != second.induction)
                    return false;
                indexed = true;
            }
            return indexed;
        }

        const llvm::Instruction* loopCompletion(const llvm::CallBase& create,
                                                const llvm::CallBase& join,
                                                const llvm::LoopInfo& loops,
                                                const llvm::DominatorTree& dominators,
                                                const ConcurrencySymbolClassifier& classifier)
        {
            const llvm::Loop* first = loops.getLoopFor(create.getParent());
            const llvm::Loop* second = loops.getLoopFor(join.getParent());
            if (first == nullptr || second == nullptr || first == second ||
                first->getLoopLatch() == nullptr || second->getLoopLatch() == nullptr ||
                !dominators.dominates(&create, first->getLoopLatch()->getTerminator()) ||
                !dominators.dominates(&join, second->getLoopLatch()->getTerminator()) ||
                !noEarlyNormalExit(*first) || !noEarlyNormalExit(*second))
                return nullptr;
            const auto a = countedLoop(*first, dominators);
            const auto b = countedLoop(*second, dominators);
            const bool indexed =
                a && b && a->begin == b->begin && a->end == b->end &&
                a->predicate == b->predicate &&
                sameIndexedRange(handleAddress(create, false), handleAddress(join, true), *a, *b) &&
                arrayStorageUnchanged(create, join, dominators, classifier);
            if (!indexed && !(a && vectorJoinRange(create, join, *first, *second, *a, dominators)))
                return nullptr;
            const auto* firstBranch =
                llvm::dyn_cast<llvm::BranchInst>(first->getHeader()->getTerminator());
            const auto* secondBranch =
                llvm::dyn_cast<llvm::BranchInst>(second->getHeader()->getTerminator());
            const llvm::BasicBlock* firstExit =
                firstBranch ? firstBranch->getSuccessor(1) : nullptr;
            const llvm::BasicBlock* secondExit =
                secondBranch ? secondBranch->getSuccessor(1) : nullptr;
            if (firstExit == nullptr || secondExit == nullptr ||
                !dominators.dominates(firstExit, second->getHeader()) ||
                !completionCoversReturns(create, *secondExit->getFirstNonPHIOrDbgOrLifetime()))
                return nullptr;
            return &*secondExit->getFirstNonPHIOrDbgOrLifetime();
        }
    } // namespace

    bool completionCoversReturns(const llvm::Instruction& start,
                                 const llvm::Instruction& completion)
    {
        if (start.getFunction() != completion.getFunction())
            return false;
        if (start.getParent() == completion.getParent())
            return &start == &completion || start.comesBefore(&completion);
        llvm::SmallPtrSet<const llvm::BasicBlock*, 32> visited;
        std::vector<const llvm::BasicBlock*> pending{start.getParent()};
        while (!pending.empty())
        {
            const llvm::BasicBlock* block = pending.back();
            pending.pop_back();
            if (block == completion.getParent() || !visited.insert(block).second)
                continue;
            if (llvm::isa<llvm::ReturnInst>(block->getTerminator()))
                return false;
            for (const llvm::BasicBlock* successor : llvm::successors(block))
                pending.push_back(successor);
        }
        return true;
    }

    ThreadCompletionMap collectThreadCompletions(const llvm::Module& module,
                                                 const ConcurrencySymbolClassifier& classifier,
                                                 LlvmFunctionAnalysisProvider& analyses)
    {
        ThreadCompletionMap result;
        for (const llvm::Function& function : module)
        {
            std::vector<const llvm::CallBase*> creates;
            std::vector<const llvm::CallBase*> joins;
            for (const llvm::BasicBlock& block : function)
                for (const llvm::Instruction& instruction : block)
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        const auto kind = classifier.classify(*call);
                        if (kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor)
                            creates.push_back(call);
                        else if (kind == CallKind::PThreadJoin || kind == CallKind::StdThreadJoin)
                            joins.push_back(call);
                    }
            if (creates.empty() || joins.empty())
                continue;
            const auto& dominators = analyses.getDominatorTree(function);
            const auto& loops = analyses.getLoopInfo(function);
            for (const auto* create : creates)
                for (const auto* join : joins)
                {
                    if (create->arg_empty() || join->arg_empty())
                        continue;
                    if (const auto* barrier =
                            loopCompletion(*create, *join, loops, dominators, classifier))
                    {
                        result.emplace(create, barrier);
                        break;
                    }
                    if (loops.getLoopFor(create->getParent()) != nullptr)
                        continue;
                    const auto first = canonicalStorageGroupId(*create->getArgOperand(0));
                    const auto second = canonicalStorageGroupId(*join->getArgOperand(0));
                    const auto* createAddress = handleAddress(*create, false);
                    const auto* joinAddress =
                        handleAddress(*join, classifier.classify(*join) == CallKind::PThreadJoin);
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
                        arrayStorageUnchanged(*create, *join, dominators, classifier) &&
                        dominators.dominates(create, join) &&
                        completionCoversReturns(*create, *join))
                    {
                        result.emplace(create, join);
                        break;
                    }
                }
        }
        return result;
    }
} // namespace ctrace::concurrency::internal::analysis
