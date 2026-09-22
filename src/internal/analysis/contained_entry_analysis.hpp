// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "facts.hpp"
#include "thread_completion_analysis.hpp"

#include <unordered_set>
#include <vector>

namespace llvm
{
    class Module;
}
namespace ctrace::concurrency::internal::analysis
{
    class ConcurrencySymbolClassifier;
    class LlvmFunctionAnalysisProvider;
    struct DirectCallSite;

    [[nodiscard]] std::unordered_set<EntryPair, EntryPairHash>
    collectContainedEntryPairs(const llvm::Module& module, const std::vector<DirectCallSite>& calls,
                               const ThreadCompletionMap& completions,
                               const ConcurrencySymbolClassifier& classifier,
                               LlvmFunctionAnalysisProvider& analyses);
} // namespace ctrace::concurrency::internal::analysis
