// SPDX-License-Identifier: Apache-2.0
#include "missing_join_detector.hpp"

#include "report_builder.hpp"
#include "internal/diagnostics/diagnostic_builder.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
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
            /// Set when some creation is not followed by a join or detach on every path, when a
            /// creation loops while its resolution does not, or when the handle escapes into
            /// storage the analysis cannot name. The first two report despite balanced counts; the
            /// third suppresses the report.
            bool hasUnresolvedPath = false;
            bool hasLoopedCreate = false;
            bool hasEscapedHandle = false;
            /// Every create in the group provably reaches a join or a detach on all paths.
            bool allCreatesResolvedOnAllPaths = true;
            /// False once the group spans several functions, is aliased by a move, or carries a
            /// propagated fact: the intraprocedural path conclusions no longer describe it.
            bool pathAnalysisApplies = true;
        };

        class HandleGroupAliases
        {
          public:
            void unite(const std::string& lhs, const std::string& rhs)
            {
                const std::string lhsRoot = find(lhs);
                const std::string rhsRoot = find(rhs);
                if (lhsRoot == rhsRoot)
                    return;

                const bool lhsIsArgument = lhsRoot.starts_with("arg:");
                const bool rhsIsArgument = rhsRoot.starts_with("arg:");
                if (lhsIsArgument != rhsIsArgument)
                {
                    parent_[lhsIsArgument ? lhsRoot : rhsRoot] = lhsIsArgument ? rhsRoot : lhsRoot;
                    return;
                }

                if (lhsRoot < rhsRoot)
                    parent_[rhsRoot] = lhsRoot;
                else
                    parent_[lhsRoot] = rhsRoot;
            }

            std::string find(const std::string& groupId)
            {
                const auto [it, inserted] = parent_.try_emplace(groupId, groupId);
                if (inserted || it->second == groupId)
                    return groupId;

                it->second = find(it->second);
                return it->second;
            }

          private:
            std::unordered_map<std::string, std::string> parent_;
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
        HandleGroupAliases aliases;
        for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
        {
            if (fact.action == ThreadLifecycleAction::Move && fact.sourceHandleGroupId.has_value())
            {
                aliases.unite(fact.handleGroupId, *fact.sourceHandleGroupId);
            }
        }

        std::map<std::string, LifecycleSummary> summariesByGroup;
        std::map<std::string, std::set<std::string>> functionsByGroup;
        for (const ThreadLifecycleFact& fact : facts.threadLifecycles)
        {
            if (fact.action == ThreadLifecycleAction::Move)
            {
                // A move ties two storages together; the intraprocedural resolution search only
                // looked at one of them.
                summariesByGroup[aliases.find(fact.handleGroupId)].pathAnalysisApplies = false;
                continue;
            }

            const std::string groupId = aliases.find(fact.handleGroupId);
            LifecycleSummary& summary = summariesByGroup[groupId];
            summary.handleKind = fact.handleKind;
            summary.functionId = fact.functionId;
            functionsByGroup[groupId].insert(fact.functionId);
            if (fact.propagated)
                summary.pathAnalysisApplies = false;
            if (summary.firstCreateLocation.file.empty() && summary.firstCreateLocation.line == 0 &&
                fact.action == ThreadLifecycleAction::Create)
            {
                summary.firstCreateLocation = fact.location;
            }

            switch (fact.action)
            {
            case ThreadLifecycleAction::Create:
                ++summary.createCount;
                summary.hasUnresolvedPath = summary.hasUnresolvedPath || !fact.resolvedOnAllPaths;
                summary.allCreatesResolvedOnAllPaths =
                    summary.allCreatesResolvedOnAllPaths && fact.resolvedOnAllPaths;
                summary.hasLoopedCreate = summary.hasLoopedCreate || fact.insideLoop;
                summary.hasEscapedHandle =
                    summary.hasEscapedHandle || fact.escapedToUntrackedStorage;
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
            case ThreadLifecycleAction::Move:
                break;
            }
        }

        for (auto& [groupId, summary] : summariesByGroup)
        {
            if (functionsByGroup[groupId].size() > 1)
                summary.pathAnalysisApplies = false;
        }

        DiagnosticReport report;
        for (const auto& [groupId, summary] : summariesByGroup)
        {
            if (groupId.starts_with("arg:"))
                continue;

            // A handle moved into storage the analysis cannot name may well be joined through it;
            // reporting would be a guess rather than a finding.
            if (summary.hasEscapedHandle)
                continue;

            const std::size_t resolvedCount = summary.joinCount + summary.detachCount;
            const bool unbalanced = summary.createCount > resolvedCount;

            // Counts can be unbalanced while every handle is still resolved: two creations on
            // mutually exclusive branches share the single join that follows them.
            if (unbalanced && summary.createCount != 0 && summary.allCreatesResolvedOnAllPaths &&
                !summary.hasLoopedCreate)
            {
                continue;
            }

            const bool pathSignal = summary.pathAnalysisApplies &&
                                    (summary.hasUnresolvedPath || summary.hasLoopedCreate) &&
                                    summary.createCount != 0;
            if (!unbalanced && !pathSignal)
                continue;

            const std::size_t outstandingCount =
                unbalanced ? summary.createCount - resolvedCount : 1;
            DiagnosticBuilder builder(report, RuleId::MissingJoin);
            builder.primaryLocation(summary.firstCreateLocation)
                .message("thread handle is not joined or detached before scope exit")
                .note("handle kind: " + handleKindLabel(summary.handleKind))
                .note("lifecycle summary: " + describeSummary(summary))
                .note("outstanding joinable handles: " + std::to_string(outstandingCount));

            if (!unbalanced && summary.hasLoopedCreate)
            {
                builder.note("the handle is created inside a loop while it is resolved outside it, "
                             "so every iteration but the last leaks its thread");
            }
            else if (!unbalanced && summary.hasUnresolvedPath)
            {
                builder.note("at least one path leaves the creation without reaching a join or a "
                             "detach");
            }

            builder.property("handleKind", handleKindLabel(summary.handleKind))
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
