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

        for (const ThreadArgumentEscapeFact& escape : facts.threadArgumentEscapes)
        {
            DiagnosticBuilder builder(report, RuleId::ThreadArgumentEscapesFrame);
            builder.primaryLocation(escape.location)
                .message("thread is given a pointer into the frame that created it")
                .note("the argument is a local variable of this function, and no join stands "
                      "between the creation and every way out: the frame is gone while the "
                      "thread still reads it")
                .note("either join before returning, or give the thread storage that outlives "
                      "the call")
                .taxonomy("CERT", "DCL30-C", "Declare objects with appropriate storage durations")
                .property("function", escape.functionId);

            if (!escape.entryFunctionId.empty())
                builder.property("entry", escape.entryFunctionId);

            builder.emit();
        }

        for (const SignalHandlerFact& handler : facts.signalHandlers)
        {
            DiagnosticBuilder(report, RuleId::UnsafeSignalHandler)
                .primaryLocation(handler.location)
                .message("signal handler reaches a call it may not make")
                .note("a handler interrupts its own thread at an arbitrary instruction, so it "
                      "may run while the interrupted code is halfway through a structure: "
                      "allocating, printing or locking there re-enters it")
                .note("set a flag of type volatile sig_atomic_t and act on it outside the "
                      "handler")
                .relatedLocation("Unsafe call reached from the handler", handler.unsafeCallLocation)
                .taxonomy("CERT", "SIG30-C",
                          "Call only asynchronous-safe functions within signal handlers")
                .property("handler", handler.handlerFunctionId)
                .emit();
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
