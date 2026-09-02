# Cross-TU mode: analyze whole projects from a compilation database

## Goal

Accept a `compile_commands.json` and report concurrency defects across the
whole project, not one translation unit at a time. Today the public entry point
is `SingleTUConcurrencyAnalyzer`, and every fact stops at the file boundary: a
worker defined in `worker.c` and spawned from `main.c` is invisible, and an
`extern` global is dropped outright.

## Approach: exchange summaries, do not link

Each translation unit keeps its own `LLVMContext` and `llvm::Module`. Compact
per-function summaries are extracted, merged into indices keyed by symbol, and
injected read-only into each module's analysis. This matches the design already
shipped in `coretrace-stack-analyzer`, and the consistency matters: two
divergent cross-TU models in one product family would mean two caching
strategies and two sets of bugs.

`llvm-link` was considered and rejected. It would reuse the whole analysis core
unchanged and resolve symbols for free, but it forces whole-program memory
cost, discards each unit's own compile flags, and requires resolving linkage
conflicts before any analysis. It remains the better answer only for an
analysis needing full bodies and a unified call graph, which this is not.

The cost of the summary approach is explicit: **only effects a summary format
was designed for cross the boundary.** Everything else stays local.

## What must cross, and why this is harder than the stack analyzer

The stack analyzer's three cross-TU domains are independent. A single data race
diagnostic needs four facts **simultaneously correct**:

| Domain | Why it must cross |
|---|---|
| Thread entry reachability | `pthread_create` in one TU naming a worker defined in another |
| Lock effects per function | a helper acquiring the mutex passed to it, defined elsewhere |
| Accesses to globals | region, atomicity and held locks on a shared `extern` |
| Handle lifecycle | `create` in one file, `join` in another |

Getting one wrong does not degrade one domain; it produces a false positive or
a false negative on the whole diagnostic. Expect phase 2 to cost more than the
sum of its parts, and the first bugs to be interactions rather than isolated
domains.

Because there are four domains rather than three, extracting a generic
`InterTUCoordinator` is a starting condition, not a later cleanup.

## What LLVM already provides

Three of the hard problems have existing answers, all used in production by
ThinLTO and PGO.

**Symbol identity.** `GlobalValue::getGlobalIdentifier()` qualifies
local-linkage symbols with their module and `getGUID()` hashes that. Joining
globals by raw name would merge two unrelated `static int counter` into one
object — for a race detector that is a false positive, not a lost annotation.

**The inter-module call graph.** `buildModuleSummaryIndex()` produces a summary
index carrying call edges, recorded `linkage()`, `notEligibleToImport`, and
alias handling. `ModuleSummaryIndex` exposes `GraphTraits`, so
`llvm::scc_iterator` replaces a hand-written Tarjan.

**Scheduling and serialization.** `llvm::DefaultThreadPool` with
`ThreadPoolStrategy` replaces a hand-rolled scheduler, and `writeIndexToFile`
is the model for a bitcode-serialized cache.

## Design decision: keep ODR duplicates in the graph

Measured on this repository, library target, 29 translation units and 10 836
external definitions (`scripts/measure-odr.sh`, findings in
`docs/cross-tu-phase-0.md`):

| | |
|---|---:|
| defined in more than one TU | 4 095 (**37 %**) |
| of those, weak / ODR | 4 095 |
| of those, strong (ambiguous) | **0** |

Excluding multi-defined functions from the call graph — as the reference design
does — would discard roughly a third of it. Any worker reached through an
inline header function would leave the thread context.

Check the linkage instead: a `linkonce_odr` or `weak_odr` definition is
identical in every TU by construction, so picking one is safe. Only a strong
duplicate is ambiguous, and there are none.

## Phases

### Phase 0 — done

Measured the ODR assumption (above). Removed the one piece of existing analysis
that could not survive module merging: approximations were weighed by whether
their location fell under the translation unit's source directory, and debug
info records a project header and a system header identically, both with an
empty directory. They are now weighed by evidence instead — reported when they
are the only thing describing a symbol, dropped once an observed conflict on
that symbol exists.

### Phase 1 — skeleton, proven end to end on one domain

Compilation database ingestion, parallel per-module loading with one
`LLVMContext` each, inter-module graph via `buildModuleSummaryIndex`,
`InterTUCoordinator` extracted from the start, and the first domain: **thread
entry reachability**. Nothing else has meaning without it.

Two guards land here, where a consumer finally exists for them:

- symbol identity through `getGUID()`, for functions and globals alike;
- an ABI gate refusing or flagging a merge across differing `DataLayout` or
  target triple, since `MemoryRegion` compares byte offsets.

*Acceptance:* a `pthread_create` in `main.c` naming a worker in `worker.c`,
with the race between them reported.

### Phase 2 — the remaining domains, in dependency order

**Lock effects per function** first. This is the parameterized lock summary
that does not exist even locally today: `canonicalLockId` requires reaching a
`GlobalVariable`, so a mutex passed by pointer is ignored. One piece of work
closes three known single-TU false negatives (non-global locks, locks taken in
a wrapper) and supplies the unit of cross-TU composition.

Then **accesses to globals**, then **handle lifecycle**, whose `arg:` groups
already cross naturally.

### Phase 3 — performance

Compilation of the N units dominates, not analysis. In order of impact: an IR
cache keyed on content rather than `(size, mtime)` and including the binary
version; **parallel final analysis**, which the reference design leaves serial
although separate contexts and immutable indices allow it; and a summary cache
keyed on GUID and module content hash, covering all four domains rather than
one.

A benchmark on a real project belongs in phase 1, not phase 3. Without it
"performant" is not measurable.

## Non-goals

No indirect or virtual calls across TU boundaries, no aliases or ifuncs, no LTO
or inlining effects, and nothing crosses outside the four designed domains.
This discipline is what makes the rest tractable.

## Testing

`scripts/ci-linux.sh` reproduces the Linux CI job and `scripts/ci-matrix.sh`
runs it across architectures; both matter here because everything above depends
on how a standard library lowers its own templates.

The fixture expectation table in `tests/unit/test_concurrency_analysis.cpp`
pins an exact diagnostic count per rule for all 67 fixtures and fails on any
fixture it does not list. Module merging changes what the analyzer sees on
*every* existing fixture, so that table is the instrument that will say what
moved. It needs a multi-file counterpart: fixtures made of several sources plus
their compile commands, with the same per-rule counts.

## Risks

The single measurement that would change the plan is whether the `single-def`
graph carries enough thread reachability on a real project. Phase 0 says the
ODR assumption is sound but that exclusion is too blunt; phase 1 should measure
how many thread entries are reached only through inline header functions before
the design is frozen.

`CompilationDatabase`, `InputPipeline`, `RunPlanBuilder`, the parallel
scheduler and the cache design would all be rewritten identically from
`coretrace-stack-analyzer`. Whether they move into a shared library or are
duplicated deliberately is worth deciding before phase 1, not after.
