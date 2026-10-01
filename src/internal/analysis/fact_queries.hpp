// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    [[nodiscard]] inline bool isSelfConcurrent(const ThreadEntrySet& entries, const TUFacts& facts)
    {
        return std::any_of(entries.begin(), entries.end(),
                           [&](const std::string& entry)
                           {
                               const auto it = facts.entryConcurrency.find(entry);
                               return it != facts.entryConcurrency.end() &&
                                      it->second.isSelfConcurrent();
                           });
    }

    [[nodiscard]] inline bool mayRunConcurrently(const ThreadEntrySet& lhsEntries,
                                                 const ThreadEntrySet& rhsEntries,
                                                 const TUFacts& facts)
    {
        for (const std::string& lhsEntry : lhsEntries)
        {
            for (const std::string& rhsEntry : rhsEntries)
            {
                if (lhsEntry != rhsEntry)
                {
                    // A join between the two spawns orders the entries, so their code can never
                    // interleave.
                    if (facts.sequencedEntryPairs.contains(makeEntryPair(lhsEntry, rhsEntry)))
                        continue;

                    return true;
                }

                const auto it = facts.entryConcurrency.find(lhsEntry);
                if (it != facts.entryConcurrency.end() && it->second.isSelfConcurrent())
                    return true;
            }
        }

        return false;
    }

    /// The initial thread executes `rootSide` while one of `otherEntries` is running.
    template <typename FactType>
    [[nodiscard]] bool racesWithRootTask(const FactType& rootSide,
                                         const ThreadEntrySet& otherEntries)
    {
        if (!rootSide.inRootTask)
            return false;

        for (const std::string& entry : otherEntries)
        {
            if (rootSide.liveEntries.contains(entry))
                return true;
        }
        return false;
    }

    /// May-happen-in-parallel relation between two program points, described by the entries that
    /// can execute them and, for code on the initial thread, the entries alive at that point.
    /// Two points reached only by the initial thread never race: it executes them in sequence.
    template <typename FactType>
    [[nodiscard]] bool mayHappenInParallel(const FactType& lhs, const ThreadEntrySet& lhsEntries,
                                           const FactType& rhs, const ThreadEntrySet& rhsEntries,
                                           const TUFacts& facts)
    {
        // A helper called both from `main` and from a worker races with itself across the two
        // contexts, even though a single thread entry reaches it.
        if (racesWithRootTask(lhs, rhsEntries) || racesWithRootTask(rhs, lhsEntries))
            return true;

        if (lhsEntries.empty() || rhsEntries.empty())
            return false;

        return mayRunConcurrently(lhsEntries, rhsEntries, facts);
    }

    [[nodiscard]] inline std::vector<std::string> sortedThreadEntries(const ThreadEntrySet& entries)
    {
        std::vector<std::string> ordered(entries.begin(), entries.end());
        std::sort(ordered.begin(), ordered.end());
        return ordered;
    }
} // namespace ctrace::concurrency::internal::analysis
