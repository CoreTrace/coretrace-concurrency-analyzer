// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ctrace::concurrency::internal::analysis::cross_tu
{
    struct ProjectAnalysis
    {
        DiagnosticReport report;
        /// Modules left out because their target ABI differs from the majority. Byte offsets and
        /// lock identities are computed against a DataLayout; comparing them across ABIs would
        /// relate addresses that have no common meaning.
        std::vector<std::string> skippedIncompatibleModules;
        /// Units the program-wide view forced through a second analysis. Reported because it is
        /// the cost of the project mode over a run of independent units.
        std::size_t reanalyzedUnitCount = 0;
        /// Units that could not be loaded, in unit order. See ProjectAnalysisReport.
        std::vector<FailedUnit> failedUnits;
        std::size_t peakLiveUnits = 0;
        std::int64_t loadMilliseconds = 0;
    };

    /// Runs the per-unit analysis over a whole project, letting each unit see the facts the
    /// others established.
    ///
    /// The pass structure follows from what actually crosses the boundary. A unit's facts only
    /// change once the program-wide view is known, so a first pass analyses every unit alone, an
    /// index is folded from the results, and a second pass re-runs only the units the index has
    /// something to say about. On a project where most units never touch a thread entry owned by
    /// another, that second pass is nearly empty.
    class InterTUCoordinator
    {
      public:
        explicit InterTUCoordinator(AnalysisOptions options = {});

        [[nodiscard]] ProjectAnalysis analyze(const ProjectUnitSource& units) const;

      private:
        AnalysisOptions options_;
    };
} // namespace ctrace::concurrency::internal::analysis::cross_tu
