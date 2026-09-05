# Changelog

Notable changes per release. Versions follow [Semantic Versioning](https://semver.org),
derived from the [Conventional Commits](https://www.conventionalcommits.org) this
repository already enforces: a `feat` moves the minor, a `fix` or `perf` moves the
patch, and a `!` or a `BREAKING CHANGE` footer moves the major.

While the version is below 1.0.0, the report format and the public C++ API may
still change between minor releases.

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
