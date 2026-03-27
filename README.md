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
- `--` to forward all trailing compiler args

## External consumer example

```bash
cmake -S extern-project -B extern-project/build-llvm20 \
  -DLLVM_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/llvm \
  -DClang_DIR=/opt/homebrew/opt/llvm@20/lib/cmake/clang \
  -DCLANG_EXECUTABLE=/opt/homebrew/opt/llvm@20/bin/clang \
  -DCLANG_RESOURCE_DIR=/opt/homebrew/opt/llvm@20/lib/clang/20

cmake --build extern-project/build-llvm20 -j4
./extern-project/build-llvm20/concurrency_consumer /tmp/sample.c ll
./extern-project/build-llvm20/concurrency_consumer /tmp/sample.c bc
```

## Code style (clang-format)

- Format: `./scripts/format.sh`
- Check: `./scripts/format-check.sh`
