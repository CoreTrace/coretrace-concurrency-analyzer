// SPDX-License-Identifier: Apache-2.0
#include "data_race_checker.hpp"

#include "fact_queries.hpp"
#include "ir_utils.hpp"
#include "report_builder.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>
#include <set>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        using internal::diagnostics::DiagnosticBuilder;
        using EntrySet = ThreadEntrySet;

        /// Number of distinct conflicting sites listed alongside the representative pair before the
        /// diagnostic stops enumerating them.
        constexpr std::size_t kMaxRelatedConflictSites = 6;

        bool shareRecognizedLock(const AccessFact& lhs, const AccessFact& rhs)
        {
            if (lhs.heldLocks.empty() || rhs.heldLocks.empty())
                return false;

            return std::any_of(lhs.heldLocks.begin(), lhs.heldLocks.end(),
                               [&](const std::string& lock)
                               { return rhs.heldLocks.contains(lock); });
        }

        /// Two atomic operations on the same location are ordered by the memory model and never
        /// form a data race. A mix of atomic and plain access still does, and is worth reporting
        /// with its own wording because the bug is the unsynchronized side.
        bool isRaceFreeAtomicPair(const AccessFact& lhs, const AccessFact& rhs)
        {
            return lhs.isAtomic && rhs.isAtomic;
        }

        bool isMixedAtomicPair(const AccessFact& lhs, const AccessFact& rhs)
        {
            return lhs.isAtomic != rhs.isAtomic;
        }

        /// A mixed atomic/plain conflict is only a bug when both sides reach the same location:
        /// the atomic side orders nothing for a *different* field. An effect inferred for a call
        /// has no known extent, so it cannot establish that, and pairing it with a plain access
        /// to a neighbouring field would invent a race between two distinct members.
        bool hasPreciseRegions(const AccessFact& lhs, const AccessFact& rhs)
        {
            if (lhs.coarseCallEffect || rhs.coarseCallEffect)
                return false;

            return lhs.region.hasKnownOffset && rhs.region.hasKnownOffset &&
                   lhs.region.byteSize != 0 && rhs.region.byteSize != 0;
        }

        std::string describeSymbol(const AccessFact& access)
        {
            return access.symbol;
        }

        /// What the conflict is about, said the way the reader can act on it.
        ///
        /// An object handed to a thread has no name of its own: its identity is the storage the
        /// pointer was read from, which is an internal label. Printing it as if it were a global
        /// would name something the reader cannot find in the source.
        std::string describeConflictSubject(const AccessFact& access)
        {
            if (access.sharedObject)
                return "unsynchronized concurrent access to an object shared with a thread";

            return "unsynchronized concurrent access to global '" + describeSymbol(access) + "'";
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

        /// One diagnostic per memory location *and* concurrency context, rather than one per pair
        /// of conflicting instructions. Two threads racing over twenty statements is a single
        /// finding; the same location raced by a different pair of tasks is a separate one.
        std::string conflictLocationKey(const AccessFact& lhs, const AccessFact& rhs,
                                        const EntrySet& lhsEntries, const EntrySet& rhsEntries)
        {
            std::ostringstream stream;
            stream << lhs.symbol;
            if (!lhs.region.hasKnownOffset || !rhs.region.hasKnownOffset)
                stream << "[*]";
            else
                stream << "+" << std::min(lhs.region.byteOffset, rhs.region.byteOffset);

            std::vector<std::string> contexts{
                joinValues(sortedThreadEntries(lhsEntries)),
                joinValues(sortedThreadEntries(rhsEntries)),
            };
            std::sort(contexts.begin(), contexts.end());
            for (const std::string& context : contexts)
                stream << "|" << context;

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
            if (access.isAtomic)
                stream << "atomic ";
            stream << toString(access.kind) << " at " << formatLocation(access.userLocation);

            if (!entries.empty())
                stream << " (thread entries: " << joinValues(entries) << ")";
            else if (access.inRootTask)
                stream << " (initial thread)";

            if (!access.heldLocks.empty())
                stream << " under recognized lock(s): " << joinValues(sortedLocks(access));

            return stream.str();
        }

        std::vector<std::string> describeContext(const AccessFact& access, const EntrySet& entries)
        {
            std::vector<std::string> ordered = sortedThreadEntries(entries);
            if (ordered.empty() && access.inRootTask)
                ordered.push_back(rootTaskId());
            return ordered;
        }

        void emitPairDiagnostic(
            DiagnosticReport& report, const AccessFact& lhs, const AccessFact& rhs,
            const EntrySet& lhsEntries, const EntrySet& rhsEntries, const TUFacts& facts,
            std::size_t additionalConflictingPairs,
            const std::vector<std::pair<std::string, SourceLocation>>& relatedConflictSites)
        {
            const std::vector<std::string> orderedLhsEntries = describeContext(lhs, lhsEntries);
            const std::vector<std::string> orderedRhsEntries = describeContext(rhs, rhsEntries);
            const std::vector<std::string> conflictKinds =
                collectConflictKinds(lhs, rhs, lhsEntries, rhsEntries, facts);
            const std::vector<std::string> variableAliasing = collectAliasProvenances(lhs, rhs);

            DiagnosticBuilder builder(report, RuleId::DataRaceGlobal);
            builder.confidence(inferConfidence(lhs, rhs))
                .primaryLocation(lhs.userLocation)
                .relatedLocation("Conflicting access", rhs.userLocation)
                .message(describeConflictSubject(lhs))
                .note("first access: " + describeAccess(lhs, orderedLhsEntries))
                .note("conflicting access: " + describeAccess(rhs, orderedRhsEntries))
                .note("possible conflict kinds: " + joinValues(conflictKinds));

            if (isMixedAtomicPair(lhs, rhs))
            {
                builder.note("one side is atomic while the other is a plain access: atomicity on "
                             "only one side does not order the pair");
            }

            builder.note("no common recognized lock protects the conflicting accesses");

            if (additionalConflictingPairs != 0)
            {
                builder.note("additional conflicting access pairs on this location: " +
                             std::to_string(additionalConflictingPairs));
            }

            builder.property("symbol", describeSymbol(lhs))
                .property("firstAccessKind", std::string(toString(lhs.kind)))
                .property("secondAccessKind", std::string(toString(rhs.kind)))
                .property("firstAliasProvenance", std::string(toString(lhs.aliasProvenance)))
                .property("secondAliasProvenance", std::string(toString(rhs.aliasProvenance)))
                .property("firstProtected", !lhs.heldLocks.empty())
                .property("secondProtected", !rhs.heldLocks.empty())
                .property("firstThreadEntries", orderedLhsEntries)
                .property("secondThreadEntries", orderedRhsEntries)
                .property("conflictKinds", conflictKinds)
                .property("variableAliasing", variableAliasing)
                .property("firstAtomic", lhs.isAtomic)
                .property("secondAtomic", rhs.isAtomic)
                .property("additionalConflictingPairs",
                          static_cast<std::int64_t>(additionalConflictingPairs));

            if (hasDistinctLoweredLocation(lhs))
                builder.relatedLocation("Lowered first access", lhs.loweredLocation);
            if (hasDistinctLoweredLocation(rhs))
                builder.relatedLocation("Lowered conflicting access", rhs.loweredLocation);

            for (const auto& [label, location] : relatedConflictSites)
                builder.relatedLocation(label, location);

            builder.emit();
        }

        void emitSelfConcurrentDiagnostic(DiagnosticReport& report, const AccessFact& access,
                                          const EntrySet& entries)
        {
            const std::vector<std::string> orderedEntries = describeContext(access, entries);
            const std::string entryLabel =
                orderedEntries.empty() ? access.functionId : joinValues(orderedEntries);
            const std::vector<std::string> conflictKinds = {"write/write"};
            const std::vector<std::string> variableAliasing = collectAliasProvenances(access);

            DiagnosticBuilder builder(report, RuleId::DataRaceGlobal);
            builder.confidence(inferConfidence(access))
                .primaryLocation(access.userLocation)
                .relatedLocation("Concurrent invocation", access.userLocation)
                .message(describeConflictSubject(access))
                .note("access: " + describeAccess(access, orderedEntries))
                .note("conflicts with another concurrent invocation reachable from thread entry "
                      "'" +
                      entryLabel + "'")
                .note("possible conflict kinds: " + joinValues(conflictKinds))
                .note("no common recognized lock protects the conflicting accesses")
                .property("symbol", describeSymbol(access))
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
        static const EntrySet emptyEntries;

        auto entriesOf = [&](const AccessFact& access) -> const EntrySet&
        {
            const auto it = reachableEntriesByFunction.find(access.functionId);
            return it != reachableEntriesByFunction.end() ? it->second : emptyEntries;
        };

        std::map<std::string, std::vector<const AccessFact*>> accessesBySymbol;
        for (const AccessFact& access : facts.accesses)
        {

            // Code on the initial thread is analyzed too: it is a task like any other, bounded by
            // the spawns and joins surrounding it.
            if (!reachableEntriesByFunction.contains(access.functionId) && !access.inRootTask)
                continue;

            accessesBySymbol[access.symbol].push_back(&access);
        }

        /// One reported race per memory location, with the remaining conflicting pairs summarized.
        struct LocationConflict
        {
            const AccessFact* lhs = nullptr;
            const AccessFact* rhs = nullptr;
            ConfidenceLevel confidence = ConfidenceLevel::Low;
            bool isWriteWrite = false;
            bool isPrecise = false;
            bool showsLoweredOrigin = false;
            std::size_t additionalPairs = 0;
            std::vector<std::pair<std::string, SourceLocation>> relatedSites;
            std::set<std::string> seenSiteKeys;
        };

        DiagnosticReport report;
        for (const auto& [symbol, accesses] : accessesBySymbol)
        {
            (void)symbol;
            std::map<std::string, LocationConflict> conflictsByLocation;
            std::set<std::string> preciseConflictSymbols;

            for (std::size_t lhsIndex = 0; lhsIndex < accesses.size(); ++lhsIndex)
            {
                for (std::size_t rhsIndex = lhsIndex + 1; rhsIndex < accesses.size(); ++rhsIndex)
                {
                    const AccessFact& lhs = *accesses[lhsIndex];
                    const AccessFact& rhs = *accesses[rhsIndex];

                    if (lhs.kind != AccessKind::Write && rhs.kind != AccessKind::Write)
                        continue;

                    if (!lhs.region.mayOverlap(rhs.region))
                        continue;

                    if (isRaceFreeAtomicPair(lhs, rhs))
                        continue;

                    if (isMixedAtomicPair(lhs, rhs) && !hasPreciseRegions(lhs, rhs))
                        continue;

                    const EntrySet& lhsEntries = entriesOf(lhs);
                    const EntrySet& rhsEntries = entriesOf(rhs);
                    if (!mayHappenInParallel(lhs, lhsEntries, rhs, rhsEntries, facts))
                        continue;

                    if (shareRecognizedLock(lhs, rhs))
                        continue;

                    // Approximations: an effect inferred for a call names the call site rather
                    // than the instruction, and an alias-guessed identity names a global the
                    // analysis merely suspects. Either is worth reporting when it is the only
                    // evidence about a symbol — that is what the alias fallback exists for — and
                    // is noise once an observed conflict on that same symbol is available.
                    const bool isPrecise = !lhs.coarseCallEffect && !rhs.coarseCallEffect &&
                                           !lhs.guessedIdentity && !rhs.guessedIdentity;
                    if (isPrecise)
                        preciseConflictSymbols.insert(lhs.symbol);
                    else if (preciseConflictSymbols.contains(lhs.symbol))
                        continue;

                    LocationConflict& conflict =
                        conflictsByLocation[conflictLocationKey(lhs, rhs, lhsEntries, rhsEntries)];
                    const ConfidenceLevel confidence = inferConfidence(lhs, rhs);
                    const bool isWriteWrite =
                        lhs.kind == AccessKind::Write && rhs.kind == AccessKind::Write;
                    // Among equally precise candidates, the one whose primary access also shows
                    // where it lowered tells the reader more: the line they wrote and the header
                    // it expanded into.
                    const bool showsLoweredOrigin = hasDistinctLoweredLocation(lhs);

                    if (conflict.lhs == nullptr ||
                        std::make_tuple(isPrecise, showsLoweredOrigin, confidence, isWriteWrite) >
                            std::make_tuple(conflict.isPrecise, conflict.showsLoweredOrigin,
                                            conflict.confidence, conflict.isWriteWrite))
                    {
                        if (conflict.lhs != nullptr)
                            ++conflict.additionalPairs;

                        conflict.lhs = &lhs;
                        conflict.rhs = &rhs;
                        conflict.confidence = confidence;
                        conflict.isWriteWrite = isWriteWrite;
                        conflict.isPrecise = isPrecise;
                        conflict.showsLoweredOrigin = showsLoweredOrigin;
                    }
                    else
                    {
                        ++conflict.additionalPairs;
                    }

                    for (const AccessFact* site : {&lhs, &rhs})
                    {
                        if (conflict.relatedSites.size() >= kMaxRelatedConflictSites)
                            break;

                        const std::string siteKey = formatLocation(site->userLocation);
                        if (!conflict.seenSiteKeys.insert(siteKey).second)
                            continue;

                        conflict.relatedSites.emplace_back("Conflicting site", site->userLocation);
                    }
                }
            }

            for (auto& [locationKey, conflict] : conflictsByLocation)
            {
                (void)locationKey;
                // The representative pair already carries both of its own locations.
                std::erase_if(
                    conflict.relatedSites,
                    [&](const std::pair<std::string, SourceLocation>& site)
                    {
                        return sameSourceLocation(site.second, conflict.lhs->userLocation) ||
                               sameSourceLocation(site.second, conflict.rhs->userLocation);
                    });

                emitPairDiagnostic(report, *conflict.lhs, *conflict.rhs, entriesOf(*conflict.lhs),
                                   entriesOf(*conflict.rhs), facts, conflict.additionalPairs,
                                   conflict.relatedSites);
            }

            if (!conflictsByLocation.empty())
                continue;

            for (const AccessFact* access : accesses)
            {
                if (access->kind != AccessKind::Write || access->isAtomic)
                    continue;

                const EntrySet& entries = entriesOf(*access);
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
