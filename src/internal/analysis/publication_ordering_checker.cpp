// SPDX-License-Identifier: Apache-2.0
#include "publication_ordering_checker.hpp"

#include "fact_queries.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <string>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        std::string joinSymbols(const std::vector<std::string>& symbols)
        {
            std::string joined;
            for (const std::string& symbol : symbols)
            {
                if (!joined.empty())
                    joined += "', '";
                joined += symbol;
            }
            return "'" + joined + "'";
        }
    } // namespace

    DiagnosticReport PublicationOrderingChecker::run(const TUFacts& facts) const
    {
        using internal::diagnostics::DiagnosticBuilder;

        auto entriesOf = [&](const std::string& functionId) -> const ThreadEntrySet*
        {
            const auto it = facts.reachableThreadEntriesByFunction.find(functionId);
            return it != facts.reachableThreadEntriesByFunction.end() ? &it->second : nullptr;
        };

        DiagnosticReport report;
        for (const WeakPublicationFact& publication : facts.weakPublications)
        {
            // Only threads that can overlap need the flag to order anything: a reader the
            // publisher's join already precedes is ordered by the join.
            const ThreadEntrySet* publisherEntries = entriesOf(publication.publisherFunctionId);
            const ThreadEntrySet* readerEntries = entriesOf(publication.readerFunctionId);
            if (publisherEntries == nullptr || readerEntries == nullptr ||
                !mayRunConcurrently(*publisherEntries, *readerEntries, facts))
            {
                continue;
            }

            DiagnosticBuilder builder(report, RuleId::WeakPublicationOrdering);
            builder.message("atomic flag '" + publication.flag + "' publishes " +
                            joinSymbols(publication.data) + " without release/acquire ordering");

            if (!publication.storeReleases)
            {
                builder.primaryLocation(publication.storeLocation)
                    .note("the store to the flag does not release: nothing orders the writes "
                          "before it ahead of a reader that sees the flag set")
                    .relatedLocation("Load that observes the flag", publication.loadLocation);
            }
            else
            {
                builder.primaryLocation(publication.loadLocation)
                    .note("the load that observes the flag does not acquire: nothing orders the "
                          "reads after it behind the writes that preceded the store")
                    .relatedLocation("Store that publishes", publication.storeLocation);
            }
            if (!publication.storeReleases && !publication.loadAcquires)
                builder.note("the load that observes the flag does not acquire either");

            builder
                .note("store the flag with memory_order_release and load it with "
                      "memory_order_acquire, or use the default sequentially consistent order")
                .property("flag", publication.flag)
                .property("data", publication.data)
                .property("storeReleases", publication.storeReleases)
                .property("loadAcquires", publication.loadAcquires)
                .emit();
        }
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
