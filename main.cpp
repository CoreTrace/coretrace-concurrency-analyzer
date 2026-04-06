// SPDX-License-Identifier: Apache-2.0
#include "coretrace_concurrency_analyzer.hpp"
#include "coretrace_concurrency_analysis.hpp"
#include "internal/diagnostics/compiler_diagnostic_parser.hpp"
#include "internal/reporting/report_renderer.hpp"

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    void printHelp()
    {
        llvm::outs()
            << "CoreTrace Concurrency Analyzer bootstrap CLI\n\n"
            << "Usage:\n"
            << "  coretrace_concurrency_analyzer <file.c|cpp> [options]\n\n"
            << "Options:\n"
            << "  --ir-format=ll|bc        Compilation output mode (default: bc)\n"
            << "  --compile-arg=<arg>      Forward a compile argument (repeatable)\n"
            << "  --instrument             Enable compilerlib instrumentation mode\n"
            << "  --analyze                Run single-TU data race analysis on the IR module\n"
            << "  --format=human|json|sarif\n"
            << "                           Diagnostic output format for --analyze (default: "
               "human)\n"
            << "  --verbose                Print request details for debugging\n"
            << "  --                       Forward all following args to compilerlib\n"
            << "  -h, --help               Show this help message\n\n"
            << "Examples:\n"
            << "  coretrace_concurrency_analyzer test.c --ir-format=ll\n"
            << "  coretrace_concurrency_analyzer test.c --ir-format=bc --compile-arg=-Iinclude\n"
            << "  coretrace_concurrency_analyzer test.c --analyze --format=human\n"
            << "  coretrace_concurrency_analyzer test.c --analyze --format=sarif\n"
            << "  coretrace_concurrency_analyzer test.c -- --std=gnu11 -Wall\n";
    }

    void printRequestSummary(const ctrace::concurrency::CompileRequest& request, bool analyze,
                             ctrace::concurrency::OutputFormat outputFormat,
                             llvm::raw_ostream& stream)
    {
        stream << "request.input-file: " << request.inputFile << "\n";
        stream << "request.ir-format: " << ctrace::concurrency::toString(request.format) << "\n";
        stream << "request.instrument: " << (request.instrument ? "true" : "false") << "\n";
        stream << "request.analyze: " << (analyze ? "true" : "false") << "\n";
        if (analyze)
            stream << "request.output-format: " << ctrace::concurrency::toString(outputFormat)
                   << "\n";
        stream << "request.extra-args-count: " << request.extraCompileArgs.size() << "\n";
        for (const std::string& arg : request.extraCompileArgs)
            stream << "request.extra-arg: " << arg << "\n";
    }

    std::size_t countDefinedFunctions(const llvm::Module& module)
    {
        std::size_t count = 0;
        for (const llvm::Function& function : module)
        {
            if (!function.isDeclaration())
                ++count;
        }
        return count;
    }

    bool parseFormat(std::string_view value, ctrace::concurrency::IRFormat& out)
    {
        if (value == "ll")
        {
            out = ctrace::concurrency::IRFormat::LL;
            return true;
        }
        if (value == "bc")
        {
            out = ctrace::concurrency::IRFormat::BC;
            return true;
        }
        return false;
    }

    bool parseOutputFormat(std::string_view value, ctrace::concurrency::OutputFormat& out)
    {
        if (value == "human")
        {
            out = ctrace::concurrency::OutputFormat::Human;
            return true;
        }

        if (value == "json")
        {
            out = ctrace::concurrency::OutputFormat::Json;
            return true;
        }

        if (value == "sarif")
        {
            out = ctrace::concurrency::OutputFormat::Sarif;
            return true;
        }

        return false;
    }

    ctrace::concurrency::internal::reporting::RenderContext
    makeRenderContext(std::string_view inputFile, std::int64_t analysisTimeMs = -1)
    {
        std::error_code currentPathError;
        const std::filesystem::path sourceRoot = std::filesystem::current_path(currentPathError);

        return ctrace::concurrency::internal::reporting::RenderContext{
            .toolName = "coretrace-concurrency-analyzer",
            .inputFile = std::string(inputFile),
            .mode = "IR",
            .analysisTimeMs = analysisTimeMs,
            .sourceRoot = currentPathError ? std::filesystem::path{} : sourceRoot,
        };
    }

    void
    emitStructuredReport(const ctrace::concurrency::DiagnosticReport& report,
                         const ctrace::concurrency::internal::reporting::RenderContext& context,
                         ctrace::concurrency::OutputFormat outputFormat)
    {
        std::string rendered =
            ctrace::concurrency::internal::reporting::renderReport(report, context, outputFormat);
        llvm::outs() << rendered;
        if (!rendered.empty() && rendered.back() != '\n')
            llvm::outs() << "\n";
    }

    bool hasStructuredLocations(const ctrace::concurrency::DiagnosticReport& report)
    {
        for (const ctrace::concurrency::Diagnostic& diagnostic : report.diagnostics)
        {
            if (diagnostic.location.line != 0 || diagnostic.location.column != 0)
                return true;
        }
        return false;
    }

    ctrace::concurrency::DiagnosticReport
    buildCompileFailureReport(const ctrace::concurrency::CompileRequest& request,
                              const ctrace::concurrency::CompileResult& result,
                              ctrace::concurrency::InMemoryIRCompiler& compiler)
    {
        ctrace::concurrency::DiagnosticReport report =
            ctrace::concurrency::internal::diagnostics::parseCompilerDiagnostics(
                result.diagnostics, result.error, request.inputFile);

        if (request.format != ctrace::concurrency::IRFormat::BC || hasStructuredLocations(report))
            return report;

        ctrace::concurrency::CompileRequest llRequest = request;
        llRequest.format = ctrace::concurrency::IRFormat::LL;

        llvm::LLVMContext retryContext;
        const ctrace::concurrency::CompileResult llResult =
            compiler.compile(llRequest, retryContext);
        if (llResult.success)
            return report;

        const ctrace::concurrency::DiagnosticReport recovered =
            ctrace::concurrency::internal::diagnostics::parseCompilerDiagnostics(
                llResult.diagnostics, llResult.error, request.inputFile);
        if (hasStructuredLocations(recovered))
            return recovered;

        if (!llResult.diagnostics.empty())
            return recovered;

        return report;
    }
} // namespace

int main(int argc, char** argv)
{
    ctrace::concurrency::CompileRequest request;
    bool analyze = false;
    bool passthroughMode = false;
    bool verbose = false;
    bool outputFormatExplicit = false;
    ctrace::concurrency::OutputFormat outputFormat = ctrace::concurrency::OutputFormat::Human;

    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg = argv[i];

        if (passthroughMode)
        {
            request.extraCompileArgs.emplace_back(arg);
            continue;
        }

        if (arg == "--")
        {
            passthroughMode = true;
            continue;
        }

        if (arg == "-h" || arg == "--help")
        {
            printHelp();
            return 0;
        }

        if (arg == "--verbose")
        {
            verbose = true;
            continue;
        }

        if (arg == "--analyze")
        {
            analyze = true;
            continue;
        }

        constexpr std::string_view formatPrefix = "--ir-format=";
        if (arg.rfind(formatPrefix, 0) == 0)
        {
            if (!parseFormat(arg.substr(formatPrefix.size()), request.format))
            {
                llvm::errs() << "Unsupported --ir-format value: " << std::string(arg) << "\n";
                return 1;
            }
            continue;
        }

        constexpr std::string_view outputFormatPrefix = "--format=";
        if (arg.rfind(outputFormatPrefix, 0) == 0)
        {
            if (!parseOutputFormat(arg.substr(outputFormatPrefix.size()), outputFormat))
            {
                llvm::errs() << "Unsupported --format value: " << std::string(arg) << "\n";
                return 1;
            }
            outputFormatExplicit = true;
            continue;
        }

        constexpr std::string_view compileArgPrefix = "--compile-arg=";
        if (arg.rfind(compileArgPrefix, 0) == 0)
        {
            request.extraCompileArgs.emplace_back(arg.substr(compileArgPrefix.size()));
            continue;
        }

        if (arg == "--instrument")
        {
            request.instrument = true;
            continue;
        }

        if (!arg.empty() && arg.front() == '-')
        {
            llvm::errs() << "Unknown option: " << std::string(arg) << "\n";
            return 1;
        }

        if (request.inputFile.empty())
        {
            request.inputFile = std::string(arg);
            continue;
        }

        llvm::errs() << "Unexpected positional argument: " << std::string(arg) << "\n";
        return 1;
    }

    if (request.inputFile.empty())
    {
        printHelp();
        return 1;
    }

    if (outputFormatExplicit && !analyze)
    {
        llvm::errs() << "--format requires --analyze\n";
        return 1;
    }

    if (verbose)
    {
        llvm::raw_ostream& stream =
            (analyze && outputFormat != ctrace::concurrency::OutputFormat::Human) ? llvm::errs()
                                                                                  : llvm::outs();
        printRequestSummary(request, analyze, outputFormat, stream);
    }

    llvm::LLVMContext context;
    ctrace::concurrency::InMemoryIRCompiler compiler;
    ctrace::concurrency::CompileResult result = compiler.compile(request, context);

    if (!analyze && !result.diagnostics.empty())
        llvm::errs() << result.diagnostics;

    if (!result.success)
    {
        if (analyze)
        {
            const ctrace::concurrency::DiagnosticReport report =
                buildCompileFailureReport(request, result, compiler);
            emitStructuredReport(report, makeRenderContext(request.inputFile), outputFormat);
            return 1;
        }

        const std::string renderedError = formatCompileError(result.error);
        if (!renderedError.empty())
            llvm::errs() << renderedError << "\n";
        else
            llvm::errs() << "unknown_compile_failure\n";
        return 1;
    }

    if (analyze)
    {
        const auto startedAt = std::chrono::steady_clock::now();
        ctrace::concurrency::SingleTUConcurrencyAnalyzer analyzer;
        const ctrace::concurrency::DiagnosticReport report = analyzer.analyze(*result.module);
        const auto finishedAt = std::chrono::steady_clock::now();

        const auto duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(finishedAt - startedAt).count();

        emitStructuredReport(report, makeRenderContext(request.inputFile, duration), outputFormat);
        return 0;
    }

    const std::size_t payloadBytes = (request.format == ctrace::concurrency::IRFormat::BC)
                                         ? result.llvmBitcode.size()
                                         : result.llvmIRText.size();

    llvm::outs() << "IR compilation succeeded\n";
    llvm::outs() << "format: " << ctrace::concurrency::toString(request.format) << "\n";
    llvm::outs() << "payload-bytes: " << payloadBytes << "\n";
    llvm::outs() << "defined-functions: " << countDefinedFunctions(*result.module) << "\n";

    return 0;
}
