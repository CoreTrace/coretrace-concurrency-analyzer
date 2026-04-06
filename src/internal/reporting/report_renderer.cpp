// SPDX-License-Identifier: Apache-2.0
#include "report_renderer.hpp"

#include "internal/diagnostics/diagnostic_catalog.hpp"

#include <llvm/Support/FormatVariadic.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MD5.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <variant>

namespace ctrace::concurrency::internal::reporting
{
    namespace
    {
        using internal::diagnostics::lookupRuleMetadata;

        std::string severityUpper(Severity severity)
        {
            switch (severity)
            {
            case Severity::Info:
                return "INFO";
            case Severity::Warning:
                return "WARNING";
            case Severity::Error:
                return "ERROR";
            }
            return "UNKNOWN";
        }

        std::string severityTitle(Severity severity)
        {
            switch (severity)
            {
            case Severity::Info:
                return "Info";
            case Severity::Warning:
                return "Warning";
            case Severity::Error:
                return "Error";
            }
            return "Unknown";
        }

        std::string confidenceUpper(ConfidenceLevel confidence)
        {
            switch (confidence)
            {
            case ConfidenceLevel::Low:
                return "LOW";
            case ConfidenceLevel::Medium:
                return "MEDIUM";
            case ConfidenceLevel::High:
                return "HIGH";
            }
            return "UNKNOWN";
        }

        std::string sarifLevel(Severity severity)
        {
            switch (severity)
            {
            case Severity::Info:
                return "note";
            case Severity::Warning:
                return "warning";
            case Severity::Error:
                return "error";
            }
            return "warning";
        }

        std::optional<std::string> propertyString(const Diagnostic& diagnostic,
                                                  std::string_view key)
        {
            const auto it = diagnostic.properties.find(std::string(key));
            if (it == diagnostic.properties.end())
                return std::nullopt;

            if (const auto* value = std::get_if<std::string>(&it->second))
                return *value;
            return std::nullopt;
        }

        std::string effectiveFile(const SourceLocation& location, const RenderContext& context)
        {
            if (!location.file.empty())
                return location.file;
            return context.inputFile;
        }

        std::string formatLocationHeader(const SourceLocation& location,
                                         const RenderContext& context)
        {
            const std::string file = effectiveFile(location, context);
            if (location.line != 0 && !file.empty())
                return file + ":" + std::to_string(location.line) + ":" +
                       std::to_string(location.column);
            if (!file.empty())
                return file;
            return "<unknown-location>";
        }

        std::string renderDiagnosticBody(const Diagnostic& diagnostic)
        {
            std::string rendered;
            llvm::raw_string_ostream stream(rendered);
            stream << "[!!!" << severityTitle(diagnostic.severity) << "] " << diagnostic.message;
            for (const DiagnosticNote& note : diagnostic.notes)
                stream << "\n\t     ↳ " << note.text;
            stream << "\n";
            return rendered;
        }

        std::optional<std::string> firstCweIdentifier(const Diagnostic& diagnostic)
        {
            for (const TaxonomyRef& taxonomy : diagnostic.taxonomies)
            {
                if (taxonomy.scheme == "CWE")
                    return taxonomy.scheme + "-" + taxonomy.id;
            }
            return std::nullopt;
        }

        llvm::json::Value toJsonValue(const DiagnosticPropertyValue& value)
        {
            return std::visit(
                [](const auto& concreteValue) -> llvm::json::Value
                {
                    using ValueType = std::decay_t<decltype(concreteValue)>;
                    if constexpr (std::is_same_v<ValueType, bool>)
                    {
                        return llvm::json::Value(concreteValue);
                    }
                    else if constexpr (std::is_same_v<ValueType, std::int64_t>)
                    {
                        return llvm::json::Value(static_cast<int64_t>(concreteValue));
                    }
                    else if constexpr (std::is_same_v<ValueType, std::string>)
                    {
                        return llvm::json::Value(concreteValue);
                    }
                    else
                    {
                        llvm::json::Array items;
                        for (const std::string& item : concreteValue)
                            items.emplace_back(item);
                        return llvm::json::Value(std::move(items));
                    }
                },
                value);
        }

        llvm::json::Object toJsonLocation(const SourceLocation& location,
                                          const RenderContext& context)
        {
            return llvm::json::Object{
                {"file", effectiveFile(location, context)},
                {"function", location.function},
                {"startLine", static_cast<int64_t>(location.line)},
                {"startColumn", static_cast<int64_t>(location.column)},
                {"endLine",
                 static_cast<int64_t>(location.endLine == 0 ? location.line : location.endLine)},
                {"endColumn", static_cast<int64_t>(location.endColumn == 0 ? location.column
                                                                           : location.endColumn)},
            };
        }

        llvm::json::Object toSarifLocation(const SourceLocation& location,
                                           const RenderContext& context)
        {
            llvm::json::Object artifact{
                {"uri",
                 [&]()
                 {
                     const std::string file = effectiveFile(location, context);
                     if (file.empty())
                         return std::string();

                     const std::filesystem::path file_path(file);
                     if (!context.sourceRoot.empty())
                     {
                         std::error_code rel_ec;
                         const std::filesystem::path relative =
                             std::filesystem::relative(file_path, context.sourceRoot, rel_ec);
                         if (!rel_ec && !relative.empty())
                             return relative.generic_string();
                     }
                     return file_path.generic_string();
                 }()},
            };

            llvm::json::Object region;
            if (location.line != 0)
            {
                region.insert({"startLine", static_cast<int64_t>(location.line)});
                region.insert({"startColumn", static_cast<int64_t>(location.column)});
                region.insert(
                    {"endLine", static_cast<int64_t>(location.endLine == 0 ? location.line
                                                                           : location.endLine)});
                region.insert({"endColumn",
                               static_cast<int64_t>(location.endColumn == 0 ? location.column
                                                                            : location.endColumn)});
            }

            llvm::json::Object physical_location{{"artifactLocation", std::move(artifact)}};
            if (!region.empty())
                physical_location.insert({"region", std::move(region)});

            llvm::json::Object location_object{{"physicalLocation", std::move(physical_location)}};
            if (!location.function.empty())
            {
                llvm::json::Array logical_locations;
                logical_locations.emplace_back(
                    llvm::json::Object{{"name", location.function}, {"kind", "function"}});
                location_object.insert({"logicalLocations", std::move(logical_locations)});
            }
            return location_object;
        }

        std::string fingerprintFor(const Diagnostic& diagnostic, const RenderContext& context)
        {
            llvm::MD5 md5;
            const auto symbol = propertyString(diagnostic, "symbol").value_or("");
            md5.update(std::string(toString(diagnostic.ruleId)));
            md5.update(effectiveFile(diagnostic.location, context));
            md5.update(std::to_string(diagnostic.location.line));
            md5.update(std::to_string(diagnostic.location.column));
            md5.update(symbol);

            llvm::MD5::MD5Result result;
            md5.final(result);
            llvm::SmallString<32> rendered;
            llvm::MD5::stringifyResult(result, rendered);
            return std::string(rendered);
        }

        std::string renderHuman(const DiagnosticReport& report, const RenderContext& context)
        {
            std::string rendered;
            llvm::raw_string_ostream stream(rendered);

            stream << "Mode: " << context.mode << "\n";
            if (report.diagnostics.empty())
            {
                stream << "\nDiagnostics summary: info=" << report.diagnosticsSummary.info
                       << ", warning=" << report.diagnosticsSummary.warning
                       << ", error=" << report.diagnosticsSummary.error << "\n";
                return rendered;
            }

            for (const Diagnostic& diagnostic : report.diagnostics)
            {
                if (!diagnostic.location.function.empty())
                    stream << "\nFunction: " << diagnostic.location.function << "\n";
                else
                    stream << "\nLocation: " << formatLocationHeader(diagnostic.location, context)
                           << "\n";
                stream << "\tseverity: " << severityUpper(diagnostic.severity) << "\n";
                stream << "\truleId: " << toString(diagnostic.ruleId) << "\n";
                if (const auto cwe = firstCweIdentifier(diagnostic); cwe.has_value())
                    stream << "\tcwe: " << *cwe << "\n";
                if (const auto symbol = propertyString(diagnostic, "symbol"); symbol.has_value())
                    stream << "\tsymbol: " << *symbol << "\n";
                if (diagnostic.location.line != 0)
                {
                    stream << "\tat line " << diagnostic.location.line << ", column "
                           << diagnostic.location.column << "\n";
                }
                else if (!effectiveFile(diagnostic.location, context).empty())
                {
                    stream << "\tat " << effectiveFile(diagnostic.location, context)
                           << " (line unavailable)\n";
                }
                else
                {
                    stream << "\tat source location unavailable\n";
                }

                const std::string body = renderDiagnosticBody(diagnostic);
                const std::size_t body_size = body.size();
                if (body_size != 0 && body.back() == '\n')
                    stream << "\t" << body.substr(0, body_size - 1) << "\n";
                else
                    stream << "\t" << body << "\n";
            }

            stream << "\nDiagnostics summary: info=" << report.diagnosticsSummary.info
                   << ", warning=" << report.diagnosticsSummary.warning
                   << ", error=" << report.diagnosticsSummary.error << "\n";
            return rendered;
        }

        llvm::json::Value renderJsonValue(const DiagnosticReport& report,
                                          const RenderContext& context)
        {
            llvm::json::Array functions;
            for (const FunctionSummary& function : report.functions)
            {
                llvm::json::Array thread_entries;
                for (const std::string& entry : function.threadEntries)
                    thread_entries.emplace_back(entry);

                functions.emplace_back(llvm::json::Object{
                    {"file", function.file.empty() ? context.inputFile : function.file},
                    {"name", function.name},
                    {"threadReachable", function.threadReachable},
                    {"threadEntries", std::move(thread_entries)},
                    {"sharedAccessCount", static_cast<int64_t>(function.sharedAccessCount)},
                    {"protectedAccessCount", static_cast<int64_t>(function.protectedAccessCount)},
                    {"writeAccessCount", static_cast<int64_t>(function.writeAccessCount)},
                    {"hasDiagnostics", function.hasDiagnostics},
                });
            }

            llvm::json::Array diagnostics;
            for (const Diagnostic& diagnostic : report.diagnostics)
            {
                llvm::json::Object details;
                details.insert({"message", renderDiagnosticBody(diagnostic)});

                llvm::json::Array notes;
                for (const DiagnosticNote& note : diagnostic.notes)
                    notes.emplace_back(note.text);
                details.insert({"notes", std::move(notes)});

                llvm::json::Object properties;
                for (const auto& [key, value] : diagnostic.properties)
                    properties.insert({key, toJsonValue(value)});
                details.insert({"properties", std::move(properties)});

                llvm::json::Array related_locations;
                for (const RelatedLocation& related : diagnostic.relatedLocations)
                {
                    related_locations.emplace_back(llvm::json::Object{
                        {"label", related.label},
                        {"location", toJsonLocation(related.location, context)},
                    });
                }

                llvm::json::Value confidence = nullptr;
                if (diagnostic.confidence.has_value())
                    confidence = llvm::json::Value(confidenceUpper(*diagnostic.confidence));

                llvm::json::Value cwe = nullptr;
                if (const auto cwe_value = firstCweIdentifier(diagnostic); cwe_value.has_value())
                    cwe = llvm::json::Value(*cwe_value);

                diagnostics.emplace_back(llvm::json::Object{
                    {"id", diagnostic.id},
                    {"severity", severityUpper(diagnostic.severity)},
                    {"ruleId", std::string(toString(diagnostic.ruleId))},
                    {"confidence", std::move(confidence)},
                    {"cwe", std::move(cwe)},
                    {"location", toJsonLocation(diagnostic.location, context)},
                    {"relatedLocations", std::move(related_locations)},
                    {"details", std::move(details)},
                });
            }

            return llvm::json::Object{
                {"meta",
                 llvm::json::Object{
                     {"tool", context.toolName},
                     {"inputFile", context.inputFile},
                     {"mode", context.mode},
                     {"analysisTimeMs", context.analysisTimeMs},
                 }},
                {"functions", std::move(functions)},
                {"diagnostics", std::move(diagnostics)},
                {"diagnosticsSummary",
                 llvm::json::Object{
                     {"info", static_cast<int64_t>(report.diagnosticsSummary.info)},
                     {"warning", static_cast<int64_t>(report.diagnosticsSummary.warning)},
                     {"error", static_cast<int64_t>(report.diagnosticsSummary.error)},
                 }},
            };
        }

        std::string renderJson(const DiagnosticReport& report, const RenderContext& context)
        {
            const llvm::json::Value document = renderJsonValue(report, context);
            return llvm::formatv("{0:2}", document).str();
        }

        std::string renderSarif(const DiagnosticReport& report, const RenderContext& context)
        {
            std::set<RuleId> emitted_rules;
            llvm::json::Array rules;
            for (const Diagnostic& diagnostic : report.diagnostics)
            {
                if (!emitted_rules.insert(diagnostic.ruleId).second)
                    continue;

                const auto& metadata = lookupRuleMetadata(diagnostic.ruleId);
                llvm::json::Array tags;
                tags.emplace_back("concurrency");
                if (const auto cwe = firstCweIdentifier(diagnostic); cwe.has_value())
                    tags.emplace_back(*cwe);

                rules.emplace_back(llvm::json::Object{
                    {"id", std::string(toString(metadata.ruleId))},
                    {"name", std::string(toString(metadata.ruleId))},
                    {"shortDescription", llvm::json::Object{{"text", std::string(metadata.title)}}},
                    {"fullDescription",
                     llvm::json::Object{{"text", std::string(metadata.shortDescription)}}},
                    {"defaultConfiguration",
                     llvm::json::Object{{"level", sarifLevel(metadata.defaultSeverity)}}},
                    {"properties", llvm::json::Object{{"tags", std::move(tags)}}},
                });
            }

            llvm::json::Array results;
            for (const Diagnostic& diagnostic : report.diagnostics)
            {
                llvm::json::Array locations;
                locations.emplace_back(toSarifLocation(diagnostic.location, context));

                llvm::json::Array related_locations;
                for (const RelatedLocation& related : diagnostic.relatedLocations)
                {
                    llvm::json::Object related_json = toSarifLocation(related.location, context);
                    related_json.insert({"message", llvm::json::Object{{"text", related.label}}});
                    related_locations.emplace_back(std::move(related_json));
                }

                results.emplace_back(llvm::json::Object{
                    {"ruleId", std::string(toString(diagnostic.ruleId))},
                    {"level", sarifLevel(diagnostic.severity)},
                    {"message", llvm::json::Object{{"text", renderDiagnosticBody(diagnostic)}}},
                    {"locations", std::move(locations)},
                    {"relatedLocations", std::move(related_locations)},
                    {"partialFingerprints",
                     llvm::json::Object{
                         {"primaryLocationLineHash", fingerprintFor(diagnostic, context)}}},
                });
            }

            const llvm::json::Value document = llvm::json::Object{
                {"version", "2.1.0"},
                {"$schema",
                 "https://schemastore.azurewebsites.net/schemas/json/sarif-2.1.0-rtm.5.json"},
                {"runs",
                 llvm::json::Array{
                     llvm::json::Object{
                         {"tool",
                          llvm::json::Object{
                              {"driver",
                               llvm::json::Object{
                                   {"name", context.toolName},
                                   {"rules", std::move(rules)},
                               }},
                          }},
                         {"results", std::move(results)},
                     },
                 }},
            };

            return llvm::formatv("{0:2}", document).str();
        }
    } // namespace

    std::string renderReport(const DiagnosticReport& report, const RenderContext& context,
                             OutputFormat format)
    {
        switch (format)
        {
        case OutputFormat::Human:
            return renderHuman(report, context);
        case OutputFormat::Json:
            return renderJson(report, context);
        case OutputFormat::Sarif:
            return renderSarif(report, context);
        }

        return renderHuman(report, context);
    }
} // namespace ctrace::concurrency::internal::reporting
