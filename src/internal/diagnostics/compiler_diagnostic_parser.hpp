// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "coretrace_concurrency_analysis.hpp"
#include "coretrace_concurrency_error.hpp"

#include <string_view>

namespace ctrace::concurrency::internal::diagnostics
{
    [[nodiscard]] DiagnosticReport parseCompilerDiagnostics(std::string_view rawDiagnostics,
                                                            const CompileError& error,
                                                            std::string_view inputFile);
} // namespace ctrace::concurrency::internal::diagnostics
