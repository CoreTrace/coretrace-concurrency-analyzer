// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_error.hpp"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace llvm
{
    class LLVMContext;
    class Module;
} // namespace llvm

namespace ctrace::concurrency
{
    enum class AccessKind
    {
        Read,
        Write,
    };

    enum class AliasProvenance
    {
        Direct,
        MustAlias,
        MayAlias,
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
        ConditionWaitWithoutPredicate,
        ForkAfterThreadCreation,
        UnreapedChildProcess,
        ThreadArgumentEscapesFrame,
        UnsafeSignalHandler,
        WeakPublicationOrdering,
        ThreadArgumentFreedEarly,
        ThreadLocalOutlivesThread,
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
        /// Absent when no selected rule read the shared accesses, so nothing counted them.
        /// Zero means the accesses were examined and this function has none of that kind —
        /// a different statement, and the one a reader would otherwise assume.
        std::optional<std::size_t> sharedAccessCount;
        std::optional<std::size_t> protectedAccessCount;
        std::optional<std::size_t> writeAccessCount;
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
        /// Every rule by default. This list is also what `allAvailable()` returns, so a rule is
        /// added once and reaches both; naming rules here narrows the analysis to them.
        std::vector<RuleId> enabledRules{RuleId::DataRaceGlobal,
                                         RuleId::MissingJoin,
                                         RuleId::DeadlockLockOrder,
                                         RuleId::ConditionWaitWithoutPredicate,
                                         RuleId::ForkAfterThreadCreation,
                                         RuleId::UnreapedChildProcess,
                                         RuleId::ThreadArgumentEscapesFrame,
                                         RuleId::UnsafeSignalHandler,
                                         RuleId::WeakPublicationOrdering,
                                         RuleId::ThreadArgumentFreedEarly,
                                         RuleId::ThreadLocalOutlivesThread};
        /// Most units of a project held in memory at once during a project analysis. Zero
        /// means one per hardware thread. Peak memory grows with this number; wall time
        /// shrinks with it until the hardware runs out of threads.
        std::size_t maxLiveUnits = 0;

        [[nodiscard]] bool isEnabled(RuleId ruleId) const
        {
            for (const RuleId enabledRule : enabledRules)
            {
                if (enabledRule == ruleId)
                    return true;
            }
            return false;
        }

        /// Every rule the analyzer implements, which is also the default selection.
        [[nodiscard]] static AnalysisOptions allAvailable()
        {
            return AnalysisOptions{};
        }
    };

    /// One unit's IR, owned for exactly as long as the analysis holds it. The context is
    /// declared first so the module is destroyed before the context it lives in; assignment is
    /// not offered because the generated one would replace the context under a live module.
    struct LoadedUnit
    {
        std::unique_ptr<llvm::LLVMContext> context;
        std::unique_ptr<llvm::Module> module;

        LoadedUnit();
        LoadedUnit(std::unique_ptr<llvm::LLVMContext> context,
                   std::unique_ptr<llvm::Module> module);
        ~LoadedUnit();
        LoadedUnit(LoadedUnit&&) noexcept;
        LoadedUnit& operator=(LoadedUnit&&) = delete;
        LoadedUnit(const LoadedUnit&) = delete;
        LoadedUnit& operator=(const LoadedUnit&) = delete;

        [[nodiscard]] bool ok() const noexcept
        {
            return module != nullptr;
        }
    };

    /// The program as bitcode the analysis opens and closes on demand, so that only as many
    /// units as it is working on are live at once. Every load parses the same bytes into a
    /// fresh context: a unit analysed a second time sees exactly the IR it saw the first time,
    /// with or without a cache on disk.
    class ProjectUnitSource
    {
      public:
        /// `identifier` names the unit in reports (typically its source path); it becomes the
        /// module identifier and plays no part in symbol identity, which comes from the
        /// `source_filename` the bitcode carries.
        void add(std::string identifier, std::string bitcode);

        [[nodiscard]] std::size_t size() const noexcept
        {
            return units_.size();
        }
        [[nodiscard]] const std::string& identifier(std::size_t index) const
        {
            return units_.at(index).identifier;
        }
        /// Bytes of bitcode held for the whole analysis: the memory floor of a project.
        [[nodiscard]] std::size_t bitcodeBytes() const noexcept;

        /// Parses unit `index` into a context of its own. On failure the result holds no
        /// module and `error` says why. Safe to call from several threads at once.
        [[nodiscard]] LoadedUnit load(std::size_t index, CompileError& error) const;

      private:
        struct Unit
        {
            std::string identifier;
            std::string bitcode;
        };
        std::vector<Unit> units_;
    };

    /// A unit the analysis could not open. Its conclusions are missing from the report.
    struct FailedUnit
    {
        std::string identifier;
        CompileError error;
    };

    struct ProjectAnalysisReport
    {
        DiagnosticReport report;
        /// Units left out because their target ABI differs from the rest of the project.
        std::vector<std::string> skippedIncompatibleModules;
        /// Units the whole-program view forced through a second analysis.
        std::size_t reanalyzedUnitCount = 0;
        /// Units that failed to load in either pass. A unit that loaded in the first pass but
        /// not in the second is listed here and its first-pass conclusions are dropped: they
        /// are exactly what the second pass was going to correct.
        std::vector<FailedUnit> failedUnits;
        /// Most units held in memory at any one moment, as observed.
        std::size_t peakLiveUnits = 0;
        /// Wall time spent parsing bitcode, summed over every load on every thread.
        std::int64_t loadMilliseconds = 0;

        /// True when every unit contributed to the report.
        [[nodiscard]] bool complete() const noexcept
        {
            return failedUnits.empty();
        }
    };

    /// Analyses a whole program, so that a thread spawned in one translation unit is related to
    /// the worker body defined in another. Each unit is still analysed on its own terms; what
    /// crosses the boundary is the set of facts a unit cannot observe by itself.
    class ProjectConcurrencyAnalyzer
    {
      public:
        explicit ProjectConcurrencyAnalyzer(AnalysisOptions options = {});

        [[nodiscard]] ProjectAnalysisReport analyze(const ProjectUnitSource& units) const;

      private:
        AnalysisOptions options_;
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

    constexpr std::string_view toString(AliasProvenance provenance)
    {
        switch (provenance)
        {
        case AliasProvenance::Direct:
            return "direct";
        case AliasProvenance::MustAlias:
            return "must_alias";
        case AliasProvenance::MayAlias:
            return "may_alias";
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
        case RuleId::ConditionWaitWithoutPredicate:
            return "ConditionWaitWithoutPredicate";
        case RuleId::ForkAfterThreadCreation:
            return "ForkAfterThreadCreation";
        case RuleId::UnreapedChildProcess:
            return "UnreapedChildProcess";
        case RuleId::ThreadArgumentEscapesFrame:
            return "ThreadArgumentEscapesFrame";
        case RuleId::UnsafeSignalHandler:
            return "UnsafeSignalHandler";
        case RuleId::WeakPublicationOrdering:
            return "WeakPublicationOrdering";
        case RuleId::ThreadArgumentFreedEarly:
            return "ThreadArgumentFreedEarly";
        case RuleId::ThreadLocalOutlivesThread:
            return "ThreadLocalOutlivesThread";
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
