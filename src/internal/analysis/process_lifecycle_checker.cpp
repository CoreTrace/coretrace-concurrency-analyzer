// SPDX-License-Identifier: Apache-2.0
#include "process_lifecycle_checker.hpp"

#include "internal/diagnostics/diagnostic_builder.hpp"
#include "report_builder.hpp"

namespace ctrace::concurrency::internal::analysis
{
    DiagnosticReport ProcessLifecycleChecker::run(const TUFacts& facts) const
    {
        using internal::diagnostics::DiagnosticBuilder;

        DiagnosticReport report;
        const bool createsThreads = !facts.spawns.empty();

        for (const ProcessForkFact& fork : facts.processForks)
        {
            // An `exec` answers both questions at once: the child keeps nothing it inherited,
            // and a program that spawns real children is expected to manage them deliberately.
            if (fork.execReachable)
                continue;

            if (createsThreads)
            {
                DiagnosticBuilder(report, RuleId::ForkAfterThreadCreation)
                    .primaryLocation(fork.location)
                    .message("fork in a program that creates threads, with no exec in the child")
                    .note("only the calling thread survives a fork, but the whole address space "
                          "is inherited: a mutex another thread held is copied locked, and "
                          "nothing will ever unlock it")
                    .note("POSIX limits the child to async-signal-safe calls until it execs, "
                          "which excludes locking, allocating and starting threads")
                    .note("either exec in the child, or fork before any thread is created")
                    .taxonomy("CERT", "POS33-C", "Do not use vfork()")
                    .property("function", fork.functionId)
                    .emit();
            }

            if (!facts.reapsChildProcesses)
            {
                DiagnosticBuilder(report, RuleId::UnreapedChildProcess)
                    .primaryLocation(fork.location)
                    .message("child process is never collected")
                    .note("the program forks but never calls wait, waitpid or waitid: every "
                          "terminated child stays in the process table as a zombie")
                    .note("keep the pid the fork returns and wait on it, or arrange for the "
                          "system to reap children")
                    .property("function", fork.functionId)
                    .emit();
            }
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
