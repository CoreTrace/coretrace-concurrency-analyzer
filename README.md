# coretrace-concurrency-analyzer

Bootstrap CoreTrace tool aligned with `coretrace-stack-analyzer` conventions:
- CMake stack-style setup (`cmake_minimum_required(VERSION 3.21)`, LLVM version check).
- `coretrace-compiler` integration via `cmake/compiler/coretrace-compiler.cmake`.
- Public C++ API + CLI wrapper to compile source files to in-memory LLVM IR (`ll` or `bc`).
- Consumer example in `extern-project/`.

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
- `error.message`: contextual details (file path, parser diagnostics, backend details, ...)

Use `formatCompileError(result.error)` to render a stable CLI/log-friendly message.

## Code style (clang-format)

- Format: `./scripts/format.sh`
- Check: `./scripts/format-check.sh`
