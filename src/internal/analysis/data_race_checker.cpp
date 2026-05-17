// SPDX-License-Identifier: Apache-2.0
#include "data_race_checker.hpp"

#include "fact_queries.hpp"
#include "report_builder.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <map>
#include <sstream>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        using internal::diagnostics::DiagnosticBuilder;
        using EntrySet = ThreadEntrySet;

        bool shareRecognizedLock(const AccessFact& lhs, const AccessFact& rhs)
        {
            if (lhs.heldLocks.empty() || rhs.heldLocks.empty())
                return false;

            return std::any_of(lhs.heldLocks.begin(), lhs.heldLocks.end(),
                               [&](const std::string& lock)
                               { return rhs.heldLocks.contains(lock); });
        }

        std::vector<std::string> sortedLocks(const AccessFact& access)
        {
            return std::vector<std::string>(access.heldLocks.begin(), access.heldLocks.end());
        }

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

        std::string formatLocation(const SourceLocation& location)
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

        bool sameSourceLocation(const SourceLocation& lhs, const SourceLocation& rhs)
        {
            return std::tie(lhs.file, lhs.line, lhs.column, lhs.function) ==
                   std::tie(rhs.file, rhs.line, rhs.column, rhs.function);
        }

        bool hasDistinctLoweredLocation(const AccessFact& access)
        {
            return !sameSourceLocation(access.userLocation, access.loweredLocation);
        }

        bool shareSelfConcurrentEntry(const EntrySet& lhsEntries, const EntrySet& rhsEntries,
                                      const TUFacts& facts)
        {
            for (const std::string& lhsEntry : lhsEntries)
            {
                if (!rhsEntries.contains(lhsEntry))
                    continue;

                const auto it = facts.entryConcurrency.find(lhsEntry);
                if (it != facts.entryConcurrency.end() && it->second.isSelfConcurrent())
                    return true;
            }

            return false;
        }

        std::string conflictKindLabel(AccessKind lhsKind, AccessKind rhsKind)
        {
            if (lhsKind == AccessKind::Write && rhsKind == AccessKind::Write)
                return "write/write";

            if (lhsKind == AccessKind::Read && rhsKind == AccessKind::Read)
                return "read/read";

            return "read/write";
        }

        std::vector<std::string> collectConflictKinds(const AccessFact& lhs, const AccessFact& rhs,
                                                      const EntrySet& lhsEntries,
                                                      const EntrySet& rhsEntries,
                                                      const TUFacts& facts)
        {
            std::set<std::string> conflictKinds;
            conflictKinds.insert(conflictKindLabel(lhs.kind, rhs.kind));

            if (sameSourceLocation(lhs.loweredLocation, rhs.loweredLocation) &&
                shareSelfConcurrentEntry(lhsEntries, rhsEntries, facts) &&
                (lhs.kind == AccessKind::Write || rhs.kind == AccessKind::Write))
            {
                conflictKinds.insert("write/write");
            }

            return std::vector<std::string>(conflictKinds.begin(), conflictKinds.end());
        }

        std::vector<std::string> collectAliasProvenances(const AccessFact& lhs,
                                                         const AccessFact& rhs)
        {
            std::set<std::string> aliasing;
            aliasing.insert(std::string(toString(lhs.aliasProvenance)));
            aliasing.insert(std::string(toString(rhs.aliasProvenance)));
            return std::vector<std::string>(aliasing.begin(), aliasing.end());
        }

        std::vector<std::string> collectAliasProvenances(const AccessFact& access)
        {
            return {std::string(toString(access.aliasProvenance))};
        }

        ConfidenceLevel inferConfidence(const AccessFact& lhs, const AccessFact& rhs)
        {
            if (lhs.aliasProvenance == AliasProvenance::MayAlias ||
                rhs.aliasProvenance == AliasProvenance::MayAlias)
            {
                return ConfidenceLevel::Low;
            }

            if (lhs.aliasProvenance == AliasProvenance::MustAlias ||
                rhs.aliasProvenance == AliasProvenance::MustAlias)
            {
                return ConfidenceLevel::Medium;
            }

            return ConfidenceLevel::High;
        }

        ConfidenceLevel inferConfidence(const AccessFact& access)
        {
            if (access.aliasProvenance == AliasProvenance::MayAlias)
                return ConfidenceLevel::Low;

            if (access.aliasProvenance == AliasProvenance::MustAlias)
                return ConfidenceLevel::Medium;

            return ConfidenceLevel::High;
        }

        std::string describeAccess(const AccessFact& access,
                                   const std::vector<std::string>& entries)
        {
            std::ostringstream stream;
            stream << toString(access.kind) << " at " << formatLocation(access.userLocation);

            if (!entries.empty())
                stream << " (thread entries: " << joinValues(entries) << ")";

            if (!access.heldLocks.empty())
                stream << " under recognized lock(s): " << joinValues(sortedLocks(access));

            return stream.str();
        }

        void emitPairDiagnostic(DiagnosticReport& report, const AccessFact& lhs,
                                const AccessFact& rhs, const EntrySet& lhsEntries,
                                const EntrySet& rhsEntries, const TUFacts& facts)
        {
            const std::vector<std::string> orderedLhsEntries = sortedThreadEntries(lhsEntries);
            const std::vector<std::string> orderedRhsEntries = sortedThreadEntries(rhsEntries);
            const std::vector<std::string> conflictKinds =
                collectConflictKinds(lhs, rhs, lhsEntries, rhsEntries, facts);
            const std::vector<std::string> variableAliasing = collectAliasProvenances(lhs, rhs);

            DiagnosticBuilder builder(report, RuleId::DataRaceGlobal);
            builder.confidence(inferConfidence(lhs, rhs))
                .primaryLocation(lhs.userLocation)
                .relatedLocation("Conflicting access", rhs.userLocation)
                .message("unsynchronized concurrent access to global '" + lhs.symbol + "'")
                .note("first access: " + describeAccess(lhs, orderedLhsEntries))
                .note("conflicting access: " + describeAccess(rhs, orderedRhsEntries))
                .note("possible conflict kinds: " + joinValues(conflictKinds))
                .note("no common recognized lock protects the conflicting accesses")
                .property("symbol", lhs.symbol)
                .property("firstAccessKind", std::string(toString(lhs.kind)))
                .property("secondAccessKind", std::string(toString(rhs.kind)))
                .property("firstAliasProvenance", std::string(toString(lhs.aliasProvenance)))
                .property("secondAliasProvenance", std::string(toString(rhs.aliasProvenance)))
                .property("firstProtected", !lhs.heldLocks.empty())
                .property("secondProtected", !rhs.heldLocks.empty())
                .property("firstThreadEntries", orderedLhsEntries)
                .property("secondThreadEntries", orderedRhsEntries)
                .property("conflictKinds", conflictKinds)
                .property("variableAliasing", variableAliasing);

            if (hasDistinctLoweredLocation(lhs))
                builder.relatedLocation("Lowered first access", lhs.loweredLocation);
            if (hasDistinctLoweredLocation(rhs))
                builder.relatedLocation("Lowered conflicting access", rhs.loweredLocation);

            builder.emit();
        }

        void emitSelfConcurrentDiagnostic(DiagnosticReport& report, const AccessFact& access,
                                          const EntrySet& entries)
        {
            const std::vector<std::string> orderedEntries = sortedThreadEntries(entries);
            const std::string entryLabel =
                orderedEntries.empty() ? access.functionId : joinValues(orderedEntries);
            const std::vector<std::string> conflictKinds = {"write/write"};
            const std::vector<std::string> variableAliasing = collectAliasProvenances(access);

            DiagnosticBuilder builder(report, RuleId::DataRaceGlobal);
            builder.confidence(inferConfidence(access))
                .primaryLocation(access.userLocation)
                .relatedLocation("Concurrent invocation", access.userLocation)
                .message("unsynchronized concurrent access to global '" + access.symbol + "'")
                .note("access: " + describeAccess(access, orderedEntries))
                .note("conflicts with another concurrent invocation reachable from thread entry "
                      "'" +
                      entryLabel + "'")
                .note("possible conflict kinds: " + joinValues(conflictKinds))
                .note("no common recognized lock protects the conflicting accesses")
                .property("symbol", access.symbol)
                .property("firstAccessKind", std::string(toString(access.kind)))
                .property("secondAccessKind", std::string(toString(access.kind)))
                .property("firstAliasProvenance", std::string(toString(access.aliasProvenance)))
                .property("secondAliasProvenance", std::string(toString(access.aliasProvenance)))
                .property("firstProtected", !access.heldLocks.empty())
                .property("secondProtected", !access.heldLocks.empty())
                .property("firstThreadEntries", orderedEntries)
                .property("secondThreadEntries", orderedEntries)
                .property("conflictKinds", conflictKinds)
                .property("variableAliasing", variableAliasing);

            if (hasDistinctLoweredLocation(access))
                builder.relatedLocation("Lowered access", access.loweredLocation);

            builder.emit();
        }

    } // namespace

    DiagnosticReport DataRaceChecker::run(const llvm::Module& module, const TUFacts& facts) const
    {
        (void)module;
        const std::unordered_map<std::string, ThreadEntrySet>& reachableEntriesByFunction =
            facts.reachableThreadEntriesByFunction;

        std::map<std::string, std::vector<const AccessFact*>> accessesBySymbol;
        for (const AccessFact& access : facts.accesses)
        {
            if (!reachableEntriesByFunction.contains(access.functionId))
                continue;
            accessesBySymbol[access.symbol].push_back(&access);
        }

        DiagnosticReport report;
        for (const auto& [symbol, accesses] : accessesBySymbol)
        {
            (void)symbol;
            bool foundPairDiagnostic = false;
            for (std::size_t lhsIndex = 0; lhsIndex < accesses.size(); ++lhsIndex)
            {
                for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < accesses.size(); ++rhsIndex)
                {
                    const AccessFact& lhs = *accesses[lhsIndex];
                    const AccessFact& rhs = *accesses[rhsIndex];

                    if (lhs.kind != AccessKind::Write && rhs.kind != AccessKind::Write)
                        continue;

                    const EntrySet& lhsEntries = reachableEntriesByFunction.at(lhs.functionId);
                    const EntrySet& rhsEntries = reachableEntriesByFunction.at(rhs.functionId);
                    if (!mayRunConcurrently(lhsEntries, rhsEntries, facts))
                        continue;

                    if (shareRecognizedLock(lhs, rhs))
                        continue;

                    emitPairDiagnostic(report, lhs, rhs, lhsEntries, rhsEntries, facts);
                    foundPairDiagnostic = true;
                }
            }

            if (foundPairDiagnostic)
                continue;

            for (const AccessFact* access : accesses)
            {
                if (access->kind != AccessKind::Write)
                    continue;

                const EntrySet& entries = reachableEntriesByFunction.at(access->functionId);
                if (!isSelfConcurrent(entries, facts))
                    continue;

                if (shareRecognizedLock(*access, *access))
                    continue;

                emitSelfConcurrentDiagnostic(report, *access, entries);
                break;
            }
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
