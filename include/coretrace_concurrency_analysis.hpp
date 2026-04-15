// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace llvm
{
    class Module;
} // namespace llvm

namespace ctrace::concurrency
{
    enum class AccessKind
    {
        Read,
        Write,
    };

    enum class Severity
    {
        Info,
        Warning,
        Error,
    };

    enum class RuleId
    {
        CompilerDiagnostic,
        DataRaceGlobal,
        MissingJoin,
        DeadlockLockOrder,
    };

    enum class ConfidenceLevel
    {
        Low,
        Medium,
        High,
    };

    enum class OutputFormat
    {
        Human,
        Json,
        Sarif,
    };

    struct SourceLocation
    {
        std::string file;
        unsigned line = 0;
        unsigned column = 0;
        unsigned endLine = 0;
        unsigned endColumn = 0;
        std::string function;
    };

    struct TaxonomyRef
    {
        std::string scheme;
        std::string id;
        std::string title;
    };

    struct RelatedLocation
    {
        std::string label;
        SourceLocation location;
    };

    struct DiagnosticNote
    {
        std::string text;
    };

    using DiagnosticPropertyValue =
        std::variant<bool, std::int64_t, std::string, std::vector<std::string>>;

    struct Diagnostic
    {
        std::string id;
        Severity severity = Severity::Info;
        RuleId ruleId = RuleId::DataRaceGlobal;
        std::optional<ConfidenceLevel> confidence;
        std::vector<TaxonomyRef> taxonomies;
        SourceLocation location;
        std::vector<RelatedLocation> relatedLocations;
        std::string message;
        std::vector<DiagnosticNote> notes;
        std::map<std::string, DiagnosticPropertyValue> properties;
    };

    struct DiagnosticSummary
    {
        std::size_t info = 0;
        std::size_t warning = 0;
        std::size_t error = 0;
    };

    struct FunctionSummary
    {
        std::string file;
        std::string name;
        bool threadReachable = false;
        std::vector<std::string> threadEntries;
        std::size_t sharedAccessCount = 0;
        std::size_t protectedAccessCount = 0;
        std::size_t writeAccessCount = 0;
        bool hasDiagnostics = false;
    };

    struct DiagnosticReport
    {
        std::vector<FunctionSummary> functions;
        std::vector<Diagnostic> diagnostics;
        DiagnosticSummary diagnosticsSummary;
    };

    using AnalysisReport = DiagnosticReport;

    struct AnalysisOptions
    {
        std::vector<RuleId> enabledRules{RuleId::DataRaceGlobal, RuleId::MissingJoin,
                                         RuleId::DeadlockLockOrder};

        [[nodiscard]] bool isEnabled(RuleId ruleId) const
        {
            for (const RuleId enabledRule : enabledRules)
            {
                if (enabledRule == ruleId)
                    return true;
            }
            return false;
        }

        [[nodiscard]] static AnalysisOptions allAvailable()
        {
            return AnalysisOptions{.enabledRules = {RuleId::DataRaceGlobal, RuleId::MissingJoin,
                                                    RuleId::DeadlockLockOrder}};
        }
    };

    class SingleTUConcurrencyAnalyzer
    {
      public:
        explicit SingleTUConcurrencyAnalyzer(AnalysisOptions options = {});

        [[nodiscard]] DiagnosticReport analyze(const llvm::Module& module) const;

      private:
        AnalysisOptions options_;
    };

    constexpr std::string_view toString(AccessKind kind)
    {
        switch (kind)
        {
        case AccessKind::Read:
            return "read";
        case AccessKind::Write:
            return "write";
        }
        return "unknown";
    }

    constexpr std::string_view toString(Severity severity)
    {
        switch (severity)
        {
        case Severity::Info:
            return "info";
        case Severity::Warning:
            return "warning";
        case Severity::Error:
            return "error";
        }
        return "unknown";
    }

    constexpr std::string_view toString(RuleId ruleId)
    {
        switch (ruleId)
        {
        case RuleId::CompilerDiagnostic:
            return "CompilerDiagnostic";
        case RuleId::DataRaceGlobal:
            return "DataRaceGlobal";
        case RuleId::MissingJoin:
            return "MissingJoin";
        case RuleId::DeadlockLockOrder:
            return "DeadlockLockOrder";
        }
        return "UnknownRule";
    }

    constexpr std::string_view toString(ConfidenceLevel confidence)
    {
        switch (confidence)
        {
        case ConfidenceLevel::Low:
            return "low";
        case ConfidenceLevel::Medium:
            return "medium";
        case ConfidenceLevel::High:
            return "high";
        }
        return "unknown";
    }

    constexpr std::string_view toString(OutputFormat format)
    {
        switch (format)
        {
        case OutputFormat::Human:
            return "human";
        case OutputFormat::Json:
            return "json";
        case OutputFormat::Sarif:
            return "sarif";
        }
        return "unknown";
    }
} // namespace ctrace::concurrency
