# Test Suite

This directory contains integration tests for the CLI bootstrap using the `ctestfw` framework.

Run:

```bash
python3 test/test_analyzer.py
```

Prerequisite:

- Python environment with `ctestfw` installed.

The script requires a built `coretrace_concurrency_analyzer` binary in either:

- `build-llvm20/coretrace_concurrency_analyzer`
- `build/coretrace_concurrency_analyzer`
