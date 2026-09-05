# Performance

Measured, not estimated. Every number below comes from a run recorded on the
machine described at the end, and the method is stated so the figures can be
disputed or reproduced.

## Method

The workload is [`coretrace-stack-analyzer`](https://github.com/CoreTrace/coretrace-stack-analyzer)
analyzed in cross-translation-unit mode: 49 units of real C++ that actually uses
threads. Fixtures are too small to show anything.

Timings come from the analyzer's own `--verbose` output, which separates
`compile-ms` from `analysis-ms`. The IR cache is warm in every measurement, so
`analysis-ms` is analysis alone. Profiles are 20-second samples attributed to
self time. Memory is peak RSS from `/usr/bin/time -l`.

## Where the time goes

Compilation is not the cost. On the 49-unit project, compiling every unit to IR
takes **11.5 s**; analysing them took **98 s** before the change described below,
and takes **15.4 s** after it.

The first profile was unambiguous:

| Self samples | Symbol |
| --- | --- |
| 12 114 | `llvm::SlotTracker::CreateMetadataSlot` |
| 2 553 | `ConcurrencySymbolClassifier::classify` |
| 1 752 | `DenseMap` insertion |

**80 % of analysis time was spent numbering metadata nodes.** That machinery
belongs to printing IR, and the analysis prints nothing. It was reached from a
single line: a stack slot's identity was built by rendering the `alloca` with
`printAsOperand`, and `printAsOperand` without a `ModuleSlotTracker` builds one
per call — walking every metadata node in the module, for every stack slot
examined.

An unnamed `alloca` needs an identifier that distinguishes it from other slots
in the same function and stays the same across runs. Its position does that, at
the cost of one walk of the function instead of one walk of the module:

| | 49 units |
| --- | --- |
| Before | 98.1 s / 99.2 s |
| After | 15.4 s / 15.5 s |
| | **6.4× faster**, identical results, 7/7 tests passing |

## Classifier memoization: follow-up measurement

Measured on 2026-09-05, comparing `24a03da` (stack-slot change, no memo) with
`824a2b1` (callee memo). Both were freshly built with CMake Release
(`-O3 -DNDEBUG`), AppleClang, and LLVM 20.1.2 on an eight-core Mac15,13 running
Darwin 24.6.0. These are a separate comparison from the historical timings
above; the absolute times and speedup ratios should not be combined.

The workload was `coretrace-stack-analyzer` at
`c91b88187f6544a9a80c7a071aa5768ba4c02729`: 84 compilation database entries,
of which 49 project units remain after the analyzer excludes dependencies.
Both builds used the same clean dependency checkouts: `coretrace-compiler` at
`866fa76403f29e4fefda99770e04d284175f4408` and `coretrace-logger` at
`624688ad5e5d00a1d04fd72909d43fe6d948575b`. Source and cached-bitcode SHA-256
hashes were unchanged throughout the measurements.

After one unmeasured warm-up per binary, the measured order was
**A B B A B A A B**, where A is the baseline and B has the memo. All builds
and tests finished before timing began. Background system activity remained;
the one-minute load average ranged from 3.66 to 6.47, so this is an observed
local improvement, not an idle-machine or universal performance claim.

| | Without memo | With memo |
| --- | --- | --- |
| Analysis, four runs (ms) | 5094 / 5171 / 5273 / 5292 | 4746 / 4786 / 4735 / 4733 |
| Median analysis (ms) | 5222 | 4740.5 |
| Peak RSS, each run (MB, decimal) | 898.580 / 1150.075 / 1087.685 / 1007.747 | 1128.628 / 925.254 / 1154.826 / 1115.865 |

The median analysis time is **9.2% lower (1.10×)**. The highest observed RSS
increases by 4.75 MB; individual RSS readings vary substantially, so this does
not establish a precise memory overhead. Every measured run reused all 49 IR
units, compiled none, and reported no failures. All JSON report fields matched
after excluding only `meta.analysisTimeMs`; both binaries passed 7/7 CTest tests.
The workload produced no diagnostics, so the existing regression suite remains
the evidence for preserving positive findings.

A separate instrumented run counted all paths directly, using atomic counters
because different classifier instances run concurrently. Its timing is excluded:

| Counter | Executions |
| --- | --- |
| All `classify()` calls | 2,472,670 |
| Unresolved callees, returned before cache lookup | 4,194 |
| Calls eligible for memoization | 2,468,476 |
| Cache hits | 2,321,499 |
| Cascade executions / cache misses | 146,977 |

The previously reported 2,468,476 calls count only resolved callees. The cache
answers 94.05% of those calls; unresolved calls are not cache hits. Instrumented
and uninstrumented reports match with the same timing-field exclusion.

To reproduce, create separate worktrees at the two analyzer revisions above.
Set `SOURCE`, `BUILD`, and `DB` for each build and the same frozen compilation
database; set `CC_SOURCE` and `LOGGER_SOURCE` to the dependency checkouts above.
Configure both builds identically (adjust LLVM paths for the host):

```sh
cmake -S "$SOURCE" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" \
  -DClang_DIR="$LLVM_PREFIX/lib/cmake/clang" \
  -DCLANG_EXECUTABLE="$LLVM_PREFIX/bin/clang" \
  -DFETCHCONTENT_SOURCE_DIR_CC="$CC_SOURCE" \
  -DFETCHCONTENT_SOURCE_DIR_CORETRACE_LOGGER="$LOGGER_SOURCE"
cmake --build "$BUILD" --parallel 4
ctest --test-dir "$BUILD" --output-on-failure
```

Finish both builds and tests, warm each binary once, then run the alternating
order above, retaining stdout and stderr separately for every run:

```sh
/usr/bin/time -l "$BUILD/coretrace_concurrency_analyzer" \
  --compile-commands="$DB" --verbose --fail-on=none --format=json \
  > "$RUN.json" 2> "$RUN.log"
```

`time -l` reports RSS in bytes on macOS; on Linux use `/usr/bin/time -v` and
convert its maximum RSS from KiB. Compare `analysis-ms`, not total wall time,
and check `0 compiled, 49 reused, 0 failed` before accepting a timed run.

## Scaling

After that change, with the cross-TU thread pool active:

| Units | Analysis | Peak RSS |
| --- | --- | --- |
| 6 | 5.7 s | 332 MB |
| 12 | 9.5 s | 437 MB |
| 25 | 10.8 s | 598 MB |
| 49 | 15.4 s | 1169 MB |

Wall-clock growth is sub-linear because units are analyzed in parallel; the
marginal cost is roughly **225 ms and 20 MB per unit** over this range. The
memory figure is the one to watch: a 49-unit project peaks above **1 GB**, since
every unit's `llvm::Module` is held while the program-wide index is built. A
project several times larger will need headroom a small CI runner may not have.

## Complexity

**Fact building** — `TUFactsBuilder::build`, which runs for every unit whatever
`--rules` selects, since the rules only consume its output. It makes several
independent passes over every function and instruction, so it is O(F·I) in the
size of the module, with a large constant: **12 distinct sites construct their
own `llvm::DominatorTree` per function**, each O(V+E) with LLVM's own allocation
cost. Nothing shares them.

**Symbol classification** — the cascade of ~31 predicates and canonical-name
construction now run once per distinct resolved callee per classifier instance.
A cache keyed on `const llvm::Function*` gives subsequent calls an expected
O(1) lookup. For C calls and U distinct callees, expected work is
O(C + U·P·L), with O(U) cache space, where P counts predicates and L their
string-comparison cost. The cache belongs to the local analysis invocation:
it is not shared across worker threads or retained after its module is destroyed.
The predicates still use string comparisons on cache misses; compiler
optimization determines whether literal-length calculations remain at runtime.

**Data race detection** — accesses are grouped by symbol, then compared
pairwise: for each symbol with k accesses, O(k²) pairs. Total is Σkᵢ², which is
fine when accesses spread across many symbols and quadratic when one symbol is
hot. The body is cheap for most pairs — kind, region overlap and atomicity are
checked before the expensive predicates — and `mayHappenInParallel` is a hash
lookup into a precomputed reachability map rather than a graph walk.

**Space** — dominated by the `llvm::Module`s held simultaneously in cross-TU
mode, plus the per-unit facts: accesses, lock states, thread-entry reachability
sets and shared-object bindings, each proportional to the unit's size.

## What to do next

Ranked by measured weight, not by guess.

1. **Memoise symbol classification — completed.** The measurement above records
   its effect. Profile the remaining cache misses before choosing a separate
   `StringSwitch` or `constexpr std::string_view` table change.
2. **Share the dominator trees.** Twelve sites build their own per function.
   Building each once and passing it through the facts would remove eleven
   redundant constructions per function.
3. **Bound memory in cross-TU mode.** Peak RSS above 1 GB at 49 units is the
   figure most likely to stop a large project outright, and it is structural:
   modules are all live at once. Releasing a unit's module once its facts are
   extracted would trade a re-parse for a much lower ceiling.
4. **Reconsider unconditional fact building.** `--rules=missing-join` pays for
   every fact the other seven rules need. Building lazily would make narrow runs
   proportional to what they ask for.

## Machine

Apple M-series, macOS 15 (Darwin 24.6), LLVM 20.1.2, Release build. Absolute
figures will differ elsewhere; the ratios are what the argument rests on.
