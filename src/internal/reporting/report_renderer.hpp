// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

namespace ctrace::concurrency::internal::reporting
{
    struct RenderContext
    {
        std::string toolName;
        std::string inputFile;
        std::string mode;
        std::int64_t analysisTimeMs = -1;
        std::filesystem::path sourceRoot;
    };

    [[nodiscard]] std::string renderReport(const DiagnosticReport& report,
                                           const RenderContext& context, OutputFormat format);
} // namespace ctrace::concurrency::internal::reporting
