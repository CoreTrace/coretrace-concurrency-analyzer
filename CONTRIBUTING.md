# Contributing

## Code Style

- Formatting:
  - Run `./scripts/format.sh` before committing.
  - Validate with `./scripts/format-check.sh`.
- Language level:
  - Use C++20.
  - Prefer simple, explicit constructs and avoid hardcoded environment-specific paths.

## Naming Conventions

- Files and directories: `snake_case`
  - Example: `coretrace_concurrency_analyzer.cpp`
- Types (`class`, `struct`, `enum`, `enum class`): `PascalCase`
  - Example: `InMemoryIRCompiler`, `CompileResult`, `IRFormat`
- Functions and methods: `camelCase`
  - Example: `buildBCCompileArgs`, `compile`, `formatCompileError`
- Variables and fields: `camelCase`
  - Example: `inputFile`, `extraCompileArgs`, `bitcodeOutputPath`
- CMake variables/options: `UPPER_SNAKE_CASE` when global/config-like
  - Example: `BUILD_CLI`, `LLVM_MIN_REQUIRED_VERSION`

## Notes

- Keep naming consistent in new code and refactors.
- When integrating external dependencies, preserve upstream naming if required by their APIs.
