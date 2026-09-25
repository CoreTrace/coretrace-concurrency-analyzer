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
    /// What a project analysis carries from one unit to the others. Every unit builds a
    /// channel and the program index hands it back, so it is only worth building when a
    /// selected rule reads what it changes.
    struct CrossUnitChannels
    {
        /// A global another unit defines, which turns an `extern` here into shared state.
        bool externGlobals = false;
        /// What a helper another unit defines does to the lock it is handed.
        bool helperLockEffects = false;
        /// How many instances of an entry the program runs, including spawns in other units.
        bool entryConcurrency = false;
        /// Whether some unit starts a thread, which makes a fork elsewhere unsafe.
        bool programThreads = false;
        /// Whether some unit collects terminated children, which reaps a fork elsewhere.
        bool programReaps = false;
        /// A thread handle another unit joins or detaches.
        bool resolvedHandles = false;
    };

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
        /// `threadArgumentFrees`.
        bool threadArgumentFrees = false;
        /// `expiredThreadLocals`.
        bool expiredThreadLocals = false;
        /// `signalHandlers`.
        bool signalHandlers = false;
        /// `weakPublications`. Built from the accesses, which it therefore requires.
        bool weakPublications = false;
        /// `entryConcurrency` as corrected by the task analysis, `sequencedEntryPairs` and
        /// `reachableThreadEntriesByFunction`.
        bool threadEntries = false;
        /// `program`: what the whole-program passes read back from a unit. A project analysis
        /// always builds it: the program index and the ABI grouping are built from it.
        bool program = false;
        /// The cross-unit channels the selected rules read, in a project analysis.
        CrossUnitChannels channels;

        /// The direct call sites of the unit, which every propagation across calls follows.
        [[nodiscard]] bool directCallSites() const noexcept
        {
            return accesses || lockOrders || threadLifecycles || conditionWaits ||
                   processLifecycle || signalHandlers || threadEntries;
        }
        /// The spawn sites: the raw entry concurrency, and whether the unit starts a thread.
        [[nodiscard]] bool spawns() const noexcept
        {
            return threadEntries || processLifecycle || channels.programThreads;
        }
        /// Lock wrapper summaries: what a helper does to the locks it is handed, which the lock
        /// state reads here and the program publishes to units calling the helper.
        [[nodiscard]] bool lockWrapperSummaries() const noexcept
        {
            return accesses || lockOrders || channels.helperLockEffects;
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

        /// The facts the selected rules read, and in a project analysis the cross-unit channels
        /// they read too. This is the one place mapping a rule to what crosses a unit boundary
        /// for it: the builder builds a channel's facts, and the second-pass gate waits for a
        /// channel, only when it is selected here. A rule that reads no channel owes the program
        /// nothing, so its project conclusions are its single-unit ones.
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
                    selection.channels.externGlobals = true;
                    selection.channels.helperLockEffects = true;
                    selection.channels.entryConcurrency = true;
                    break;
                case RuleId::DeadlockLockOrder:
                    selection.lockOrders = true;
                    selection.recursiveLockIds = true;
                    selection.threadEntries = true;
                    selection.channels.helperLockEffects = true;
                    selection.channels.entryConcurrency = true;
                    break;
                case RuleId::MissingJoin:
                    selection.threadLifecycles = true;
                    selection.channels.resolvedHandles = true;
                    break;
                case RuleId::ConditionWaitWithoutPredicate:
                    selection.conditionWaits = true;
                    break;
                case RuleId::ForkAfterThreadCreation:
                    selection.processLifecycle = true;
                    selection.channels.programThreads = true;
                    break;
                case RuleId::UnreapedChildProcess:
                    selection.processLifecycle = true;
                    selection.channels.programReaps = true;
                    break;
                case RuleId::ThreadArgumentEscapesFrame:
                    selection.threadArgumentEscapes = true;
                    break;
                case RuleId::ThreadArgumentFreedEarly:
                    selection.threadArgumentFrees = true;
                    break;
                case RuleId::ThreadLocalOutlivesThread:
                    selection.expiredThreadLocals = true;
                    break;
                case RuleId::UnsafeSignalHandler:
                    selection.signalHandlers = true;
                    break;
                case RuleId::WeakPublicationOrdering:
                    selection.accesses = true;
                    selection.threadEntries = true;
                    selection.weakPublications = true;
                    selection.channels.externGlobals = true;
                    selection.channels.entryConcurrency = true;
                    break;
                case RuleId::CompilerDiagnostic:
                    break;
                }
            }

            if (projectAnalysis)
            {
                selection.program = true;
                // Every unit builds the facts behind each selected channel, so the program can
                // answer it for the others; a unit-local selection builds none of them.
                selection.threadEntries =
                    selection.threadEntries || selection.channels.entryConcurrency;
                selection.threadLifecycles =
                    selection.threadLifecycles || selection.channels.resolvedHandles;
            }
            else
            {
                selection.channels = {};
            }

            return selection;
        }

        [[nodiscard]] static FactSelection everything()
        {
            return forRules(AnalysisOptions::allAvailable().enabledRules, true);
        }
    };
} // namespace ctrace::concurrency::internal::analysis
