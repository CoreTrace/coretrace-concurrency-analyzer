# Test Suite

This directory contains integration tests for the CLI bootstrap.

Default test path (no Python dependency): C++ integration tests wired into `ctest`
(`coretrace_concurrency_cli_cpp_tests` target).

Optional Python paths:
- `ctestfw`-based CLI runner
- `pytest`-based human-output golden tests driven by fixture comments

Run:

```bash
python3 tests/integration/cli/test_analyzer.py
python3 -m pytest tests/integration/cli/test_human_output_golden.py
CORETRACE_ANALYZER_BIN=./build/coretrace_concurrency_analyzer \
python3 -m pytest tests/integration/cli/test_human_output_golden.py
```

Prerequisite:

- Python environment with `ctestfw` installed.
- Python environment with `pytest` installed for the golden tests.

The Python scripts require a built `coretrace_concurrency_analyzer` binary in either:

- `build-llvm20/coretrace_concurrency_analyzer`
- `build/coretrace_concurrency_analyzer`
