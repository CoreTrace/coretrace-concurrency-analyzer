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

        static const RuleMetadata kConditionWaitWithoutPredicate{
            .ruleId = RuleId::ConditionWaitWithoutPredicate,
            .title = "Condition-variable wait rechecks nothing on wake-up",
            .shortDescription =
                "Detects a condition-variable wait with neither a predicate nor a loop around "
                "it, which treats any wake-up as proof that the condition holds.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "662",
                    .title = "Improper Synchronization",
                },
        };

        static const RuleMetadata kForkAfterThreadCreation{
            .ruleId = RuleId::ForkAfterThreadCreation,
            .title = "Fork in a threaded program without an exec in the child",
            .shortDescription =
                "Detects a fork performed by a program that creates threads, where the child "
                "keeps running instead of replacing its process image.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "662",
                    .title = "Improper Synchronization",
                },
        };

        static const RuleMetadata kUnreapedChildProcess{
            .ruleId = RuleId::UnreapedChildProcess,
            .title = "Child process is never collected",
            .shortDescription =
                "Detects a program that forks but never waits on its children, leaving them as "
                "zombies in the process table.",
            .defaultSeverity = Severity::Warning,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "404",
                    .title = "Improper Resource Shutdown or Release",
                },
        };

        static const RuleMetadata kThreadArgumentEscapesFrame{
            .ruleId = RuleId::ThreadArgumentEscapesFrame,
            .title = "Thread argument points into the frame that created it",
            .shortDescription =
                "Detects a thread started with a pointer to a local variable of a function that "
                "returns without waiting for it.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "562",
                    .title = "Return of Stack Variable Address",
                },
        };

        static const RuleMetadata kUnsafeSignalHandler{
            .ruleId = RuleId::UnsafeSignalHandler,
            .title = "Signal handler calls a function it may not call",
            .shortDescription =
                "Detects a signal handler that reaches an allocation, a stream or a lock, none "
                "of which is async-signal-safe.",
            .defaultSeverity = Severity::Error,
            .primaryTaxonomy =
                TaxonomyMetadata{
                    .scheme = "CWE",
                    .id = "479",
                    .title = "Signal Handler Use of a Non-reentrant Function",
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
        case RuleId::ConditionWaitWithoutPredicate:
            return kConditionWaitWithoutPredicate;
        case RuleId::ForkAfterThreadCreation:
            return kForkAfterThreadCreation;
        case RuleId::UnreapedChildProcess:
            return kUnreapedChildProcess;
        case RuleId::ThreadArgumentEscapesFrame:
            return kThreadArgumentEscapesFrame;
        case RuleId::UnsafeSignalHandler:
            return kUnsafeSignalHandler;
        }

        return kCompilerDiagnostic;
    }
} // namespace ctrace::concurrency::internal::diagnostics
