// SPDX-License-Identifier: Apache-2.0
#include "lock_order_analyzer.hpp"

#include "fact_queries.hpp"
#include "report_builder.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <algorithm>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_set>

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

        std::string pairKey(const LockOrderFact& lhs, const LockOrderFact& rhs)
        {
            const auto lhsKey = std::make_tuple(lhs.functionId, lhs.firstLockId, lhs.secondLockId);
            const auto rhsKey = std::make_tuple(rhs.functionId, rhs.firstLockId, rhs.secondLockId);
            const auto normalized = std::minmax(lhsKey, rhsKey);

            std::ostringstream stream;
            stream << std::get<0>(normalized.first) << "|" << std::get<1>(normalized.first) << "|"
                   << std::get<2>(normalized.first) << "||" << std::get<0>(normalized.second) << "|"
                   << std::get<1>(normalized.second) << "|" << std::get<2>(normalized.second);
            return stream.str();
        }

        void emitCycleDiagnostic(DiagnosticReport& report, const LockOrderFact& lhs,
                                 const LockOrderFact& rhs, const TUFacts& facts)
        {
            const ThreadEntrySet& lhsEntries =
                facts.reachableThreadEntriesByFunction.at(lhs.functionId);
            const ThreadEntrySet& rhsEntries =
                facts.reachableThreadEntriesByFunction.at(rhs.functionId);
            const std::vector<std::string> orderedLhsEntries = sortedThreadEntries(lhsEntries);
            const std::vector<std::string> orderedRhsEntries = sortedThreadEntries(rhsEntries);

            DiagnosticBuilder(report, RuleId::DeadlockLockOrder)
                .primaryLocation(lhs.location)
                .relatedLocation("Conflicting lock order", rhs.location)
                .message("potential deadlock caused by inconsistent lock acquisition order")
                .note("first order: acquire '" + lhs.secondLockId + "' while holding '" +
                      lhs.firstLockId + "' at " + locationLabel(lhs.location) +
                      " (thread entries: " + joinValues(orderedLhsEntries) + ")")
                .note("conflicting order: acquire '" + rhs.secondLockId + "' while holding '" +
                      rhs.firstLockId + "' at " + locationLabel(rhs.location) +
                      " (thread entries: " + joinValues(orderedRhsEntries) + ")")
                .property("firstLock", lhs.firstLockId)
                .property("secondLock", lhs.secondLockId)
                .property("firstThreadEntries", orderedLhsEntries)
                .property("secondThreadEntries", orderedRhsEntries)
                .emit();
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
        std::unordered_set<std::string> emittedPairKeys;

        for (std::size_t lhsIndex = 0; lhsIndex < facts.lockOrders.size(); ++lhsIndex)
        {
            const LockOrderFact& lhs = facts.lockOrders[lhsIndex];
            if (lhs.firstLockId == lhs.secondLockId)
            {
                emitSelfDeadlockDiagnostic(report, lhs);
                continue;
            }

            const auto lhsEntriesIt = facts.reachableThreadEntriesByFunction.find(lhs.functionId);
            if (lhsEntriesIt == facts.reachableThreadEntriesByFunction.end())
                continue;

            for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < facts.lockOrders.size();
                 ++rhsIndex)
            {
                const LockOrderFact& rhs = facts.lockOrders[rhsIndex];
                if (lhs.firstLockId != rhs.secondLockId || lhs.secondLockId != rhs.firstLockId)
                    continue;

                const auto rhsEntriesIt =
                    facts.reachableThreadEntriesByFunction.find(rhs.functionId);
                if (rhsEntriesIt == facts.reachableThreadEntriesByFunction.end())
                    continue;

                if (!mayRunConcurrently(lhsEntriesIt->second, rhsEntriesIt->second, facts))
                    continue;

                const std::string key = pairKey(lhs, rhs);
                if (!emittedPairKeys.insert(key).second)
                    continue;

                emitCycleDiagnostic(report, lhs, rhs, facts);
            }
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
