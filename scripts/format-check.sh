#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
CLANG_FORMAT_BIN="${CLANG_FORMAT:-clang-format}"

if ! command -v "${CLANG_FORMAT_BIN}" >/dev/null 2>&1; then
    echo "ERROR: clang-format binary not found: ${CLANG_FORMAT_BIN}" >&2
    exit 1
fi

files=()
while IFS= read -r -d '' file; do
    files+=("$file")
done < <(find "${REPO_ROOT}" \
    \( -path "${REPO_ROOT}/build" \
       -o -path "${REPO_ROOT}/build-llvm20" \
       -o -path "${REPO_ROOT}/build-coverage" \
       -o -path "${REPO_ROOT}/extern-project" \
       -o -path "${REPO_ROOT}/external" \
       -o -path "${REPO_ROOT}/third_party" \
       -o -path "${REPO_ROOT}/vendor" \
       -o -path "${REPO_ROOT}/.git" \
       -o -path "${REPO_ROOT}/tests/fixtures" \
       -o -path "${REPO_ROOT}/tests/integration/cli/.work" \) -prune -o \
    -type f \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \) -print0)

if [ "${#files[@]}" -eq 0 ]; then
    echo "No source files to check."
    exit 0
fi

echo "Checking formatting on ${#files[@]} files..."
failed=0
for file in "${files[@]}"; do
    if ! "${CLANG_FORMAT_BIN}" --dry-run --Werror "${file}"; then
        failed=1
    fi
done

if [ "${failed}" -ne 0 ]; then
    echo "Formatting check failed. Run scripts/format.sh to fix."
    exit 1
fi

echo "Formatting is clean."
