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

        static const RuleMetadata kMissingJoin{
            .ruleId = RuleId::MissingJoin,
            .title = "Thread handle is not joined before scope exit",
            .shortDescription =
                "Detects thread creation sites whose handle is not joined before the enclosing "
                "scope completes.",
            .defaultSeverity = Severity::Warning,
            .primaryTaxonomy = std::nullopt,
        };

        static const RuleMetadata kDeadlockLockOrder{
            .ruleId = RuleId::DeadlockLockOrder,
            .title = "Potential deadlock caused by inconsistent lock order",
            .shortDescription =
                "Detects conflicting lock acquisition orders that can lead to circular wait.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "833",
                    .title = "Deadlock",
                },
        };

        switch (ruleId)
        {
        case RuleId::CompilerDiagnostic:
            return kCompilerDiagnostic;
        case RuleId::DataRaceGlobal:
            return kDataRaceGlobal;
        case RuleId::MissingJoin:
            return kMissingJoin;
        case RuleId::DeadlockLockOrder:
            return kDeadlockLockOrder;
        }

        return kCompilerDiagnostic;
    }
} // namespace ctrace::concurrency::internal::diagnostics
