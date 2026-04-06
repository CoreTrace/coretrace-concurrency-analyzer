#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Golden tests for fixture-embedded human diagnostic expectations."""

from __future__ import annotations

import difflib
import sys
from pathlib import Path

import pytest


HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

from human_output_expectations import (  # noqa: E402
    BIN,
    FIXTURES,
    ROOT,
    discover_human_expectation_fixtures,
    extract_actual_human_diagnostics,
    extract_expected_human_diagnostics,
    normalize_human_output,
    run_human_analyzer,
)


EXPECTED_FIXTURES = discover_human_expectation_fixtures()


def _fixture_id(path: Path) -> str:
    return path.relative_to(FIXTURES).as_posix()


def test_human_output_fixtures_are_configured() -> None:
    assert EXPECTED_FIXTURES, "no fixtures define EXPECT-HUMAN-DIAGNOSTICS blocks"


@pytest.mark.skipif(not BIN.exists(), reason=f"analyzer binary not found: {BIN}")
@pytest.mark.parametrize("fixture_path", EXPECTED_FIXTURES, ids=_fixture_id)
def test_human_output_matches_fixture_expectation(fixture_path: Path, tmp_path: Path) -> None:
    expected = normalize_human_output(extract_expected_human_diagnostics(fixture_path), ROOT)

    result = run_human_analyzer(fixture_path)
    if result.returncode not in {0, 1}:
        pytest.fail(
            f"unexpected analyzer exit code {result.returncode} for {fixture_path}\n"
            f"stderr:\n{result.stderr}"
        )

    actual = normalize_human_output(extract_actual_human_diagnostics(result.stdout), ROOT)
    if actual == expected:
        return

    actual_path = tmp_path / "actual-human-diagnostics.txt"
    stderr_path = tmp_path / "stderr.txt"
    actual_path.write_text(actual + "\n", encoding="utf-8")
    stderr_path.write_text(result.stderr, encoding="utf-8")

    diff = "\n".join(
        difflib.unified_diff(
            expected.splitlines(),
            actual.splitlines(),
            fromfile=f"{fixture_path} (expected)",
            tofile=f"{fixture_path} (actual)",
            lineterm="",
        )
    )

    pytest.fail(
        f"human diagnostics mismatch for {fixture_path}\n"
        f"{diff}\n\n"
        f"actual diagnostics: {actual_path}\n"
        f"stderr: {stderr_path}"
    )
