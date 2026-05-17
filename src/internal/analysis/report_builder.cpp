// SPDX-License-Identifier: Apache-2.0
#include "report_builder.hpp"

#include "fact_queries.hpp"

#include <algorithm>
#include <map>
#include <tuple>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        FunctionSummary& ensureDiagnosticSummary(std::map<std::string, FunctionSummary>& functions,
                                                 const SourceLocation& location)
        {
            const std::string key = !location.function.empty() ? location.function : location.file;
            FunctionSummary& summary = functions[key];
            if (summary.name.empty())
                summary.name = location.function.empty() ? key : location.function;
            if (summary.file.empty())
                summary.file = location.file;
            return summary;
        }

        void markDiagnosticFunctions(std::map<std::string, FunctionSummary>& functions,
                                     const Diagnostic& diagnostic)
        {
            if (!diagnostic.location.function.empty())
            {
                ensureDiagnosticSummary(functions, diagnostic.location).hasDiagnostics = true;
            }

            for (const RelatedLocation& related : diagnostic.relatedLocations)
            {
                if (related.location.function.empty())
                    continue;
                ensureDiagnosticSummary(functions, related.location).hasDiagnostics = true;
            }
        }
    } // namespace

    DiagnosticSummary computeSummary(const std::vector<Diagnostic>& diagnostics)
    {
        DiagnosticSummary summary;
        for (const Diagnostic& diagnostic : diagnostics)
        {
            switch (diagnostic.severity)
            {
            case Severity::Info:
                ++summary.info;
                break;
            case Severity::Warning:
                ++summary.warning;
                break;
            case Severity::Error:
                ++summary.error;
                break;
            }
        }
        return summary;
    }

    std::vector<FunctionSummary> buildFunctionSummaries(const TUFacts& facts,
                                                        const std::vector<Diagnostic>& diagnostics)
    {
        std::map<std::string, FunctionSummary> functions;

        auto ensureSummary = [&](const std::string& functionId) -> FunctionSummary&
        {
            FunctionSummary& summary = functions[functionId];
            if (summary.name.empty())
                summary.name = functionId;
            return summary;
        };

        for (const AccessFact& access : facts.accesses)
        {
            FunctionSummary& summary = ensureSummary(access.functionId);
            if (!access.userLocation.function.empty())
                summary.name = access.userLocation.function;

            if (summary.file.empty() && !access.userLocation.file.empty())
                summary.file = access.userLocation.file;

            ++summary.sharedAccessCount;
            if (!access.heldLocks.empty())
                ++summary.protectedAccessCount;
            if (access.kind == AccessKind::Write)
                ++summary.writeAccessCount;
        }

        for (const LockOrderFact& lockOrder : facts.lockOrders)
        {
            FunctionSummary& summary = ensureSummary(lockOrder.functionId);
            if (!lockOrder.location.function.empty())
                summary.name = lockOrder.location.function;
            if (summary.file.empty() && !lockOrder.location.file.empty())
                summary.file = lockOrder.location.file;
        }

        for (const auto& [functionId, entries] : facts.reachableThreadEntriesByFunction)
        {
            FunctionSummary& summary = ensureSummary(functionId);
            summary.threadReachable = true;
            summary.threadEntries = sortedThreadEntries(entries);
        }

        for (const Diagnostic& diagnostic : diagnostics)
            markDiagnosticFunctions(functions, diagnostic);

        std::vector<FunctionSummary> ordered;
        ordered.reserve(functions.size());
        for (auto& [_, summary] : functions)
            ordered.push_back(std::move(summary));

        std::sort(ordered.begin(), ordered.end(),
                  [](const FunctionSummary& lhs, const FunctionSummary& rhs)
                  { return std::tie(lhs.name, lhs.file) < std::tie(rhs.name, rhs.file); });
        return ordered;
    }

    void finalizeReport(DiagnosticReport& report, const TUFacts& facts)
    {
        std::sort(report.diagnostics.begin(), report.diagnostics.end(),
                  [](const Diagnostic& lhs, const Diagnostic& rhs)
                  {
                      return std::tie(lhs.ruleId, lhs.location.file, lhs.location.line,
                                      lhs.location.column, lhs.message) <
                             std::tie(rhs.ruleId, rhs.location.file, rhs.location.line,
                                      rhs.location.column, rhs.message);
                  });

        for (std::size_t index = 0; index < report.diagnostics.size(); ++index)
            report.diagnostics[index].id = "diag-" + std::to_string(index + 1);

        report.functions = buildFunctionSummaries(facts, report.diagnostics);
        report.diagnosticsSummary = computeSummary(report.diagnostics);
    }
} // namespace ctrace::concurrency::internal::analysis
