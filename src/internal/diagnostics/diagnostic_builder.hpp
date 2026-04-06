// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include "diagnostic_catalog.hpp"

#include <string>
#include <utility>

namespace ctrace::concurrency::internal::diagnostics
{
    class DiagnosticBuilder
    {
      public:
        DiagnosticBuilder(DiagnosticReport& report, RuleId ruleId) : report_(report)
        {
            diagnostic_.ruleId = ruleId;

            const RuleMetadata& metadata = lookupRuleMetadata(ruleId);
            diagnostic_.severity = metadata.defaultSeverity;
            if (metadata.primaryTaxonomy.has_value())
            {
                const TaxonomyMetadata& taxonomy = *metadata.primaryTaxonomy;
                diagnostic_.taxonomies.push_back(TaxonomyRef{
                    .scheme = std::string(taxonomy.scheme),
                    .id = std::string(taxonomy.id),
                    .title = std::string(taxonomy.title),
                });
            }
        }

        DiagnosticBuilder& severity(Severity severity)
        {
            diagnostic_.severity = severity;
            return *this;
        }

        DiagnosticBuilder& confidence(ConfidenceLevel confidence)
        {
            diagnostic_.confidence = confidence;
            return *this;
        }

        DiagnosticBuilder& primaryLocation(SourceLocation location)
        {
            diagnostic_.location = std::move(location);
            return *this;
        }

        DiagnosticBuilder& relatedLocation(std::string label, SourceLocation location)
        {
            diagnostic_.relatedLocations.push_back(
                RelatedLocation{.label = std::move(label), .location = std::move(location)});
            return *this;
        }

        DiagnosticBuilder& message(std::string message)
        {
            diagnostic_.message = std::move(message);
            return *this;
        }

        DiagnosticBuilder& note(std::string text)
        {
            diagnostic_.notes.push_back(DiagnosticNote{.text = std::move(text)});
            return *this;
        }

        DiagnosticBuilder& taxonomy(std::string scheme, std::string id, std::string title)
        {
            diagnostic_.taxonomies.push_back(TaxonomyRef{
                .scheme = std::move(scheme),
                .id = std::move(id),
                .title = std::move(title),
            });
            return *this;
        }

        DiagnosticBuilder& property(std::string key, DiagnosticPropertyValue value)
        {
            diagnostic_.properties.insert_or_assign(std::move(key), std::move(value));
            return *this;
        }

        void emit()
        {
            report_.diagnostics.push_back(std::move(diagnostic_));
        }

      private:
        DiagnosticReport& report_;
        Diagnostic diagnostic_;
    };
} // namespace ctrace::concurrency::internal::diagnostics
