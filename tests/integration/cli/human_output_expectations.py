# SPDX-License-Identifier: Apache-2.0
"""Helpers for fixture-embedded human-output golden tests."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
FIXTURES = ROOT / "tests" / "fixtures"

_BIN_CANDIDATES = [
    ROOT / "build" / "coretrace_concurrency_analyzer",
    ROOT / "build-llvm20" / "coretrace_concurrency_analyzer",
    ROOT
    / "extern-project"
    / "build-llvm20"
    / "_deps"
    / "concurrency_analyzer-build"
    / "coretrace_concurrency_analyzer",
]

_ENV_BIN = os.environ.get("CORETRACE_ANALYZER_BIN")
if _ENV_BIN:
    BIN = Path(_ENV_BIN)
else:
    BIN = next((path for path in _BIN_CANDIDATES if path.exists()), _BIN_CANDIDATES[0])

EXPECT_HUMAN_DIAGNOSTICS_BEGIN = "EXPECT-HUMAN-DIAGNOSTICS-BEGIN"
EXPECT_HUMAN_DIAGNOSTICS_END = "EXPECT-HUMAN-DIAGNOSTICS-END"

_BEGIN_RE = re.compile(r"^\s*//\s*" + EXPECT_HUMAN_DIAGNOSTICS_BEGIN + r"\s*$")
_END_RE = re.compile(r"^\s*//\s*" + EXPECT_HUMAN_DIAGNOSTICS_END + r"\s*$")
_COMMENT_RE = re.compile(r"^\s*// ?(.*)$")


def _normalize_newlines(text: str) -> str:
    return text.replace("\r\n", "\n").replace("\r", "\n")


def discover_human_expectation_fixtures(root: Path = FIXTURES) -> list[Path]:
    """Return every C/C++ fixture that embeds a human diagnostics expectation block."""

    fixtures: list[Path] = []
    for path in sorted(root.rglob("*")):
        if path.suffix not in {".c", ".cpp"}:
            continue

        contents = path.read_text(encoding="utf-8")
        if EXPECT_HUMAN_DIAGNOSTICS_BEGIN in contents:
            fixtures.append(path)

    return fixtures


def extract_expected_human_diagnostics(path: Path) -> str:
    """Extract and uncomment the fixture-embedded expected diagnostics block."""

    lines = path.read_text(encoding="utf-8").splitlines()
    begin_index: int | None = None
    end_index: int | None = None

    for index, line in enumerate(lines):
        if _BEGIN_RE.match(line):
            if begin_index is not None:
                raise ValueError(f"multiple {EXPECT_HUMAN_DIAGNOSTICS_BEGIN} blocks in {path}")
            begin_index = index
            continue

        if _END_RE.match(line):
            if end_index is not None:
                raise ValueError(f"multiple {EXPECT_HUMAN_DIAGNOSTICS_END} blocks in {path}")
            end_index = index

    if begin_index is None or end_index is None or end_index < begin_index:
        raise ValueError(f"incomplete expectation block in {path}")

    body_lines = lines[begin_index + 1 : end_index]
    uncommented: list[str] = []
    for offset, line in enumerate(body_lines, start=begin_index + 2):
        if not line.strip():
            uncommented.append("")
            continue

        match = _COMMENT_RE.match(line)
        if match is None:
            raise ValueError(f"expected comment line inside expectation block at {path}:{offset}")

        uncommented.append(match.group(1))

    return "\n".join(uncommented).rstrip("\n")


def extract_actual_human_diagnostics(output: str) -> str:
    """Return only the diagnostics section from the human renderer output."""

    normalized = _normalize_newlines(output)

    mode_index = normalized.find("Mode: ")
    if mode_index == -1:
        raise ValueError("human report is missing the 'Mode:' header")

    report = normalized[mode_index:]
    body_start = report.find("\n\n")
    if body_start == -1:
        raise ValueError("human report is missing the blank line after the mode header")

    summary_marker = "\n\nDiagnostics summary:"
    body_end = report.rfind(summary_marker)
    if body_end == -1:
        raise ValueError("human report is missing the diagnostics summary footer")

    body = report[body_start + 2 : body_end]
    return body.rstrip("\n")


def normalize_human_output(text: str, repo_root: Path = ROOT) -> str:
    """Normalize machine-specific bits without changing semantic output content."""

    normalized = _normalize_newlines(text)

    repo_root_text = str(repo_root.resolve())
    repo_root_posix = repo_root.resolve().as_posix()
    for candidate in {repo_root_text, repo_root_posix}:
        normalized = normalized.replace(candidate, "${REPO_ROOT}")

    return normalized.rstrip("\n")


def run_human_analyzer(fixture_path: Path, binary: Path = BIN) -> subprocess.CompletedProcess[str]:
    """Run the analyzer in human mode for a single fixture."""

    return subprocess.run(
        [str(binary), str(fixture_path), "--analyze", "--format=human"],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
