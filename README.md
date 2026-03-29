# coretrace-concurrency-analyzer

Bootstrap CoreTrace tool aligned with `coretrace-stack-analyzer` conventions:
- CMake stack-style setup (`cmake_minimum_required(VERSION 3.21)`, LLVM version check).
- `coretrace-compiler` integration via `cmake/compiler/coretrace-compiler.cmake`.
- Public C++ API + CLI wrapper to compile source files to in-memory LLVM IR (`ll` or `bc`).
- Consumer example in `extern-project/`.

## Current Scope

This repository currently provides a compilation bootstrap (source -> LLVM IR in memory).
It does not yet emit concurrency diagnostics. The analyzer logic will be layered on top of the
`llvm::Module` produced by the API.

## Backend Dependency

The project intentionally depends on `compilerlib` (`coretrace-compiler`) as the compilation
backend, but this dependency is now isolated behind an internal backend interface:

- CLI/consumer layer: parse options, render diagnostics/errors.
- `InMemoryIRCompiler` (orchestrator): validate request, choose format (`ll|bc`), map failures.
- `CompileCommandBuilder`: normalize/construct compile arguments.
- `ICompilationBackend` / `CompilerLibBackend`: invoke `compilerlib` (memory LL or file BC).
- `IIRLoader` / `LLVMIRLoader`: parse LLVM payloads (`parseIR`, `parseBitcodeFile`) into `llvm::Module`.
- Future analyzer layer: consume `llvm::Module` independently from CLI/backend choices.

`InMemoryIRCompiler` keeps the same public `compile(...)` API and now also supports dependency
injection (backend/loader constructor) for architecture-level tests.

## Build (LLVM/Clang 20)

```bash
cmake -S . -B build-llvm20 \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm \
  -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang \
  -DCLANG_EXECUTABLE=/opt/homebrew/opt/llvm@20/bin/clang \
  -DCLANG_RESOURCE_DIR=/opt/homebrew/opt/llvm@20/lib/clang/20

cmake --build build-llvm20 -j4
```

## CLI usage

```bash
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --ir-format=ll
./build-llvm20/coretrace_concurrency_analyzer /tmp/sample.c --ir-format=bc
```

Options:
- `--ir-format=ll|bc`
- `--compile-arg=<arg>` (repeatable)
- `--instrument`
- `--verbose`
- `--` to forward all trailing compiler args

### Trust model for `--compile-arg` / `extraCompileArgs`

`extraCompileArgs` are forwarded as raw compiler arguments to `compilerlib::compile(...)`
without sanitization. They are not shell-expanded by this tool, but they still influence
compilation behavior and file access done by the compiler toolchain.

Use this API/CLI only with trusted inputs, or implement an explicit allowlist in front of it
for untrusted callers.

## External consumer example

```bash
cmake -S extern-project -B extern-project/build-llvm20 \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm \
  -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang \
  -DCLANG_EXECUTABLE=/opt/homebrew/opt/llvm@20/bin/clang \
  -DCLANG_RESOURCE_DIR=/opt/homebrew/opt/llvm@20/lib/clang/20

cmake --build extern-project/build-llvm20 -j4
./extern-project/build-llvm20/concurrency_consumer /tmp/sample.c --ir-format=ll
./extern-project/build-llvm20/concurrency_consumer /tmp/sample.c --ir-format=bc
```

`concurrency_consumer` keeps backward compatibility with the legacy positional format (`ll|bc`) as second argument.

## Error model (library API)

`CompileResult` now exposes a structured `CompileError`:
- `error.code`: typed `std::error_code` backed by `CompileErrc` (`coretrace_concurrency_error.hpp`)
- `error.phase`: coarse pipeline stage (`validate_input`, `build_command`, `backend_compile`, `ir_parse`)
- `error.message`: contextual details (file path, parser diagnostics, backend details, ...)

Use `formatCompileError(result.error)` to render a stable CLI/log-friendly message.

## Test Plan

Run the C++ test suite (unit + CLI integration) with CTest:

```bash
ctest --test-dir build --output-on-failure
```

Optional Python integration tests (requires `ctestfw`):

```bash
python3 tests/integration/cli/test_analyzer.py
```

## Code style (clang-format)

- Format: `./scripts/format.sh`
- Check: `./scripts/format-check.sh`
- Naming/style conventions: see `CONTRIBUTING.md`
- Fixture corpora under `tests/fixtures/` are excluded from clang-format checks.

## License

This project is licensed under the Apache License 2.0.
See [LICENSE](LICENSE) and [NOTICE](NOTICE).
