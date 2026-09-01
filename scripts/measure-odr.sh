#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Measures how often a function is defined in more than one translation unit,
# and whether those duplicates are weak (ODR: inline functions, templates) or
# strong (genuinely ambiguous).
#
# The cross-TU design excludes multi-defined functions from the inter-module
# call graph, assuming they are ODR noise. This says whether that holds, and at
# what cost: see docs/cross-tu-phase-0.md.
#
# Requires a completed build. Reads object files, so it costs no compilation.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_ROOT}/build}"

if [ ! -d "${BUILD_DIR}" ]; then
    echo "usage: $(basename "$0") [build-dir]" >&2
    exit 2
fi

python3 - "${BUILD_DIR}" <<'PYTHON'
import collections, pathlib, re, subprocess, sys

build = pathlib.Path(sys.argv[1])
objects = [p for p in build.rglob("*.o")]
if not objects:
    sys.exit(f"no object files under {build}; build first")

# Apple's nm reports weak definitions; llvm-nm shows them as strong on Mach-O.
nm = "nm" if sys.platform == "darwin" else "llvm-nm"
darwin = sys.platform == "darwin"

# Objects of different link targets are never linked together, so the same
# source compiled into a library and into a test is not a duplicate.
by_target = collections.defaultdict(list)
for obj in objects:
    match = re.search(r"CMakeFiles/([^/]+)\.dir/", str(obj))
    by_target[match.group(1) if match else obj.parent.name].append(obj)

for target, files in sorted(by_target.items()):
    if len(files) < 2:
        continue

    symbols = collections.defaultdict(lambda: {"weak": False, "objects": set()})
    for obj in files:
        args = [nm, "-m", str(obj)] if darwin else [nm, "--defined-only", str(obj)]
        for line in subprocess.run(args, capture_output=True, text=True).stdout.splitlines():
            if darwin:
                if "(undefined)" in line or "non-external" in line:
                    continue
                if not re.search(r"\bexternal\b", line):
                    continue
                weak = bool(re.search(r"\bweak\b", line))
            else:
                fields = line.split()
                if len(fields) < 3 or not fields[-2].isupper():
                    continue
                weak = fields[-2] in ("W", "V")
            name = line.split()[-1]
            symbols[name]["objects"].add(obj.name)
            symbols[name]["weak"] = symbols[name]["weak"] or weak

    multi = {k: v for k, v in symbols.items() if len(v["objects"]) > 1}
    strong = {k: v for k, v in multi.items() if not v["weak"]}
    share = 100 * len(multi) // max(len(symbols), 1)

    print(f"{target}")
    print(f"  translation units      {len(files)}")
    print(f"  external definitions   {len(symbols)}")
    print(f"  defined in >1 TU       {len(multi)} ({share} %)")
    print(f"    weak / ODR           {len(multi) - len(strong)}")
    print(f"    strong (ambiguous)   {len(strong)}")
    for name in list(strong)[:5]:
        print(f"      ! {name[:72]}")
    print()
PYTHON
