# Whole-project mode

The analyzer can read a `compile_commands.json` and reason about a program rather than a file:

```
coretrace_concurrency_analyzer --compile-commands=build/compile_commands.json --format=human
```

`--compile-commands` implies `--analyze`. It is exclusive with a positional input file and with
`--compile-arg`, because the database already states which sources exist and how each is built.

## What crosses the unit boundary, and why only that

A translation unit is not a wrong view of the program, it is a partial one. Two facts are
systematically missing from it, and they are the two the project mode carries across:

- **A thread entry spawned elsewhere.** `worker.c` holds the body and never sees a
  `pthread_create` naming it, so on its own it has no reason to believe two threads run it.
  `main.c` holds the spawns and never sees the body.
- **A global defined elsewhere.** A unit alone cannot tell `extern int g;` backed by real storage
  from an unresolved symbol, so it drops both, and every access to a project-wide global
  disappears with them.
- **What a lock helper does to the lock it is handed.** A `void take(pthread_mutex_t*)` defined in
  another unit is opaque here, so an inversion expressed through it is invisible and an access it
  protects looks unguarded.
- **Where a thread handle is joined.** A thread started in one unit and joined in another was
  reported as leaked. This is the one place the project mode *removes* a finding.

Nothing else is shared. In particular, each unit still computes its own lock state, its own
may-happen-in-parallel relation and its own diagnostics: those are answerable locally, and making
them global would cost precision without buying anything.

## Passes

1. **Every unit alone**, in parallel, already in project mode so that a spawn naming an external
   entry is recorded rather than dropped. Nothing is seeded back yet.
2. **The index is folded** from those results. Spawn counts are read from each unit's finished
   facts, not recounted from its calls, because those facts already carry the correction that
   rules out two spawns sitting on mutually exclusive branches.
3. **Only the contradicted units are re-analysed.** A unit is re-run when the program says one of
   its functions is a thread entry it did not know about, when an `extern` it dropped turns out to
   name real storage, when a helper it only sees declared takes a lock for its caller, or when a
   handle it creates is joined elsewhere. On this repository that is 3 units out of 40.

### Symbol identity

Units are joined by `GlobalValue::getGlobalIdentifier`, not by plain name. Two `static int
counter` in different files stay distinct; merging them would invent a conflict between threads
that share nothing. External-linkage symbols keep their bare name, which is exactly the ABI rule
the linker applies.

### Target ABI

Byte offsets and lock identities are computed against a `DataLayout`. Modules whose triple and
layout differ from the majority are named on stderr and left out rather than compared against
addresses that have no common meaning.

## Cost

Measured on this repository (42 units, macOS arm64, 8 cores):

| Phase | Cold | Warm |
| --- | --- | --- |
| Compile | 49.3 s | 8.5 s |
| Analyse | 27.1 s | 27.0 s |
| **Total** | **76.4 s** | **35.5 s** |

Compiling is about half the run and almost none of it is useful work a second time, so compiled
IR is cached under `.coretrace-ir-cache/` beside the database. An entry is keyed on the source,
the flags, and every header the compiler actually opened — read from the dependency list the
compiler writes, not guessed at — so a header edit the source never names directly still
invalidates it. `--no-cache` recompiles everything.

`--verbose` reports the split, how many units the second pass touched, and — separately — how
many units were compiled and how many were reused from the cache. The two are never added
together: a project whose sources no longer build reads as fully healthy for as long as its
cache entries stay valid, which is exactly when the reader most needs to be told otherwise.

```
units: 0 compiled, 15 reused, 0 failed
```

## Memory

Only the bitcode of a project is held for the whole run. The analysis parses a unit when it
gets to it and lets the module go once its facts are extracted, so the number of modules in
memory is bounded by the number of workers — `--max-live-units=N`, one per hardware thread
by default — rather than by the size of the project. A unit the second pass has to revisit
is parsed again from the same bytes, with or without a cache on disk, so it sees exactly the
IR the first pass saw.

The trade is time for memory, and it is explicit: with one live unit the analysis runs
serially; with as many as there are hardware threads it runs as it did before, with the
parsing of each unit moved into the worker that analyses it. `--verbose` prints the ceiling
that was actually reached (`live-units-max`), the bitcode retained (`bitcode-mb`), the time
spent parsing across all workers (`load-ms`) and the peak resident size of the process
(`peak-rss-mb`). Measurements are in `performance.md`.

A unit whose bitcode does not parse is named on stderr with the reason, left out of the
report, and counted as failed; the run then exits non-zero as it does for a unit that did not
compile. A unit that parsed in the first pass and not in the second is treated the same way,
and its first-pass conclusions are dropped rather than reported: they are exactly what the
second pass was about to correct.

## Limits

- **Analysis is not cached.** It is the larger half of a warm run. Per-unit facts are plain
  strings and numbers, so caching them is a matter of choosing a serialised form and an
  invalidation key, not of redesigning them.
- **Bitcode is held in memory for the whole run.** Modules are bounded; the bitcode they are
  parsed from is not, and grows with the project (139 MB for the 49-unit workload in
  `performance.md`). Spilling it to a temporary file would lower that floor without changing
  the API.
- **Compilation is serial.** The clang backend relies on process-wide state; a race inside a race
  detector would be a poor trade for the wall-clock it would save.
- **Lock helper summaries describe one parameter at a time, unconditionally.** A helper that takes
  the lock on one branch only, or that touches the same parameter twice, is left opaque. Crediting
  it with protection it does not always provide would silence a real race, which is the worse
  failure of the two.
- **Handles are matched only through globals.** A handle passed between units by pointer, or held
  in a heap structure, is not matched.
- **The cache is never pruned.** It grows with the number of distinct build configurations; it is
  removed with the build directory it sits in.
