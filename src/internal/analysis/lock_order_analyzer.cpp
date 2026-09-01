// SPDX-License-Identifier: Apache-2.0
#include "lock_order_analyzer.hpp"

#include "fact_queries.hpp"
#include "report_builder.hpp"
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

        void reportCycleIfDeadlocking(DiagnosticReport& report,
                                      const std::vector<const LockOrderFact*>& path,
                                      const std::string& cycleStartLock, const TUFacts& facts,
                                      std::unordered_set<std::string>& emittedCycleKeys)
        {
            const auto cycleBegin =
                std::find_if(path.begin(), path.end(), [&](const LockOrderFact* edge)
                             { return edge->firstLockId == cycleStartLock; });
            if (cycleBegin == path.end())
                return;

            const std::vector<const LockOrderFact*> cycle(cycleBegin, path.end());
            if (cycle.size() < 2)
                return;

            // Every acquisition in the cycle must be able to run in parallel with every other one;
            // otherwise no set of threads can hold the locks simultaneously.
            for (std::size_t lhsIndex = 0; lhsIndex < cycle.size(); ++lhsIndex)
            {
                for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < cycle.size(); ++rhsIndex)
                {
                    if (!mayHappenInParallel(*cycle[lhsIndex], entriesFor(*cycle[lhsIndex], facts),
                                             *cycle[rhsIndex], entriesFor(*cycle[rhsIndex], facts),
                                             facts))
                    {
                        return;
                    }
                }
            }

            if (hasCommonGateLock(cycle))
                return;

            if (!emittedCycleKeys.insert(cycleKey(cycle)).second)
                return;

            emitCycleDiagnostic(report, cycle, facts);
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
        std::unordered_set<std::string> emittedCycleKeys;

        for (const LockOrderFact& fact : facts.lockOrders)
        {
            if (fact.firstLockId != fact.secondLockId)
                continue;

            // A recursive (or error-checking) mutex may legally be reacquired by its owner.
            if (facts.recursiveLockIds.contains(fact.secondLockId))
                continue;

            emitSelfDeadlockDiagnostic(report, fact);
        }

        // Any cycle in the lock-order graph can deadlock, not only the two-lock inversion. Edges
        // are grouped by lock pair so that a cycle of any length is found by a depth-first search.
        std::unordered_map<std::string, std::vector<const LockOrderFact*>> edgesByFirstLock;
        for (const LockOrderFact& fact : facts.lockOrders)
        {
            if (fact.firstLockId == fact.secondLockId || !participatesInConcurrency(fact, facts))
                continue;

            edgesByFirstLock[fact.firstLockId].push_back(&fact);
        }

        std::vector<const LockOrderFact*> currentPath;
        std::unordered_set<std::string> onPath;
        std::unordered_set<std::string> exhausted;

        const std::function<void(const std::string&)> explore =
            [&](const std::string& lockId) -> void
        {
            onPath.insert(lockId);

            const auto edgesIt = edgesByFirstLock.find(lockId);
            if (edgesIt != edgesByFirstLock.end())
            {
                for (const LockOrderFact* edge : edgesIt->second)
                {
                    currentPath.push_back(edge);

                    if (onPath.contains(edge->secondLockId))
                    {
                        reportCycleIfDeadlocking(report, currentPath, edge->secondLockId, facts,
                                                 emittedCycleKeys);
                    }
                    else if (!exhausted.contains(edge->secondLockId))
                    {
                        explore(edge->secondLockId);
                    }

                    currentPath.pop_back();
                }
            }

            onPath.erase(lockId);
            exhausted.insert(lockId);
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
        {
            if (exhausted.contains(lockId))
                continue;

            explore(lockId);
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
