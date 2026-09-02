// SPDX-License-Identifier: Apache-2.0
#include "condition_wait_checker.hpp"

#include "internal/diagnostics/diagnostic_builder.hpp"
#include "report_builder.hpp"

namespace ctrace::concurrency::internal::analysis
{
    DiagnosticReport ConditionWaitChecker::run(const TUFacts& facts) const
    {
        using internal::diagnostics::DiagnosticBuilder;

        DiagnosticReport report;

        for (const ConditionWaitFact& fact : facts.conditionWaits)
        {
            DiagnosticBuilder builder(report, RuleId::ConditionWaitWithoutPredicate);
            builder.primaryLocation(fact.location)
                .message("condition-variable wait rechecks nothing when it wakes")
                .note("a wait may return without the condition holding: the standard permits a "
                      "spurious wake-up, and a broadcast wakes every waiter while only one may "
                      "proceed")
                .note("wrap the wait in a loop that rechecks the condition, or pass a predicate "
                      "to the wait itself")
                .taxonomy("CERT", "CON36-C",
                          "Wrap functions that can spuriously wake up in a loop");

            if (fact.viaHelper)
            {
                builder
                    .note("the wait is performed by a helper called from here; a helper cannot "
                          "recheck a condition it does not know, so the loop belongs at this "
                          "call site")
                    .relatedLocation("Wait performed here", fact.loweredLocation);
            }

            builder.property("viaHelper", fact.viaHelper)
                .property("function", fact.functionId)
                .emit();
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
