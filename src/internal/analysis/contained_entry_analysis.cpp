// SPDX-License-Identifier: Apache-2.0
#include "contained_entry_analysis.hpp"

#include "concurrency_symbol_classifier.hpp"
#include "interprocedural_bindings.hpp"
#include "ir_utils.hpp"
#include "llvm_function_analysis_provider.hpp"

#include <llvm/IR/Dominators.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

#include <functional>
#include <unordered_map>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        struct Frame
        {
            const llvm::Instruction* start = nullptr;
            const llvm::Instruction* completion = nullptr;
        };

        struct Occurrence
        {
            const llvm::Function* entry = nullptr;
            // Innermost spawn first, outermost caller last. Context is kept until all
            // occurrences have been checked: one sequential call must not hide another
            // concurrently executing instance of the same entry.
            std::vector<Frame> frames;
        };

        bool ordered(const Occurrence& first, const Occurrence& second,
                     LlvmFunctionAnalysisProvider& analyses)
        {
            auto a = first.frames.rbegin();
            auto b = second.frames.rbegin();
            while (a != first.frames.rend() && b != second.frames.rend() && a->start == b->start)
            {
                ++a;
                ++b;
            }
            if (a == first.frames.rend() || b == second.frames.rend() ||
                a->start->getFunction() != b->start->getFunction())
                return false;
            const auto& dominators = analyses.getDominatorTree(*a->start->getFunction());
            return (a->completion && dominators.dominates(a->completion, b->start)) ||
                   (b->completion && dominators.dominates(b->completion, a->start));
        }
    } // namespace

    std::unordered_set<EntryPair, EntryPairHash>
    collectContainedEntryPairs(const llvm::Module& module, const std::vector<DirectCallSite>& calls,
                               const ThreadCompletionMap& completions,
                               const ConcurrencySymbolClassifier& classifier,
                               LlvmFunctionAnalysisProvider& analyses)
    {
        std::unordered_set<const llvm::Function*> relevant;
        for (const auto& function : module)
            for (const auto& block : function)
                for (const auto& instruction : block)
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction))
                    {
                        const auto kind = classifier.classify(*call);
                        if (kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor)
                            relevant.insert(&function);
                    }
        bool changed = true;
        while (changed)
        {
            changed = false;
            for (const auto& site : calls)
                if (site.call && relevant.contains(classifier.directCallee(*site.call)))
                    changed = relevant.insert(site.call->getFunction()).second || changed;
        }
        std::unordered_set<const llvm::Function*> called;
        for (const auto& site : calls)
            if (site.call && relevant.contains(site.call->getFunction()))
            {
                const auto kind = classifier.classify(*site.call);
                if (kind != CallKind::PThreadCreate && kind != CallKind::StdThreadCtor)
                    called.insert(classifier.directCallee(*site.call));
            }

        using Bindings = std::unordered_map<unsigned, const llvm::Function*>;
        std::unordered_set<const llvm::Function*> active;
        std::unordered_set<const llvm::Function*> visited;
        bool complete = true;
        // A bounded expansion is a precision limit, never a proof. On overflow all new
        // sequencing conclusions are discarded. The ordinary may-concurrency facts remain.
        constexpr std::size_t kMaxOccurrences = 4096;
        std::size_t occurrenceCount = 0;
        auto bind = [](const llvm::Value& value, const Bindings& arguments) -> const llvm::Function*
        {
            const auto binding = resolveFunctionBinding(value);
            if (!binding)
                return nullptr;
            if (binding->function)
                return binding->function;
            if (binding->argumentIndex)
            {
                const auto found = arguments.find(*binding->argumentIndex);
                if (found != arguments.end())
                    return found->second;
            }
            return nullptr;
        };

        std::function<std::vector<Occurrence>(const llvm::Function&, const Bindings&)> expand;
        expand = [&](const llvm::Function& function, const Bindings& arguments)
        {
            std::vector<Occurrence> result;
            if (!complete || !active.insert(&function).second)
            {
                complete = false;
                return result;
            }
            visited.insert(&function);
            const auto& dominators = analyses.getDominatorTree(function);
            for (const auto& block : function)
            {
                if (!dominators.isReachableFromEntry(&block))
                    continue;
                for (const auto& instruction : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
                    if (call == nullptr)
                        continue;
                    const auto kind = classifier.classify(*call);
                    if (kind == CallKind::PThreadCreate || kind == CallKind::StdThreadCtor)
                    {
                        const unsigned operand = kind == CallKind::PThreadCreate ? 2 : 1;
                        if (call->arg_size() <= operand)
                            continue;
                        const auto* entry = bind(*call->getArgOperand(operand), arguments);
                        if (entry == nullptr || ++occurrenceCount > kMaxOccurrences)
                        {
                            complete = false;
                            continue;
                        }
                        const auto found = completions.find(call);
                        result.push_back(Occurrence{
                            entry, {{call, found == completions.end() ? nullptr : found->second}}});
                        continue;
                    }
                    const auto* callee = classifier.directCallee(*call);
                    if (callee == nullptr || !relevant.contains(callee))
                        continue;
                    Bindings bound;
                    for (unsigned index = 0; index < call->arg_size(); ++index)
                        if (const auto* entry = bind(*call->getArgOperand(index), arguments))
                            bound.emplace(index, entry);
                    auto inner = expand(*callee, bound);
                    for (auto& occurrence : inner)
                    {
                        const Frame& frame = occurrence.frames.back();
                        const bool contained =
                            frame.completion &&
                            completionCoversReturns(*frame.start, *frame.completion);
                        const llvm::Instruction* completedCall = call;
                        if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(call))
                            completedCall =
                                &*invoke->getNormalDest()->getFirstNonPHIOrDbgOrLifetime();
                        occurrence.frames.push_back({call, contained ? completedCall : nullptr});
                        result.push_back(std::move(occurrence));
                    }
                }
            }
            active.erase(&function);
            return result;
        };

        std::vector<Occurrence> occurrences;
        for (const auto* function : relevant)
            if (!called.contains(function))
            {
                auto root = expand(*function, {});
                occurrences.insert(occurrences.end(), std::make_move_iterator(root.begin()),
                                   std::make_move_iterator(root.end()));
            }
        if (!complete)
            return {};

        // Helpers invoked by workers can execute concurrently even when their own bodies
        // are sequential. Such roots cannot contribute a global never-overlaps statement.
        std::unordered_set<const llvm::Function*> workerReachable;
        for (const auto& occurrence : occurrences)
            workerReachable.insert(occurrence.entry);
        changed = true;
        while (changed)
        {
            changed = false;
            for (const auto& site : calls)
                if (site.call && workerReachable.contains(site.call->getFunction()))
                    if (const auto* callee = classifier.directCallee(*site.call))
                        changed = workerReachable.insert(callee).second || changed;
        }
        for (const auto* function : relevant)
            if (!visited.contains(function) && workerReachable.contains(function))
                return {};

        std::unordered_set<EntryPair, EntryPairHash> proved;
        std::unordered_set<EntryPair, EntryPairHash> uncertain;
        for (std::size_t i = 0; i < occurrences.size(); ++i)
            for (std::size_t j = i + 1; j < occurrences.size(); ++j)
            {
                const auto& a = occurrences[i];
                const auto& b = occurrences[j];
                if (a.entry == b.entry)
                    continue;
                const auto pair = makeEntryPair(functionId(*a.entry), functionId(*b.entry));
                const bool inWorker =
                    workerReachable.contains(a.frames.back().start->getFunction()) ||
                    workerReachable.contains(b.frames.back().start->getFunction());
                if (!inWorker && ordered(a, b, analyses))
                    proved.insert(pair);
                else
                    uncertain.insert(pair);
            }
        for (const auto& pair : uncertain)
            proved.erase(pair);
        return proved;
    }
} // namespace ctrace::concurrency::internal::analysis
