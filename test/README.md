# Test Suite

This directory contains lightweight integration tests for the CLI bootstrap.

Run:

```bash
python3 test/test_analyzer.py
```

The script requires a built `coretrace_concurrency_analyzer` binary in either:

- `build-llvm20/coretrace_concurrency_analyzer`
- `build/coretrace_concurrency_analyzer`
