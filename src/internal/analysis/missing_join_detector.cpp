// SPDX-License-Identifier: Apache-2.0
#include "missing_join_detector.hpp"

#include "report_builder.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <algorithm>
#include <map>
#include <sstream>
#include <vector>

namespace ctrace::concurrency::internal::analysis
{
    namespace
    {
        using internal::diagnostics::DiagnosticBuilder;

        struct LifecycleSummary
        {
            ThreadHandleKind handleKind = ThreadHandleKind::PThread;
            std::string functionId;
            SourceLocation firstCreateLocation;
            std::size_t createCount = 0;
            std::size_t joinCount = 0;
            std::size_t detachCount = 0;
        };

        std::string handleKindLabel(ThreadHandleKind kind)
        {
            switch (kind)
            {
            case ThreadHandleKind::PThread:
                return "pthread";
            case ThreadHandleKind::StdThread:
                return "std::thread";
            }

            return "thread";
        }

        std::string describeSummary(const LifecycleSummary& summary)
        {
            std::ostringstream stream;
            stream << "creates=" << summary.createCount << ", joins=" << summary.joinCount
                   << ", detaches=" << summary.detachCount;
            return stream.str();
        }
    } // namespace

    DiagnosticReport MissingJoinDetector::run(const TUFacts& facts) const
    {
        std::map<std::string, LifecycleSummary> summariesByGroup;
        for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
        {
            LifecycleSummary& summary = summariesByGroup[fact.handleGroupId];
            summary.handleKind = fact.handleKind;
            summary.functionId = fact.functionId;
            if (summary.firstCreateLocation.file.empty() && summary.firstCreateLocation.line == 0 &&
                fact.action == ThreadLifecycleAction::Create)
            {
                summary.firstCreateLocation = fact.location;
            }

            switch (fact.action)
            {
            case ThreadLifecycleAction::Create:
                ++summary.createCount;
                if (summary.firstCreateLocation.file.empty() &&
                    summary.firstCreateLocation.line == 0)
                    summary.firstCreateLocation = fact.location;
                break;
            case ThreadLifecycleAction::Join:
                ++summary.joinCount;
                break;
            case ThreadLifecycleAction::Detach:
                ++summary.detachCount;
                break;
            }
        }

        DiagnosticReport report;
        for (const auto& [groupId, summary] : summariesByGroup)
        {
            if (groupId.starts_with("arg:"))
                continue;

            const std::size_t resolvedCount = summary.joinCount + summary.detachCount;
            if (summary.createCount <= resolvedCount)
                continue;

            const std::size_t outstandingCount = summary.createCount - resolvedCount;
            DiagnosticBuilder(report, RuleId::MissingJoin)
                .primaryLocation(summary.firstCreateLocation)
                .message("thread handle is not joined or detached before scope exit")
                .note("handle kind: " + handleKindLabel(summary.handleKind))
                .note("lifecycle summary: " + describeSummary(summary))
                .note("outstanding joinable handles: " + std::to_string(outstandingCount))
                .property("handleKind", handleKindLabel(summary.handleKind))
                .property("createCount", static_cast<std::int64_t>(summary.createCount))
                .property("joinCount", static_cast<std::int64_t>(summary.joinCount))
                .property("detachCount", static_cast<std::int64_t>(summary.detachCount))
                .property("outstandingCount", static_cast<std::int64_t>(outstandingCount))
                .emit();
        }

        finalizeReport(report, facts);
        return report;
    }
} // namespace ctrace::concurrency::internal::analysis
