#!/usr/bin/env python3
"""Integration tests for coretrace_concurrency_analyzer CLI."""

from __future__ import annotations

import sys
from pathlib import Path

from ctestfw.assertions.compiler import assert_exit_code, assert_stdout_contains
from ctestfw.assertions.core import Assertion, require
from ctestfw.framework.reporter import ConsoleReporter
from ctestfw.framework.suite import TestSuite
from ctestfw.framework.testcase import TestCase
from ctestfw.plan import CompilePlan
from ctestfw.runner import CompilerRunner, RunnerConfig


# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "test" / "fixtures"
CONCURRENCY = ROOT / "tests" / "concurrency"
WORK = ROOT / "test" / ".work"

_bin_candidates = [
    ROOT
    / "extern-project"
    / "build-llvm20"
    / "_deps"
    / "concurrency_analyzer-build"
    / "coretrace_concurrency_analyzer",
    ROOT / "build-llvm20" / "coretrace_concurrency_analyzer",
    ROOT / "build" / "coretrace_concurrency_analyzer",
]
BIN = next((path for path in _bin_candidates if path.exists()), _bin_candidates[0])


# ---------------------------------------------------------------------------
# Custom assertions
# ---------------------------------------------------------------------------
def assert_stderr_contains(text: str) -> Assertion:
    def _check(res) -> None:
        require(
            text in (res.run.stderr or ""),
            f"stderr does not contain '{text}'\nstderr:\n{res.run.stderr}",
        )

    return Assertion(name=f"stderr_contains_{text[:30]}", check=_check)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
def _plan(name: str, source: Path, extra: list[str] | None = None) -> CompilePlan:
    """Build a CompilePlan that passes the source as a positional arg."""
    return CompilePlan(
        name=name,
        sources=[source],
        out=None,
        extra_args=extra or [],
    )


def _plan_no_source(name: str, extra: list[str] | None = None) -> CompilePlan:
    """Build a CompilePlan without any source file (for --help, etc.)."""
    return CompilePlan(
        name=name,
        sources=[],
        out=None,
        extra_args=extra or [],
    )


# ---------------------------------------------------------------------------
# 1. CLI smoke tests
# ---------------------------------------------------------------------------
tc_help = TestCase(
    name="cli_help_long",
    plan=_plan_no_source("help_long", ["--help"]),
    assertions=[
        assert_exit_code(0),
        assert_stdout_contains("Usage:"),
        assert_stdout_contains("--ir-format"),
        assert_stdout_contains("--instrument"),
    ],
)

tc_help_short = TestCase(
    name="cli_help_short",
    plan=_plan_no_source("help_short", ["-h"]),
    assertions=[
        assert_exit_code(0),
        assert_stdout_contains("Usage:"),
    ],
)

tc_no_args = TestCase(
    name="cli_no_args",
    plan=_plan_no_source("no_args"),
    assertions=[
        assert_exit_code(1),
    ],
)

tc_invalid_format = TestCase(
    name="cli_invalid_format",
    plan=_plan(
        "invalid_format",
        FIXTURES / "hello.c",
        ["--ir-format=xyz"],
    ),
    assertions=[
        assert_exit_code(1),
        assert_stderr_contains("Unsupported --ir-format"),
    ],
)

tc_unknown_option = TestCase(
    name="cli_unknown_option",
    plan=_plan_no_source("unknown_option", ["--foo"]),
    assertions=[
        assert_exit_code(1),
        assert_stderr_contains("Unknown option"),
    ],
)

tc_extra_positional = TestCase(
    name="cli_extra_positional",
    plan=CompilePlan(
        name="extra_positional",
        sources=[FIXTURES / "hello.c"],
        out=None,
        extra_args=[str(FIXTURES / "hello.cpp")],
    ),
    assertions=[
        assert_exit_code(1),
        assert_stderr_contains("Unexpected positional argument"),
    ],
)

CLI_CASES = [
    tc_help,
    tc_help_short,
    tc_no_args,
    tc_invalid_format,
    tc_unknown_option,
    tc_extra_positional,
]


# ---------------------------------------------------------------------------
# 2. IR compilation tests (LL + BC, C + C++)
# ---------------------------------------------------------------------------
def _success_assertions(fmt: str) -> list[Assertion]:
    return [
        assert_exit_code(0),
        assert_stdout_contains("IR compilation succeeded"),
        assert_stdout_contains(f"format: {fmt}"),
        assert_stdout_contains("payload-bytes:"),
        assert_stdout_contains("defined-functions:"),
    ]


tc_c_ll = TestCase(
    name="compile_c_ll",
    plan=_plan("c_ll", FIXTURES / "hello.c", ["--ir-format=ll"]),
    assertions=_success_assertions("ll"),
)

tc_c_bc = TestCase(
    name="compile_c_bc",
    plan=_plan("c_bc", FIXTURES / "hello.c", ["--ir-format=bc"]),
    assertions=_success_assertions("bc"),
)

tc_c_default_bc = TestCase(
    name="compile_c_default_bc",
    plan=_plan("c_default_bc", FIXTURES / "hello.c"),
    assertions=_success_assertions("bc"),
)

tc_cpp_ll = TestCase(
    name="compile_cpp_ll",
    plan=_plan("cpp_ll", FIXTURES / "hello.cpp", ["--ir-format=ll"]),
    assertions=_success_assertions("ll"),
)

tc_cpp_bc = TestCase(
    name="compile_cpp_bc",
    plan=_plan("cpp_bc", FIXTURES / "hello.cpp", ["--ir-format=bc"]),
    assertions=_success_assertions("bc"),
)

tc_empty_source = TestCase(
    name="compile_empty_source",
    plan=_plan("empty_source", FIXTURES / "empty.c"),
    assertions=[
        assert_exit_code(0),
        assert_stdout_contains("IR compilation succeeded"),
        assert_stdout_contains("defined-functions: 0"),
    ],
)

COMPILE_CASES = [
    tc_c_ll,
    tc_c_bc,
    tc_c_default_bc,
    tc_cpp_ll,
    tc_cpp_bc,
    tc_empty_source,
]


# ---------------------------------------------------------------------------
# 3. Input validation tests
# ---------------------------------------------------------------------------
tc_nonexistent = TestCase(
    name="validate_nonexistent_file",
    plan=_plan("nonexistent", Path("/tmp/_ctrace_nonexistent_xyz.c")),
    assertions=[
        assert_exit_code(1),
    ],
)

tc_directory_input = TestCase(
    name="validate_directory_input",
    plan=_plan("directory_input", Path("/tmp")),
    assertions=[
        assert_exit_code(1),
    ],
)

tc_invalid_source = TestCase(
    name="validate_invalid_source",
    plan=_plan("invalid_source", FIXTURES / "invalid.c"),
    assertions=[
        assert_exit_code(1),
    ],
)

VALIDATE_CASES = [
    tc_nonexistent,
    tc_directory_input,
    tc_invalid_source,
]


# ---------------------------------------------------------------------------
# 4. Concurrency fixture compilation tests
# ---------------------------------------------------------------------------
_EXPECT_COMPILE_FAILURE = {
    "cpp_double_checked_locking",
}


def _concurrency_cases() -> list[TestCase]:
    """Generate a test case for each concurrency fixture (LL + BC)."""
    cases: list[TestCase] = []
    if not CONCURRENCY.exists():
        return cases

    for cpp_file in sorted(CONCURRENCY.rglob("*.cpp")):
        rel = cpp_file.relative_to(CONCURRENCY)
        safe_name = str(rel).replace("/", "_").replace(".cpp", "")
        stem = cpp_file.stem

        if stem in _EXPECT_COMPILE_FAILURE:
            cases.append(
                TestCase(
                    name=f"concurrency_{safe_name}_compile_error",
                    plan=_plan(f"conc_{safe_name}_err", cpp_file, ["--ir-format=ll"]),
                    assertions=[assert_exit_code(1)],
                )
            )
            continue

        cases.append(
            TestCase(
                name=f"concurrency_{safe_name}_ll",
                plan=_plan(f"conc_{safe_name}_ll", cpp_file, ["--ir-format=ll"]),
                assertions=[
                    assert_exit_code(0),
                    assert_stdout_contains("IR compilation succeeded"),
                    assert_stdout_contains("format: ll"),
                ],
            )
        )
        cases.append(
            TestCase(
                name=f"concurrency_{safe_name}_bc",
                plan=_plan(f"conc_{safe_name}_bc", cpp_file, ["--ir-format=bc"]),
                assertions=[
                    assert_exit_code(0),
                    assert_stdout_contains("IR compilation succeeded"),
                    assert_stdout_contains("format: bc"),
                ],
            )
        )

    return cases


CONCURRENCY_CASES = _concurrency_cases()


# ---------------------------------------------------------------------------
# 5. Passthrough and extra compile-arg tests
# ---------------------------------------------------------------------------
tc_compile_arg = TestCase(
    name="arg_compile_arg_define",
    plan=_plan(
        "compile_arg_define",
        FIXTURES / "hello.c",
        ["--compile-arg=-DFOO=1", "--ir-format=ll"],
    ),
    assertions=_success_assertions("ll"),
)

tc_passthrough = TestCase(
    name="arg_passthrough",
    plan=_plan(
        "passthrough",
        FIXTURES / "hello.c",
        ["--ir-format=ll", "--", "-DBAR=1", "-Wall"],
    ),
    assertions=_success_assertions("ll"),
)

tc_instrument = TestCase(
    name="arg_instrument_ll",
    plan=_plan(
        "instrument_ll",
        FIXTURES / "hello.c",
        ["--ir-format=ll", "--instrument"],
    ),
    assertions=[
        assert_exit_code(0),
        assert_stdout_contains("IR compilation succeeded"),
    ],
)

tc_instrument_bc = TestCase(
    name="arg_instrument_bc",
    plan=_plan(
        "instrument_bc",
        FIXTURES / "hello.c",
        ["--ir-format=bc", "--instrument"],
    ),
    assertions=[
        assert_exit_code(0),
        assert_stdout_contains("IR compilation succeeded"),
    ],
)

ARGS_CASES = [
    tc_compile_arg,
    tc_passthrough,
    tc_instrument,
    tc_instrument_bc,
]


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main() -> int:
    if not BIN.exists():
        print(f"ERROR: binary not found: {BIN}", file=sys.stderr)
        print("Build the project first, then re-run tests.", file=sys.stderr)
        return 1

    runner = CompilerRunner(RunnerConfig(executable=BIN, default_timeout_s=30.0))
    reporter = ConsoleReporter()

    suites = [
        TestSuite(name="cli-smoke", cases=CLI_CASES),
        TestSuite(name="ir-compilation", cases=COMPILE_CASES),
        TestSuite(name="input-validation", cases=VALIDATE_CASES),
        TestSuite(name="concurrency-fixtures", cases=CONCURRENCY_CASES),
        TestSuite(name="args-and-passthrough", cases=ARGS_CASES),
    ]

    exit_code = 0
    for suite in suites:
        if not suite.cases:
            print(f"== Suite: {suite.name} == (skipped, no cases)")
            continue
        report = suite.run(runner, root_workspace=WORK)
        rc = reporter.render(report)
        if rc != 0:
            exit_code = 1
        print()

    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
