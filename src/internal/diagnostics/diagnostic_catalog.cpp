// SPDX-License-Identifier: Apache-2.0
#include "diagnostic_catalog.hpp"

namespace ctrace::concurrency::internal::diagnostics
{
    const RuleMetadata& lookupRuleMetadata(RuleId ruleId)
    {
        static const RuleMetadata kCompilerDiagnostic{
            .ruleId = RuleId::CompilerDiagnostic,
            .title = "Compiler diagnostic",
            .shortDescription =
                "Represents a compiler-originated diagnostic captured during IR generation.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy = std::nullopt,
        };

        static const RuleMetadata kDataRaceGlobal{
            .ruleId = RuleId::DataRaceGlobal,
            .title = "Unsynchronized concurrent access to a shared global",
            .shortDescription =
                "Detects shared global accesses that can run concurrently without a common "
                "recognized lock.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "362",
                    .title = "Concurrent Execution using Shared Resource with Improper "
                             "Synchronization ('Race Condition')",
                },
        };

        switch (ruleId)
        {
        case RuleId::CompilerDiagnostic:
            return kCompilerDiagnostic;
        case RuleId::DataRaceGlobal:
            return kDataRaceGlobal;
        }

        return kCompilerDiagnostic;
    }
} // namespace ctrace::concurrency::internal::diagnostics
