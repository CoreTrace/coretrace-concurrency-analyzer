// SPDX-License-Identifier: Apache-2.0
#include "compiler_diagnostic_parser.hpp"

#include "diagnostic_builder.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace ctrace::concurrency::internal::diagnostics
{
    namespace
    {
        struct ParsedDiagnosticLine
        {
            SourceLocation location;
            Severity severity = Severity::Error;
            std::string severityLabel;
            std::string message;
            bool hasStructuredLocation = false;
        };

        std::string trim(std::string_view value)
        {
            std::size_t begin = 0;
            while (begin < value.size() &&
                   std::isspace(static_cast<unsigned char>(value[begin])) != 0)
            {
                ++begin;
            }

            std::size_t end = value.size();
            while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
                --end;

            return std::string(value.substr(begin, end - begin));
        }

        bool parseUnsigned(std::string_view text, unsigned& out)
        {
            if (text.empty())
                return false;

            unsigned value = 0;
            for (const char ch : text)
            {
                if (!std::isdigit(static_cast<unsigned char>(ch)))
                    return false;

                value = value * 10u + static_cast<unsigned>(ch - '0');
            }

            out = value;
            return true;
        }

        std::vector<std::string_view> splitLines(std::string_view text)
        {
            std::vector<std::string_view> lines;
            std::size_t cursor = 0;
            while (cursor < text.size())
            {
                const std::size_t lineEnd = text.find('\n', cursor);
                if (lineEnd == std::string_view::npos)
                {
                    lines.push_back(text.substr(cursor));
                    break;
                }

                lines.push_back(text.substr(cursor, lineEnd - cursor));
                cursor = lineEnd + 1;
            }

            if (text.empty())
                lines.emplace_back();

            return lines;
        }

        std::string_view toSeverityToken(std::string_view value)
        {
            if (value == "fatal error")
                return value;
            if (value == "error")
                return value;
            if (value == "warning")
                return value;
            if (value == "note")
                return value;
            if (value == "remark")
                return value;
            return {};
        }

        Severity mapSeverity(std::string_view value)
        {
            if (value == "warning")
                return Severity::Warning;
            if (value == "remark" || value == "note")
                return Severity::Info;
            return Severity::Error;
        }

        bool parseStructuredLine(std::string_view line, ParsedDiagnosticLine& parsed)
        {
            const std::size_t firstColon = line.find(':');
            if (firstColon == std::string_view::npos)
                return false;

            const std::size_t secondColon = line.find(':', firstColon + 1);
            if (secondColon == std::string_view::npos)
                return false;

            const std::size_t thirdColon = line.find(':', secondColon + 1);
            if (thirdColon == std::string_view::npos)
                return false;

            const std::size_t fourthColon = line.find(':', thirdColon + 1);
            if (fourthColon == std::string_view::npos)
                return false;

            unsigned lineNumber = 0;
            unsigned columnNumber = 0;
            if (!parseUnsigned(line.substr(firstColon + 1, secondColon - firstColon - 1),
                               lineNumber) ||
                !parseUnsigned(line.substr(secondColon + 1, thirdColon - secondColon - 1),
                               columnNumber))
            {
                return false;
            }

            const std::string severity =
                trim(line.substr(thirdColon + 1, fourthColon - thirdColon - 1));
            const std::string_view severityToken = toSeverityToken(severity);
            if (severityToken.empty())
                return false;

            parsed.location.file = std::string(line.substr(0, firstColon));
            parsed.location.line = lineNumber;
            parsed.location.column = columnNumber;
            parsed.location.endLine = lineNumber;
            parsed.location.endColumn = columnNumber;
            parsed.severity = mapSeverity(severityToken);
            parsed.severityLabel = severity;
            parsed.message = trim(line.substr(fourthColon + 1));
            parsed.hasStructuredLocation = true;
            return true;
        }

        void addFallbackDiagnostic(DiagnosticReport& report, const CompileError& error,
                                   std::string_view inputFile, std::string_view rawDiagnostics)
        {
            SourceLocation location;
            location.file = std::string(inputFile);

            DiagnosticBuilder builder(report, RuleId::CompilerDiagnostic);
            builder.primaryLocation(std::move(location))
                .severity(Severity::Error)
                .message(error.message.empty() ? error.code.message() : error.message)
                .property("compilerSeverity", std::string("error"))
                .property("rawDiagnostic", std::string(rawDiagnostics));

            const std::string formattedError = formatCompileError(error);
            if (!formattedError.empty())
                builder.note(formattedError);

            builder.emit();
        }

        DiagnosticSummary summarize(const DiagnosticReport& report)
        {
            DiagnosticSummary summary;
            for (const Diagnostic& diagnostic : report.diagnostics)
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
    } // namespace

    DiagnosticReport parseCompilerDiagnostics(std::string_view rawDiagnostics,
                                              const CompileError& error, std::string_view inputFile)
    {
        DiagnosticReport report;

        Diagnostic* currentPrimary = nullptr;
        for (const std::string_view rawLine : splitLines(rawDiagnostics))
        {
            const std::string line = trim(rawLine);
            if (line.empty())
                continue;

            ParsedDiagnosticLine parsed;
            if (!parseStructuredLine(line, parsed))
            {
                if (currentPrimary != nullptr)
                {
                    currentPrimary->notes.push_back(DiagnosticNote{.text = line});
                }
                else
                {
                    SourceLocation location;
                    location.file = std::string(inputFile);
                    DiagnosticBuilder(report, RuleId::CompilerDiagnostic)
                        .primaryLocation(std::move(location))
                        .severity(Severity::Error)
                        .message(line)
                        .property("compilerSeverity", std::string("error"))
                        .property("rawDiagnostic", line)
                        .emit();
                    currentPrimary = &report.diagnostics.back();
                }
                continue;
            }

            if (parsed.severityLabel == "note")
            {
                if (currentPrimary != nullptr)
                {
                    currentPrimary->notes.push_back(DiagnosticNote{.text = parsed.message});
                    currentPrimary->relatedLocations.push_back(
                        RelatedLocation{.label = "Compiler note", .location = parsed.location});
                }
                else
                {
                    DiagnosticBuilder(report, RuleId::CompilerDiagnostic)
                        .primaryLocation(parsed.location)
                        .severity(parsed.severity)
                        .message(parsed.message)
                        .property("compilerSeverity", parsed.severityLabel)
                        .property("rawDiagnostic", line)
                        .emit();
                    currentPrimary = &report.diagnostics.back();
                }
                continue;
            }

            DiagnosticBuilder(report, RuleId::CompilerDiagnostic)
                .primaryLocation(parsed.location)
                .severity(parsed.severity)
                .message(parsed.message)
                .property("compilerSeverity", parsed.severityLabel)
                .property("rawDiagnostic", line)
                .emit();
            currentPrimary = &report.diagnostics.back();
        }

        if (report.diagnostics.empty())
            addFallbackDiagnostic(report, error, inputFile, rawDiagnostics);

        std::sort(report.diagnostics.begin(), report.diagnostics.end(),
                  [](const Diagnostic& lhs, const Diagnostic& rhs)
                  {
                      return std::tie(lhs.location.file, lhs.location.line, lhs.location.column,
                                      lhs.message) < std::tie(rhs.location.file, rhs.location.line,
                                                              rhs.location.column, rhs.message);
                  });

        for (std::size_t index = 0; index < report.diagnostics.size(); ++index)
            report.diagnostics[index].id = "diag-" + std::to_string(index + 1);

        report.diagnosticsSummary = summarize(report);
        return report;
    }
} // namespace ctrace::concurrency::internal::diagnostics
