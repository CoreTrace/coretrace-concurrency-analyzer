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

**Symbol classification** — `classify()` is a cascade of ~31 predicates tried in
order, each comparing the callee name against `const char*` literals. Every
comparison converts a literal to `StringRef`, so each call costs a `strlen` per
literal tried, and `canonicalName` allocates a `std::string` per call. It is
O(P·L) per call site where P is the number of predicates and L the literal
lengths — with no memoisation across call sites that share a callee.

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

1. **Memoise symbol classification.** It is now the largest self-time consumer,
   and its whole cascade depends only on the callee. A map keyed on
   `llvm::Function*` would collapse repeated work across call sites; a
   `StringSwitch` or a `constexpr std::string_view` table would remove the
   repeated `strlen` within one classification.
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
