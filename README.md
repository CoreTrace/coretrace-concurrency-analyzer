# coretrace-concurrency-analyzer

CoreTrace concurrency analyzer for C and C++ source files compiled to in-memory LLVM IR.

The project follows the `coretrace-stack-analyzer` conventions:
- CMake-based build with LLVM/Clang integration.
- `coretrace-compiler` / `compilerlib` used as the compilation backend.
- Public C++ API for compilation and analysis.
- CLI wrapper for local analysis runs.
- Consumer example in `extern-project/`.

## Current Scope

The repository no longer stops at IR compilation. It currently provides a single-translation-unit
concurrency analysis pipeline on top of the generated `llvm::Module`.

Supported analysis rules:
- `DataRaceGlobal`: unsynchronized concurrent access detection for shared globals.
- `MissingJoin`: joinable thread handles not joined or detached before scope exit.
- `DeadlockLockOrder`: simple lock-order inversion and direct-call self-deadlock detection.

Current implementation boundaries:
- Single TU only.
- Direct-call interprocedural propagation is supported for thread context, thread lifecycle, and
  lock state.
- `MissingJoin` supports both `pthread` and `std::thread`.
- `DeadlockLockOrder` is intentionally conservative and does not yet model arbitrary `3+` lock
  cycles across the whole program.
- Multi-TU analysis, compile database ingestion, incremental cache, and full LLVM alias-analysis
  coverage are not finished yet.

## High-Level Architecture

The analyzer is intentionally layered so compilation concerns stay isolated from analysis concerns:

- CLI / consumer layer: parse options, invoke compilation, render reports.
- `InMemoryIRCompiler`: validate request, build compile commands, invoke the backend, load LLVM IR.
- Analysis facts layer: build translation-unit facts from the `llvm::Module`.
- Propagation layer: derive reusable interprocedural facts such as thread reachability, lifecycle,
  and effective held locks at call sites.
- Checker layer: run rule-specific analyzers on top of the shared facts.
- Reporting layer: emit human, JSON, or SARIF diagnostics.

This separation is preferable to a monolithic checker because new rules can reuse the same facts and
propagation passes without duplicating LLVM traversal logic.

## Backend Dependency

The project intentionally depends on `compilerlib` (`coretrace-compiler`) as the compilation
backend, but the dependency is isolated behind internal interfaces:

- `InMemoryIRCompiler`: orchestration and error mapping.
- `CompileCommandBuilder`: normalize and construct compile arguments.
- `ICompilationBackend` / `CompilerLibBackend`: invoke `compilerlib`.
- `IIRLoader` / `LLVMIRLoader`: parse `ll` or `bc` payloads into `llvm::Module`.

`InMemoryIRCompiler` keeps a stable public `compile(...)` API and also supports dependency
injection for architecture-level tests.

## Build (LLVM/Clang 20)

```bash
cmake -S . -B build-llvm20 \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm \
  -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang \
  -DCLANG_EXECUTABLE=/opt/homebrew/opt/llvm@20/bin/clang \
  -DCLANG_RESOURCE_DIR=/opt/homebrew/opt/llvm@20/lib/clang/20

cmake --build build-llvm20 -j4
```

## CLI Usage

Compile only:

```bash
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --ir-format=ll
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --ir-format=bc
```

Analyze with all available rules enabled by default:

```bash
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --format=human
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --format=json
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --format=sarif
```

Select one or more rules explicitly:

```bash
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --rules=data-race
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --rules=missing-join
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --rules=data-race,missing-join
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --rules=all
```

Supported CLI options:
- `--ir-format=ll|bc`
- `--compile-arg=<arg>` repeatable
- `--instrument`
- `--analyze`
- `--rules=data-race|missing-join|deadlock-lock-order|all`
- `--format=human|json|sarif`
- `--verbose`
- `--` to forward all trailing compiler args

Notes:
- `--analyze` enables all currently available rules by default.
- `--rules=...` acts as an explicit rule filter.
- `--rules` and `--format` require `--analyze`.

## Public API

Compilation API:
- `CompileRequest`
- `CompileResult`
- `InMemoryIRCompiler`

Analysis API:
- `AnalysisOptions`
- `SingleTUConcurrencyAnalyzer`
- `DiagnosticReport`

Minimal example:

```cpp
#include "coretrace_concurrency_analyzer.hpp"
#include "coretrace_concurrency_analysis.hpp"

#include <llvm/IR/LLVMContext.h>

llvm::LLVMContext context;

ctrace::concurrency::CompileRequest request{
    .inputFile = "sample.cpp",
};

ctrace::concurrency::InMemoryIRCompiler compiler;
const auto result = compiler.compile(request, context);

if (!result.success)
    return 1;

ctrace::concurrency::SingleTUConcurrencyAnalyzer analyzer;
const auto report = analyzer.analyze(*result.module);
```

Use `AnalysisOptions` when you want a subset of rules instead of the default all-rules behavior.

## Output Model

Structured diagnostics expose:
- severity
- rule identifier
- message
- source location
- related locations
- notes
- rule-specific properties

Rendered formats:
- `human`
- `json`
- `sarif`

## Trust Model for `--compile-arg` / `extraCompileArgs`

`extraCompileArgs` are forwarded as raw compiler arguments to `compilerlib::compile(...)`
without sanitization. They are not shell-expanded by this tool, but they still influence
compilation behavior and file access done by the compiler toolchain.

Use this API or CLI only with trusted inputs, or place an explicit allowlist in front of it for
untrusted callers.

## External Consumer Example

```bash
cmake -S extern-project -B extern-project/build-llvm20 \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm \
  -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang \
  -DCLANG_EXECUTABLE=/opt/homebrew/opt/llvm@20/bin/clang \
  -DCLANG_RESOURCE_DIR=/opt/homebrew/opt/llvm@20/lib/clang/20

cmake --build extern-project/build-llvm20 -j4

./extern-project/build-llvm20/concurrency_consumer /tmp/sample.c --ir-format=ll
./extern-project/build-llvm20/concurrency_consumer /tmp/sample.c --analyze --rules=all
```

`concurrency_consumer` keeps backward compatibility with the legacy positional format (`ll|bc`) as
second argument.

## Error Model

`CompileResult` exposes a structured `CompileError`:
- `error.code`: typed `std::error_code` backed by `CompileErrc`
- `error.phase`: coarse pipeline stage
- `error.message`: contextual details such as input path, parser diagnostics, or backend details

Use `formatCompileError(result.error)` to render a stable CLI or log-friendly message.

## Test Plan

Run the C++ test suite with CTest:

```bash
ctest --test-dir build --output-on-failure
```

Replace `build` with your configured build directory, for example `build-llvm20`.

Optional Python integration tests:

```bash
python3 tests/integration/cli/test_analyzer.py
CORETRACE_ANALYZER_BIN=./build/coretrace_concurrency_analyzer \
python3 -m pytest tests/integration/cli/test_human_output_golden.py
```

## Code Style

- Format: `./scripts/format.sh`
- Check: `./scripts/format-check.sh`
- Naming and style conventions: see `CONTRIBUTING.md`
- Fixture corpora under `tests/fixtures/` are excluded from clang-format checks

## License

This project is licensed under the Apache License 2.0.
See [LICENSE](LICENSE) and [NOTICE](NOTICE).
