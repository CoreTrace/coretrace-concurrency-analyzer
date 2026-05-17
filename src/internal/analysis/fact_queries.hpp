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
                    return true;

                const auto it = facts.entryConcurrency.find(lhsEntry);
                if (it != facts.entryConcurrency.end() && it->second.isSelfConcurrent())
                    return true;
            }
        }

        return false;
    }

    [[nodiscard]] inline std::vector<std::string> sortedThreadEntries(const ThreadEntrySet& entries)
    {
        std::vector<std::string> ordered(entries.begin(), entries.end());
        std::sort(ordered.begin(), ordered.end());
        return ordered;
    }
} // namespace ctrace::concurrency::internal::analysis
