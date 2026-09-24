# Changelog

Notable changes per release. Versions follow [Semantic Versioning](https://semver.org),
derived from the [Conventional Commits](https://www.conventionalcommits.org) this
repository already enforces: a `feat` moves the minor, a `fix` or `perf` moves the
patch, and a `!` or a `BREAKING CHANGE` footer moves the major.

While the version is below 1.0.0, the report format and the public C++ API may
still change between minor releases.

## Unreleased

- A function-local static is no longer reported as racing with itself. The
  language initializes it once, under a guard that every other thread passes or
  waits on, but `data-race` saw the constructor run in every thread and the
  guard's own bookkeeping as plain accesses: every Meyers singleton reached by
  threads was reported twice. `__cxa_guard_acquire` now holds the guard for the
  initialization and `__cxa_guard_release`/`__cxa_guard_abort` release it, and
  what the initialization writes is ordered before every other access to the
  object. What threads do with the object afterwards is still checked. In the
  JSON `functions` array of a function that initializes such a static, the
  guard no longer counts as a shared access and the initialization's accesses
  count as protected.
- New rule `weak-publication` (`WeakPublicationOrdering`, warning, on by
  default): data published through an atomic flag whose store does not
  release, or whose observing load does not acquire, directly or through a
  fence. The reader may then see the flag set and the data stale. It reads the
  same publications `data-race` now treats as ordering, under the same
  conditions, and names the flag, the data and the weak side. A plain payload
  behind a relaxed flag gets both diagnostics: the race on the data and the
  reason the flag does not order it.
- An atomic flag reached through wrappers is no longer reported as a race
  when the wrappers also call a helper that touches no shared memory. Such a
  helper used to make the wrapper's accesses count as plain ones. libstdc++
  builds every `std::atomic<bool>` member this way, so on Linux a correctly
  used atomic flag was reported as a data race.
- `data-race` no longer reports data published through an atomic flag with
  release and acquire ordering: a thread writes the data and stores a constant
  to the flag with a release store (or after a release fence), and the reader
  touches the data only on the branch taken once an acquire load (or a load
  followed by an acquire fence) has seen that constant. The ordering is only
  trusted when observing the constant proves which store was read: the flag
  has that single store, from a thread `main` starts once, outside any loop,
  and the constant differs from the flag's initial value. A relaxed flag, a
  second writer, data written after the store or read on the wrong branch
  still race. Orders are read from C11 atomics and from the libc++ and
  libstdc++ `std::atomic` members, including the ones libstdc++ inlines.

## v0.5.0

- **Default change.** `--analyze` without `--rules`, and a default-constructed
  `AnalysisOptions`, now run every rule: `thread-arg-escape` and
  `unsafe-signal-handler` join the six that ran before. Both report at error
  severity, so a run gated with `--fail-on=error` or `--fail-on=warning` can
  fail where it passed. To keep the previous behaviour, name the six rules:
  `--rules=data-race,missing-join,deadlock-lock-order,condition-wait,fork-after-thread,unreaped-child`.
  The GitHub Action already defaulted to `rules: all` and is unaffected.
  `AnalysisOptions::allAvailable()` now returns the default, so a new rule is
  listed once and reaches both. Measured on the fixtures and on pigz, zstd, this
  analyzer and the 49-unit workload before the change (#53); the analysis costs
  about 2% more on that workload.
- `thread-arg-escape` no longer reports threads that are all joined by a loop
  over the range that created them, the usual `&ids[i]` worker shape. The rule
  used to accept only a join that dominates every return, which a join inside a
  loop never does. It now also reads the join-range proof that `missing-join`
  and the task analysis already share. A range it cannot prove, such as a join
  loop that stops short or a loop counter handed to the threads, is still
  reported.

## v0.4.0

- **Report and public API change.** The JSON `functions` array reports what
  the selected rules computed, as the diagnostics already did. `--rules=all` is
  unchanged; under a selection that reads no shared accesses,
  `sharedAccessCount`, `protectedAccessCount` and `writeAccessCount` are absent
  from an entry instead of rendered as zero, so a count nobody took is not
  mistaken for a count of none. `FunctionSummary` holds them as
  `std::optional<std::size_t>` accordingly. See `README.md`.
- Threads started and joined inside a helper, or across two counted loops, are
  no longer paired with the threads of a later phase. A completion proof
  identifies a point after every instance a spawn site produced has been
  joined, and both the lifecycle collector and the task analysis read it, so
  the two rules agree on what a finished phase is. Detach resolves ownership
  and is not completion; an unknown path, an incomplete range, an overwritten
  handle or a recursive context keeps the threads concurrent, and races
  *within* a phase are retained. Covers the shapes every thread-pool and
  run-in-parallel helper takes, including a fixed-size `std::vector` of
  handles traversed by a full range. No public API or report change.
  `docs/contained-thread-phases.md` states the contract and its limits.
- 27 fixtures contributed in April are now registered as tests, one category
  per import, with their original bytes and authorship. Six of them are pinned
  silent because no rule covers what they exercise; those gaps are recorded
  rather than presented as clean results.
- A unit analysis builds only the facts the selected rules read. `--rules` now
  decides which collectors run, through one table from rule to fact; the
  collectors themselves know nothing about rules. On the documented 49-unit
  workload a `--rules=missing-join` analysis takes 1.9 s instead of 8.6 s and
  peaks at 464 MB instead of 783 MB; `--rules=all` is unchanged. A project
  analysis still builds, for every unit, the facts the whole-program index and
  the second-pass gate read — the entry concurrency, the thread lifecycles and
  the lock wrapper summaries — whatever the rules select.
- A signal handler that calls `strtok` is reported. `strtok` keeps its position
  in hidden state that the scan it interrupted is still using; POSIX lists
  `strtok_r` as async-signal-safe and leaves `strtok` out. One of the six
  fixtures pinned silent now expects the diagnostic.

## v0.3.0

- **Public API change.** `ProjectConcurrencyAnalyzer::analyze` takes a
  `ProjectUnitSource` — the program as bitcode buffers it opens and closes on
  demand — instead of a vector of live `llvm::Module` pointers. Callers that
  compiled units themselves add each unit's bitcode with an identifier; the
  analysis parses a unit when it gets to it and releases it afterwards.
  `ProjectAnalysisReport` gains `failedUnits` (units whose bitcode did not
  parse, with the error), `peakLiveUnits` and `loadMilliseconds`, and
  `AnalysisOptions::maxLiveUnits` bounds how many units are in memory at once.
  `SingleTUConcurrencyAnalyzer` is unchanged.
- Project analysis holds at most `--max-live-units` modules in memory (one per
  hardware thread by default) instead of every module of the project. On the
  documented 49-unit workload peak RSS falls from about 1.0 GB to about 0.75 GB
  at eight live units and about 0.44 GB at one, at the cost of parsing each unit
  inside its worker; `docs/performance.md` records the measurements and
  `docs/cross-tu-mode.md` the model. A unit that fails to parse is named,
  reported as failed and left out, and the run exits non-zero.
- The second cross-TU pass is decided from the facts of the first alone; nothing
  reads a module back once its facts exist. `--verbose` prints `peak-rss-mb`,
  `live-units-max`, `bitcode-mb` and `load-ms`.
- A captureless lambda passed to `pthread_create` is resolved as a thread entry:
  the copy walk now follows a call whose every return names the same function,
  which is what the lambda's conversion operator is. Races inside such workers
  were never reported before.
- An access the analysis cannot resolve is no longer attributed to whichever
  global the module happens to define. The may-alias fallback is kept only when
  the pointer could have come from outside the module; a pointer into a
  container the module built itself cannot reach a global. On a real project
  this removed 25 false `DataRaceGlobal` errors, all on an LLVM ABI guard nothing
  ever touched.
- Each function's dominator tree and loop info are computed once per unit and
  shared by every collector, through the analysis provider that already served
  alias analysis: 756,947 constructions became 71,747 on the 49-unit workload.
  A unit's call sites are likewise resolved once instead of three times, and
  each checker no longer builds the function summaries the caller rebuilt.
- The LLVM installer is downloaded with one bounded retry policy on every CI
  runner and in both container images (`docker/wgetrc`), so a transient
  apt.llvm.org failure is retried and a persistent one fails with the reason in
  the log.

## v0.2.3

- Memoize concurrency-symbol classification by resolved callee within each
  analysis invocation. On the documented 49-unit workload, 94.05% of eligible
  calls reuse the cached result. Fresh Release builds measured 9.2% lower median
  analysis time (5222 ms versus 4740.5 ms), with identical reports. This is a
  local workload measurement, not a universal speedup claim.
- Record the complete benchmark protocol, dependency revisions, per-run timing
  and peak memory, and direct counts of cache hits and unresolved calls in
  `docs/performance.md`.

## v0.2.2

- Analysing a project is roughly six times faster. Identifying a stack slot used
  to render its `alloca` with `printAsOperand`, which builds a module-wide
  `SlotTracker` per call and walks every metadata node in the module; 80% of
  analysis time went there. A slot's position identifies it just as well. On a
  49-unit project: 98.1s before, 15.4s after, with identical results.
- `docs/performance.md` records the method, the scaling and memory figures, the
  complexity of each phase, and the next axes in the order the profile ranks
  them.

## v0.2.1

- The action's examples referenced `@v1`, a tag that does not exist, so anyone
  copying the documentation got "unable to resolve action". They now say `@v0`,
  a moving tag the release workflow updates on every release. It is `v0` rather
  than `v1` deliberately: this project is below 1.0.0 and its report format may
  still change, which a `v1` tag would deny.
- The release workflow triggers on `v*.*.*` instead of `v*`, so the moving tag
  it maintains no longer triggers a release of itself.
- The action accepts an `image` input overriding `version` with a full
  reference, which lets CI point it at an image built from the commit under
  test. Unset, nothing changes.
- The smoke tests build that image from source, so a pull request breaking the
  analyzer or the Dockerfile is caught before a tag rather than after one.
- `docs/github-action.md` covers integrating the action in another project, and
  an issue form collects what makes a wrong finding actionable.

## v0.2.0

Two features, both aimed at running this in CI.

- `--fail-on=none|error|warning` gates the exit code on findings. The analyzer
  used to exit `0` whatever it reported, which made a CI job incapable of
  failing. Findings exit `2`, distinct from the `1` that means the analysis
  could not conclude, so a pipeline cannot mistake a crashed tool for a clean
  tree. The CLI default stays `none`.
- `action.yml` publishes a GitHub Action that runs the released container
  image, so a job starts in seconds instead of building the analyzer. It writes
  SARIF, uploads it to Code Scanning before the gate fails the job, and exposes
  `errors` and `warnings` as outputs. Its gate defaults to `error`: a CI job is
  asked to have an opinion, a command line is not.

The release workflow now also refuses a tag whose version disagrees with the
action's default image, which would otherwise analyse with the previous release
while claiming to be the current one.

## v0.1.1

A packaging release: no analysis behaviour changed. The conventional commits
since v0.1.0 are `ci` and `docs`, which move no version on their own, but the
published artifact differs -- an arm64 machine can now pull the image at all --
and a tag is the only thing that publishes it.

- The published image is built for `linux/amd64` and `linux/arm64`, each on a
  native runner and merged into one manifest list, so a pull resolves without
  `--platform`. Emulating arm64 through QEMU was rejected: this image compiles
  an LLVM-based analyzer, which emulated turns minutes into hours.
- `docker/release-linux.Dockerfile` clones its dependencies over HTTP/1.1.
  Git's HTTP/2 transfer is truncated by some container network stacks, and the
  truncated response is misread as an auth challenge on a public repository.
- `SECURITY.md` states which versions are supported and how to report a
  vulnerability privately.

## v0.1.0

First tagged release. It covers the 216 commits made before tagging began, which
contained 85 `feat`, 23 `fix` and 2 `perf` entries and no breaking change — one
minor step away from nothing.

### Analysis

Single-translation-unit concurrency analysis over the in-memory LLVM IR of C and
C++ sources, with eight rules, each selectable through `--rules=`:

- `data-race` — shared globals reached concurrently under no common lock.
- `missing-join` — joinable thread handles left neither joined nor detached,
  including a thread a callee started and left running for its caller.
- `deadlock-lock-order` — lock-order inversions and self-deadlock.
- `condition-wait` — a condition-variable wait that rechecks nothing when it wakes.
- `fork-after-thread` — a `fork` in a threaded program with no `exec` in the child.
- `unreaped-child` — a `fork` whose children are never collected.
- `thread-arg-escape` — a thread handed a pointer into the frame that created it.
- `unsafe-signal-handler` — a signal handler reaching a call it may not make.

### Cross-translation-unit mode

`--compile-commands=compile_commands.json` analyses a whole project rather than a
single file, resolving symbols across translation units. Compiled IR is cached
between units; `--no-cache` recompiles everything.

### Output

`--format=human` for reading, `--format=json` for consuming, `--format=sarif` for
code-scanning tooling. `--version` reports the release the binary was cut from.

### Distribution

- Source, consumable through CMake `FetchContent` at this tag.
- `ghcr.io/coretrace/coretrace-concurrency-analyzer:v0.1.0`, which carries the
  clang 20 the analyzer needs to compile the sources it is given.

### Reproducibility

The `coretrace-compiler` backend is pinned to an exact commit rather than
tracking `main`, so this tag builds the same way tomorrow as today.
