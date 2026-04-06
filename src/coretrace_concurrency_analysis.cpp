// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analysis.hpp"

#include "internal/analysis/data_race_checker.hpp"
#include "internal/analysis/tu_facts_builder.hpp"

namespace ctrace::concurrency
{
    DiagnosticReport SingleTUConcurrencyAnalyzer::analyze(const llvm::Module& module) const
    {
        internal::analysis::TUFactsBuilder factsBuilder;
        const internal::analysis::TUFacts facts = factsBuilder.build(module);

        internal::analysis::DataRaceChecker checker;
        return checker.run(module, facts);
    }
} // namespace ctrace::concurrency
