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

Measured on this repository (40 units, macOS arm64, 8 cores):

| Phase | Cold | Warm |
| --- | --- | --- |
| Compile | 32.0 s | 8.0 s |
| Analyse | 24.5 s | 24.8 s |
| **Total** | **56.5 s** | **32.8 s** |

Compiling is about half the run and almost none of it is useful work a second time, so compiled
IR is cached under `.coretrace-ir-cache/` beside the database. An entry is keyed on the source,
the flags, and every header the compiler actually opened — read from the dependency list the
compiler writes, not guessed at — so a header edit the source never names directly still
invalidates it. `--no-cache` recompiles everything.

`--verbose` reports the split, the number of cache hits, and how many units the second pass
touched.

## Limits

- **Analysis is not cached.** It is the larger half of a warm run. Per-unit facts hold pointers
  into the LLVM module they were built from, so caching them means giving them a serialisable
  form first.
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
