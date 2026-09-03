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

Supported analysis rules. The name in the second column is what `--rules=` accepts.

| Rule | `--rules=` | Reports |
| --- | --- | --- |
| `DataRaceGlobal` | `data-race` | shared globals accessed concurrently with no common lock |
| `MissingJoin` | `missing-join` | joinable thread handles left neither joined nor detached |
| `DeadlockLockOrder` | `deadlock-lock-order` | lock-order inversions and self-deadlock |
| `ConditionWaitWithoutPredicate` | `condition-wait` | a condition-variable wait that rechecks nothing when it wakes |
| `ForkAfterThreadCreation` | `fork-after-thread` | a `fork` in a threaded program with no `exec` in the child |
| `UnreapedChildProcess` | `unreaped-child` | a `fork` whose children are never collected |
| `ThreadArgumentEscapesFrame` | `thread-arg-escape` | a thread given a pointer into the frame that created it |
| `UnsafeSignalHandler` | `unsafe-signal-handler` | a signal handler reaching a call it may not make |

What each of the newer rules establishes, and what it deliberately does not:

- **`condition-wait`** — a wait may return without the condition holding: the standard permits a
  spurious wake-up, and a broadcast wakes every waiter while only one may proceed. A bare wait
  with no loop around it therefore reads waking up as proof. A helper cannot recheck a condition
  it does not know, so the obligation to loop travels to its caller and keeps travelling until
  some caller does loop; only the outermost function still carrying it is reported. *Not covered:*
  a wait inside a loop that rechecks the wrong condition.
- **`fork-after-thread`** — only the calling thread survives a `fork`, while the whole address
  space is inherited: a mutex another thread held is copied locked with nobody left to unlock it.
  An `exec` reachable from the forking function settles the question and suppresses the report.
  *Not proven:* that the thread creation actually runs before the fork.
- **`unreaped-child`** — a `fork` whose pid is never waited on leaves every finished child in the
  process table. Handing `SIGCHLD` to `SIG_IGN` counts as reaping. *Not tracked:* which pid a
  given `wait` collects; `SA_NOCLDWAIT` through `sigaction` is unrecognized.
- **`thread-arg-escape`** — a thread argument outlives the call that passed it unless the creator
  waits, so a pointer to a local dangles as soon as that function returns. *Covers* `pthread`
  creation only.
- **`unsafe-signal-handler`** — a handler interrupts its own thread at an arbitrary instruction,
  so allocating, printing or locking there re-enters a structure the interrupted code may have
  left inconsistent. The unsafety travels back along direct calls to the handler. *Covers*
  handlers installed through `signal` and `sigaction`.

Current implementation boundaries:
- Whole-project analysis reads a `compile_commands.json`; four kinds of fact cross the unit
  boundary and nothing else does.
- Direct-call interprocedural propagation is supported for thread context, thread lifecycle, and
  lock state.
- `MissingJoin` supports both `pthread` and `std::thread`.
- `DeadlockLockOrder` is intentionally conservative and does not yet model arbitrary `3+` lock
  cycles across the whole program.
- Shared state reached through a pointer — a field of an object on the heap or the stack — is not
  tracked; only globals are. This is the single largest gap on idiomatic C++.
- Thread entries reached through a pointer-to-member, as in `std::thread(&Class::method, obj)`,
  are not resolved, so those methods are not seen as running in a thread.
- Full LLVM alias-analysis coverage is not finished yet.
- Sources are always compiled unoptimized: the analysis describes the program as written, and an
  optimizer removes the very structure the rules read.

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

## Releases

Released versions are tagged `vX.Y.Z`. The version lives in `project(... VERSION ...)`
in `CMakeLists.txt`, the binary reports it under `--version`, and the release workflow
refuses a tag that disagrees with the tree.

The published image carries the clang 20 the analyzer needs, so it needs nothing
installed beyond a container runtime:

```bash
docker run --rm -v "$PWD:/work" \
  ghcr.io/coretrace/coretrace-concurrency-analyzer:v0.2.0 file.c --analyze
```

To build a specific release from source instead, consume the tag through CMake:

```cmake
FetchContent_Declare(
  concurrency_analyzer
  GIT_REPOSITORY https://github.com/CoreTrace/coretrace-concurrency-analyzer.git
  GIT_TAG v0.2.0
)
```

Cutting a release is pushing a tag. `.github/workflows/release.yml` then checks the
tag against the tree, rebuilds and retests on Linux and macOS, publishes the image,
and only then creates the GitHub release — so a release that exists is one that
built and passed. Versions follow the Conventional Commits this repository already
enforces; see [CHANGELOG.md](CHANGELOG.md).

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
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --rules=condition-wait
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --analyze --rules=all
```

Analyze a whole project instead of a single file, from the compilation database its build
already produces. See [docs/cross-tu-mode.md](docs/cross-tu-mode.md) for what crosses the unit
boundary and what does not:

```bash
./build-llvm20/coretrace_concurrency_analyzer --compile-commands=build/compile_commands.json
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

## GitHub Action

`action.yml` at the root of this repository runs the published container image,
so a job starts in seconds rather than installing LLVM and building the
analyzer.

```yaml
permissions:
  contents: read
  security-events: write   # only needed while upload-sarif is true

steps:
  - uses: actions/checkout@v4
  - uses: CoreTrace/coretrace-concurrency-analyzer@v1
    with:
      sources: src/worker.c src/pool.c
      fail-on: error
```

Whole-project analysis takes a compilation database instead:

```yaml
  - uses: CoreTrace/coretrace-concurrency-analyzer@v1
    with:
      compile-commands: build/compile_commands.json
```

**Generate that database on the runner.** It records absolute paths and the
toolchain that produced it, so one generated on a developer's macOS machine
names an SDK the Linux container does not have, and every unit fails to
compile. The action mounts the workspace at its own path precisely so a
database made on the runner resolves unchanged inside the container.

| Input | Default | |
| --- | --- | --- |
| `sources` | | Files to analyze, space-separated |
| `compile-commands` | | Compilation database; takes precedence over `sources` |
| `rules` | `all` | Comma-separated rule selection |
| `fail-on` | `error` | `none`, `error`, or `warning` |
| `sarif-file` | `coretrace-concurrency.sarif` | Where the report is written |
| `upload-sarif` | `true` | Send results to Code Scanning |
| `version` | the release this action ships with | Analyzer image tag |
| `extra-args` | | Forwarded to the analyzer verbatim |

Outputs are `sarif-file`, `errors` and `warnings`. The SARIF is uploaded before
the gate fails the job, so the run that stops the build is not the run whose
findings get lost.

When the gate fails the job, the action's `errors` and `warnings` outputs are
**empty**: GitHub does not evaluate a composite action's outputs when it fails,
so they are unavailable exactly when findings exist. Read `sarif-file` from the
workspace instead, or set `fail-on: none` and decide for yourself:

```yaml
  - uses: CoreTrace/coretrace-concurrency-analyzer@v1
    id: scan
    with:
      sources: src/worker.c
      fail-on: none
  - run: |
      test "${{ steps.scan.outputs.errors }}" -eq 0 || exit 1
```

Note that `fail-on` defaults to `error` here while the CLI defaults to `none`:
a CI job is asked to have an opinion, a command line is not.

## Exit Codes

The exit code is what a CI job reads, so it distinguishes *the analysis found
something* from *the analysis could not run* — a pipeline that conflates them
reports a crashed tool as a clean tree.

| Code | Meaning |
| --- | --- |
| `0` | The analysis ran, and nothing tripped `--fail-on`. |
| `1` | The analysis could not produce a verdict: bad arguments, a source that would not compile, a unit missing from a compilation database. |
| `2` | The analysis ran and reported at or above the `--fail-on` severity. |

`--fail-on=none|error|warning` selects the gate, and defaults to `none`: the
tool reports, and the caller decides what is fatal. In CI you usually want
`--fail-on=error`.

```bash
coretrace_concurrency_analyzer src/worker.c --analyze --format=sarif --fail-on=error
```

A unit that would not compile outranks the gate. `1` wins over `2`, because a
tree that was never fully seen cannot be pronounced clean.

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
