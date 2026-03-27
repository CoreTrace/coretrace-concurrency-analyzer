#!/usr/bin/env python3
"""Lightweight integration tests for coretrace_concurrency_analyzer CLI."""

from __future__ import annotations

import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "test" / "fixtures"


def _resolve_binary() -> Path:
    candidate_paths = [
        ROOT
        / "extern-project"
        / "build-llvm20"
        / "_deps"
        / "concurrency_analyzer-build"
        / "coretrace_concurrency_analyzer",
        ROOT / "build-llvm20" / "coretrace_concurrency_analyzer",
        ROOT / "build" / "coretrace_concurrency_analyzer",
    ]
    for path in candidate_paths:
        if path.exists():
            return path
    raise FileNotFoundError(
        "coretrace_concurrency_analyzer not found. Build first in build-llvm20/ or build/."
    )


def _run(binary: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(binary), *args],
        text=True,
        capture_output=True,
        check=False,
    )


class AnalyzerCLITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.binary = _resolve_binary()

    def test_help(self) -> None:
        res = _run(self.binary, "--help")
        self.assertEqual(res.returncode, 0)
        self.assertIn("Usage:", res.stdout)
        self.assertIn("--ir-format=ll|bc", res.stdout)

    def test_no_args(self) -> None:
        res = _run(self.binary)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Usage:", res.stdout)

    def test_invalid_format(self) -> None:
        res = _run(self.binary, str(FIXTURES / "hello.c"), "--ir-format=xyz")
        self.assertEqual(res.returncode, 1)
        self.assertIn("Unsupported --ir-format value", res.stderr)

    def test_unknown_option(self) -> None:
        res = _run(self.binary, "--unknown")
        self.assertEqual(res.returncode, 1)
        self.assertIn("Unknown option:", res.stderr)

    def test_compile_ll_c(self) -> None:
        res = _run(self.binary, str(FIXTURES / "hello.c"), "--ir-format=ll")
        self.assertEqual(res.returncode, 0)
        self.assertIn("IR compilation succeeded", res.stdout)
        self.assertIn("format: ll", res.stdout)

    def test_compile_bc_cpp(self) -> None:
        res = _run(self.binary, str(FIXTURES / "hello.cpp"), "--ir-format=bc")
        self.assertEqual(res.returncode, 0)
        self.assertIn("IR compilation succeeded", res.stdout)
        self.assertIn("format: bc", res.stdout)

    def test_compile_default_bc(self) -> None:
        res = _run(self.binary, str(FIXTURES / "hello.c"))
        self.assertEqual(res.returncode, 0)
        self.assertIn("format: bc", res.stdout)

    def test_compile_empty_source(self) -> None:
        res = _run(self.binary, str(FIXTURES / "empty.c"))
        self.assertEqual(res.returncode, 0)
        self.assertIn("defined-functions: 0", res.stdout)

    def test_input_validation_nonexistent(self) -> None:
        res = _run(self.binary, "/tmp/_ctrace_nonexistent_abc123.c", "--ir-format=bc")
        self.assertEqual(res.returncode, 1)
        self.assertIn("input_file_does_not_exist", res.stderr)

    def test_input_validation_directory(self) -> None:
        res = _run(self.binary, "/tmp", "--ir-format=bc")
        self.assertEqual(res.returncode, 1)
        self.assertIn("input_file_not_regular", res.stderr)

    def test_invalid_source_reports_backend_failure(self) -> None:
        res = _run(self.binary, str(FIXTURES / "invalid.c"), "--ir-format=ll")
        self.assertEqual(res.returncode, 1)
        self.assertTrue(
            "backend_compilation_failed" in res.stderr or "Compilation failed" in res.stderr,
            msg=f"unexpected stderr:\n{res.stderr}",
        )

    def test_verbose_mode(self) -> None:
        res = _run(self.binary, str(FIXTURES / "hello.c"), "--ir-format=ll", "--verbose")
        self.assertEqual(res.returncode, 0)
        self.assertIn("request.input-file:", res.stdout)
        self.assertIn("request.ir-format: ll", res.stdout)

    def test_compile_arg_and_passthrough(self) -> None:
        res = _run(
            self.binary,
            str(FIXTURES / "hello.c"),
            "--ir-format=ll",
            "--compile-arg=-DFOO=1",
            "--",
            "-DBAR=1",
            "-Wall",
        )
        self.assertEqual(res.returncode, 0)
        self.assertIn("IR compilation succeeded", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
