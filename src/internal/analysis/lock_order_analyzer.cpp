// SPDX-License-Identifier: Apache-2.0
#include "lock_order_analyzer.hpp"

#include "fact_queries.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <algorithm>
#include <functional>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        using internal::diagnostics::DiagnosticBuilder;

        std::string joinValues(const std::vector<std::string>& values)
        {
            std::ostringstream stream;
            for (std::size_t index = 0; index < values.size(); ++index)
            {
                if (index != 0)
                    stream << ", ";
                stream << values[index];
            }
            return stream.str();
        }

        std::string locationLabel(const SourceLocation& location)
        {
            std::ostringstream stream;
            if (!location.file.empty())
                stream << location.file;
            else
                stream << "<unknown-file>";

            if (location.line != 0)
                stream << ":" << location.line << ":" << location.column;

            if (!location.function.empty())
                stream << " in " << location.function;
            return stream.str();
        }

        const ThreadEntrySet& entriesFor(const LockOrderFact& fact, const TUFacts& facts)
        {
            static const ThreadEntrySet emptyEntries;
            const auto it = facts.reachableThreadEntriesByFunction.find(fact.functionId);
            return it != facts.reachableThreadEntriesByFunction.end() ? it->second : emptyEntries;
        }

        /// Only an acquisition some task can execute may take part in a deadlock.
        bool participatesInConcurrency(const LockOrderFact& fact, const TUFacts& facts)
        {
            return facts.reachableThreadEntriesByFunction.contains(fact.functionId) ||
                   fact.inRootTask;
        }

        std::vector<std::string> contextLabel(const LockOrderFact& fact, const TUFacts& facts)
        {
            std::vector<std::string> entries = sortedThreadEntries(entriesFor(fact, facts));
            if (entries.empty() && fact.inRootTask)
                entries.push_back(rootTaskId());
            return entries;
        }

        void emitCycleDiagnostic(DiagnosticReport& report,
                                 const std::vector<const LockOrderFact*>& cycle,
                                 const TUFacts& facts)
        {
            const LockOrderFact& first = *cycle.front();
            const bool isInversion = cycle.size() == 2;

            DiagnosticBuilder builder(report, RuleId::DeadlockLockOrder);
            builder.primaryLocation(first.location)
                .message(isInversion
                             ? "potential deadlock caused by inconsistent lock acquisition order"
                             : "potential deadlock caused by a cycle of " +
                                   std::to_string(cycle.size()) + " lock acquisitions");

            std::vector<std::string> cycleLocks;
            for (std::size_t index = 0; index < cycle.size(); ++index)
            {
                const LockOrderFact& edge = *cycle[index];
                cycleLocks.push_back(edge.firstLockId);

                const std::string label =
                    index == 0 ? std::string("first order") : std::string("conflicting order");
                builder.note(label + ": acquire '" + edge.secondLockId + "' while holding '" +
                             edge.firstLockId + "' at " + locationLabel(edge.location) +
                             " (thread entries: " + joinValues(contextLabel(edge, facts)) + ")");

                if (index != 0)
                    builder.relatedLocation("Conflicting lock order", edge.location);
            }

            builder.property("firstLock", first.firstLockId)
                .property("secondLock", first.secondLockId)
                .property("cycleLocks", cycleLocks)
                .property("firstThreadEntries", contextLabel(first, facts))
                .property("secondThreadEntries", contextLabel(*cycle.back(), facts))
                .emit();
        }

        /// Locks held at every acquisition of the cycle, excluding the cycle's own locks. Such an
        /// outer "gate" serializes the whole cycle, so the inconsistent order can never deadlock.
        bool hasCommonGateLock(const std::vector<const LockOrderFact*>& cycle)
        {
            std::set<std::string> cycleLocks;
            for (const LockOrderFact* edge : cycle)
            {
                cycleLocks.insert(edge->firstLockId);
                cycleLocks.insert(edge->secondLockId);
            }

            std::optional<std::set<std::string>> gates;
            for (const LockOrderFact* edge : cycle)
            {
                std::set<std::string> candidates;
                for (const std::string& heldLock : edge->heldLocks)
                {
                    if (!cycleLocks.contains(heldLock))
                        candidates.insert(heldLock);
                }

                if (!gates.has_value())
                {
                    gates = std::move(candidates);
                    continue;
                }

                std::set<std::string> intersection;
                std::set_intersection(gates->begin(), gates->end(), candidates.begin(),
                                      candidates.end(),
                                      std::inserter(intersection, intersection.end()));
                gates = std::move(intersection);
            }

            return gates.has_value() && !gates->empty();
        }

        /// Each order is taken only when its first lock's object sits at the lower address, so the
        /// locks of the cycle would each have to lie below the next, all around it: no layout
        /// closes it. One order not proven so is enough to keep the choice, and another choice of
        /// orders on the same locks, or another cycle through them, is judged on its own.
        bool isAddressOrdered(const std::vector<const LockOrderFact*>& cycle)
        {
            for (const LockOrderFact* order : cycle)
            {
                if (!order->lowerAddressFirst)
                    return false;
            }
            return true;
        }

        std::string cycleKey(const std::vector<const LockOrderFact*>& cycle)
        {
            std::set<std::string> nodes;
            for (const LockOrderFact* edge : cycle)
                nodes.insert(edge->firstLockId + "->" + edge->secondLockId);

            std::ostringstream stream;
            for (const std::string& node : nodes)
                stream << node << "||";
            return stream.str();
        }

        using EdgesByFirstLock = std::unordered_map<std::string, std::vector<const LockOrderFact*>>;

        /// The orders from one lock to another, in the order they were collected.
        struct LockSuccessor
        {
            std::size_t lock = 0;
            std::vector<const LockOrderFact*> orders;
        };

        /// The successors of each lock, locks being numbered from 0.
        using LockGraph = std::vector<std::vector<LockSuccessor>>;

        /// Numbers the locks in the order a depth-first walk first reaches them, starting from the
        /// locks in name order and following orders in the order they were collected. A cycle is
        /// reported from its lowest-numbered lock, the first of its locks that walk reaches.
        LockGraph buildLockGraph(const EdgesByFirstLock& edgesByFirstLock)
        {
            std::unordered_map<std::string, std::size_t> numbers;
            const std::function<void(const std::string&)> number = [&](const std::string& lockId)
            {
                if (!numbers.try_emplace(lockId, numbers.size()).second)
                    return;

                const auto edgesIt = edgesByFirstLock.find(lockId);
                if (edgesIt == edgesByFirstLock.end())
                    return;
                for (const LockOrderFact* edge : edgesIt->second)
                    number(edge->secondLockId);
            };

            std::vector<std::string> orderedRoots;
            orderedRoots.reserve(edgesByFirstLock.size());
            for (const auto& [lockId, edges] : edgesByFirstLock)
            {
                (void)edges;
                orderedRoots.push_back(lockId);
            }
            std::sort(orderedRoots.begin(), orderedRoots.end());
            for (const std::string& lockId : orderedRoots)
                number(lockId);

            LockGraph graph(numbers.size());
            for (const auto& [lockId, edges] : edgesByFirstLock)
            {
                std::vector<LockSuccessor>& successors = graph[numbers.at(lockId)];
                std::unordered_map<std::size_t, std::size_t> positions;
                for (const LockOrderFact* edge : edges)
                {
                    const std::size_t target = numbers.at(edge->secondLockId);
                    const auto [position, inserted] =
                        positions.try_emplace(target, successors.size());
                    if (inserted)
                        successors.push_back(LockSuccessor{.lock = target});
                    successors[position->second].orders.push_back(edge);
                }
            }
            return graph;
        }

        /// Every acquisition in the cycle must be able to run in parallel with every other one;
        /// otherwise no set of threads can hold the locks simultaneously.
        bool mayHappenInParallelWithEach(const LockOrderFact& order,
                                         const std::vector<const LockOrderFact*>& taken,
                                         const TUFacts& facts)
        {
            for (const LockOrderFact* other : taken)
            {
                if (!mayHappenInParallel(*other, entriesFor(*other, facts), order,
                                         entriesFor(order, facts), facts))
                {
                    return false;
                }
            }
            return true;
        }

        /// Orders the walk from one lock may examine. A dense lock hierarchy that a few orders
        /// invert closes more cycles than a report can list: past this many, the walk moves on to
        /// the next lock. A cycle through this one may then go unreported, and the report says
        /// the search is incomplete.
        constexpr std::size_t kMaxOrdersExaminedPerLock = std::size_t{1} << 16;

        /// Reports each elementary cycle of `graph` once, when some choice of one order between
        /// each two successive locks may deadlock. Two locks may be joined by several orders taken
        /// in different contexts: one taken before a thread exists, or under a gate, must hide
        /// none that is not.
        ///
        /// From each lock `start` in turn, a depth-first walk follows orders toward the locks
        /// numbered after `start` that lead back to it, and closes a cycle at each order into it.
        /// It takes an order only when that order may run in parallel with each one already on
        /// the path: no cycle extending the path could deadlock otherwise. Orders are tried in the
        /// order they were collected, so a cycle is reported with the first choice that passes.
        ///
        /// False when the walk from some lock stopped at the bound.
        bool reportDeadlockingCycles(DiagnosticReport& report, const LockGraph& graph,
                                     const TUFacts& facts)
        {
            std::vector<std::vector<std::size_t>> predecessors(graph.size());
            for (std::size_t lock = 0; lock < graph.size(); ++lock)
            {
                for (const LockSuccessor& successor : graph[lock])
                    predecessors[successor.lock].push_back(lock);
            }

            std::size_t start = 0;
            std::vector<bool> leadsToStart(graph.size());
            std::vector<bool> onPath(graph.size());
            std::vector<const LockOrderFact*> path;
            std::unordered_set<std::string> emittedCycleKeys;
            const auto emitOnce = [&](const std::vector<const LockOrderFact*>& cycle)
            {
                if (emittedCycleKeys.insert(cycleKey(cycle)).second)
                    emitCycleDiagnostic(report, cycle, facts);
            };
            // The first address-ordered choice closing each cycle, until the search is known to
            // have finished.
            std::vector<std::vector<const LockOrderFact*>> addressOrderedCycles;
            std::unordered_set<std::string> addressOrderedKeys;
            std::size_t examined = 0;

            // False once the walk from `start` has examined its share of orders.
            const std::function<bool(std::size_t)> walk = [&](std::size_t lock) -> bool
            {
                for (const LockSuccessor& successor : graph[lock])
                {
                    const bool closes = successor.lock == start;
                    if (!closes && (!leadsToStart[successor.lock] || onPath[successor.lock]))
                        continue;

                    for (const LockOrderFact* order : successor.orders)
                    {
                        if (++examined > kMaxOrdersExaminedPerLock)
                            return false;
                        if (!mayHappenInParallelWithEach(*order, path, facts))
                            continue;

                        path.push_back(order);
                        bool withinShare = true;
                        if (!closes)
                        {
                            onPath[successor.lock] = true;
                            withinShare = walk(successor.lock);
                            onPath[successor.lock] = false;
                        }
                        else if (!hasCommonGateLock(path))
                        {
                            if (!isAddressOrdered(path))
                                emitOnce(path);
                            else if (addressOrderedKeys.insert(cycleKey(path)).second)
                                addressOrderedCycles.push_back(path);
                        }
                        path.pop_back();

                        if (!withinShare)
                            return false;
                    }
                }
                return true;
            };

            bool complete = true;
            for (; start < graph.size(); ++start)
            {
                std::fill(leadsToStart.begin(), leadsToStart.end(), false);
                std::vector<std::size_t> pending{start};
                while (!pending.empty())
                {
                    const std::size_t reached = pending.back();
                    pending.pop_back();
                    for (const std::size_t predecessor : predecessors[reached])
                    {
                        if (predecessor > start && !leadsToStart[predecessor])
                        {
                            leadsToStart[predecessor] = true;
                            pending.push_back(predecessor);
                        }
                    }
                }
                examined = 0;
                if (!walk(start))
                    complete = false;
            }

            // Skipping an address-ordered choice relies on every other cycle through its locks
            // being judged on its own. Past the bound one may have been lost, and the choice is
            // reported in its place.
            if (!complete)
            {
                for (const std::vector<const LockOrderFact*>& cycle : addressOrderedCycles)
                    emitOnce(cycle);
            }
            return complete;
        }

        void emitSelfDeadlockDiagnostic(DiagnosticReport& report, const LockOrderFact& fact)
        {
            DiagnosticBuilder(report, RuleId::DeadlockLockOrder)
                .primaryLocation(fact.location)
                .relatedLocation("Reacquired lock", fact.location)
                .message("potential deadlock caused by reacquiring a non-recursive lock")
                .note("reacquires '" + fact.secondLockId + "' while it is already held at " +
                      locationLabel(fact.location))
                .property("firstLock", fact.firstLockId)
                .property("secondLock", fact.secondLockId)
                .emit();
        }
    } // namespace

    DiagnosticReport LockOrderAnalyzer::run(const TUFacts& facts) const
    {
        DiagnosticReport report;

        for (const LockOrderFact& fact : facts.lockOrders)
        {
            if (fact.firstLockId != fact.secondLockId)
                continue;

            // A recursive (or error-checking) mutex may legally be reacquired by its owner.
            if (facts.recursiveLockIds.contains(fact.secondLockId))
                continue;

            emitSelfDeadlockDiagnostic(report, fact);
        }

        // Any cycle in the lock-order graph can deadlock, not only the two-lock inversion. Each
        // elementary cycle is judged once, whatever lock the search reaches it by.
        EdgesByFirstLock edgesByFirstLock;
        for (const LockOrderFact& fact : facts.lockOrders)
        {
            if (fact.firstLockId == fact.secondLockId || !participatesInConcurrency(fact, facts))
                continue;

            edgesByFirstLock[fact.firstLockId].push_back(&fact);
        }

        if (!reportDeadlockingCycles(report, buildLockGraph(edgesByFirstLock), facts))
        {
            report.notices.push_back(AnalysisNotice{
                .id = "cycle-search-limit-reached",
                .ruleId = RuleId::DeadlockLockOrder,
                .message = "lock-order cycle search incomplete: exploration limit reached; some "
                           "lock-order cycles may not be reported",
            });
        }

        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
