// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    /// Which facts a unit analysis must produce, decided once from the selected rules. This is
    /// the only place that knows which rule reads which fact; the builder asks it what to
    /// build, and the collectors never see a rule name.
    ///
    /// Each flag names an output of TUFactsBuilder::build. The methods below spell out the
    /// prerequisites those outputs share, so a gate in the builder reads as the fact it
    /// serves rather than as a list of rules.
    struct FactSelection
    {
        /// `accesses`, with their held locks, live entries and shared-object identities.
        bool accesses = false;
        /// `lockOrders`, with the locks held on entry and the live entries at each site.
        bool lockOrders = false;
        /// `recursiveLockIds`.
        bool recursiveLockIds = false;
        /// `threadLifecycles` and `resolvedGlobalHandleIds`.
        bool threadLifecycles = false;
        /// `conditionWaits`.
        bool conditionWaits = false;
        /// `processForks`, `reapsChildProcesses` and `programCreatesThreads`.
        bool processLifecycle = false;
        /// `threadArgumentEscapes`.
        bool threadArgumentEscapes = false;
        /// `signalHandlers`.
        bool signalHandlers = false;
        /// `entryConcurrency` as corrected by the task analysis, `sequencedEntryPairs` and
        /// `reachableThreadEntriesByFunction`.
        bool threadEntries = false;
        /// `program`: what the whole-program passes read back from a unit. Only a project
        /// analysis needs it, and then whatever the rules say, because the symbol index and
        /// the second-pass gate are built from it.
        bool program = false;

        /// The direct call sites of the unit, which every propagation across calls follows.
        [[nodiscard]] bool directCallSites() const noexcept
        {
            return accesses || lockOrders || threadLifecycles || conditionWaits ||
                   processLifecycle || signalHandlers || threadEntries;
        }
        /// The spawn sites: the raw entry concurrency, and whether the unit starts a thread.
        [[nodiscard]] bool spawns() const noexcept
        {
            return threadEntries || processLifecycle;
        }
        /// Lock wrapper summaries: what a helper does to the locks it is handed. Published to
        /// the program as well, so a project analysis always has them.
        [[nodiscard]] bool lockWrapperSummaries() const noexcept
        {
            return accesses || lockOrders || program;
        }
        /// Shared-object bindings and the lock state propagated across calls.
        [[nodiscard]] bool lockState() const noexcept
        {
            return accesses || lockOrders;
        }
        /// The task analysis: which entries are live at each instruction, and which spawn
        /// sites can overlap. Both the accesses and the lock orders carry its answers, and the
        /// entry concurrency is corrected by it.
        [[nodiscard]] bool taskConcurrency() const noexcept
        {
            return threadEntries || accesses || lockOrders;
        }

        /// The facts the selected rules read. In a project analysis the symbol index and the
        /// second-pass gate read the entry concurrency, the thread lifecycles, the lock wrapper
        /// summaries and the fork facts of every unit whatever the rules, so those are always
        /// built there.
        [[nodiscard]] static FactSelection forRules(const std::vector<RuleId>& rules,
                                                    bool projectAnalysis)
        {
            FactSelection selection;
            for (const RuleId rule : rules)
            {
                switch (rule)
                {
                case RuleId::DataRaceGlobal:
                    selection.accesses = true;
                    selection.threadEntries = true;
                    break;
                case RuleId::DeadlockLockOrder:
                    selection.lockOrders = true;
                    selection.recursiveLockIds = true;
                    selection.threadEntries = true;
                    break;
                case RuleId::MissingJoin:
                    selection.threadLifecycles = true;
                    break;
                case RuleId::ConditionWaitWithoutPredicate:
                    selection.conditionWaits = true;
                    break;
                case RuleId::ForkAfterThreadCreation:
                case RuleId::UnreapedChildProcess:
                    selection.processLifecycle = true;
                    break;
                case RuleId::ThreadArgumentEscapesFrame:
                    selection.threadArgumentEscapes = true;
                    break;
                case RuleId::UnsafeSignalHandler:
                    selection.signalHandlers = true;
                    break;
                case RuleId::CompilerDiagnostic:
                    break;
                }
            }

            if (projectAnalysis)
            {
                selection.program = true;
                selection.threadEntries = true;
                selection.threadLifecycles = true;
                selection.processLifecycle = true;
            }

            return selection;
        }

        [[nodiscard]] static FactSelection everything()
        {
            return forRules(AnalysisOptions::allAvailable().enabledRules, true);
        }
    };
} // namespace ctrace::concurrency::internal::analysis
