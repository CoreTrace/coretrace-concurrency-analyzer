// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <optional>
#include <string_view>

namespace ctrace::concurrency::internal::diagnostics
{
    struct TaxonomyMetadata
    {
        std::string_view scheme;
        std::string_view id;
        std::string_view title;
    };

    struct RuleMetadata
    {
        RuleId ruleId = RuleId::DataRaceGlobal;
        std::string_view title;
        std::string_view shortDescription;
        Severity defaultSeverity = Severity::Info;
        std::optional<TaxonomyMetadata> primaryTaxonomy;
    };

    [[nodiscard]] const RuleMetadata& lookupRuleMetadata(RuleId ruleId);
} // namespace ctrace::concurrency::internal::diagnostics
