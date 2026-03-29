# Test Suite

This directory contains integration tests for the CLI bootstrap.

Default test path (no Python dependency): C++ integration tests wired into `ctest`
(`coretrace_concurrency_cli_cpp_tests` target).

Optional extended path: `ctestfw`-based Python runner.

Run:

```bash
python3 tests/integration/cli/test_analyzer.py
```

Prerequisite:

- Python environment with `ctestfw` installed.

The script requires a built `coretrace_concurrency_analyzer` binary in either:

- `build-llvm20/coretrace_concurrency_analyzer`
- `build/coretrace_concurrency_analyzer`
