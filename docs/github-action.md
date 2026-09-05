# Using the analyzer in another project's CI

The action runs the published container image, so a consuming repository needs
nothing installed: no LLVM, no build, no toolchain of its own.

## A single-file check

The smallest useful workflow. Copy it whole — the fragment in the README omits
the surrounding `on:` and `jobs:`, which is where most of the mistakes are.

```yaml
name: concurrency

on:
  push:
    branches: [main]
  pull_request:

jobs:
  analyze:
    runs-on: ubuntu-24.04
    permissions:
      contents: read
      security-events: write   # only while upload-sarif is true
    steps:
      - uses: actions/checkout@v4
      - uses: CoreTrace/coretrace-concurrency-analyzer@v0
        with:
          sources: src/worker.c src/pool.c
          fail-on: error
```

Findings appear under the repository's **Security → Code scanning** tab, and the
job fails when one reaches `error`.

## Which reference to use

`@v0` follows releases: it is moved to each new one by the release workflow, so
a fix reaches you without editing anything. `@v0.2.2` pins one exact release and
never changes.

It is `v0` rather than the `v1` the ecosystem usually offers because this
project is below 1.0.0 and says so: the report format may still change between
minor releases. A `v1` tag would promise a stability that has not been declared.
When 1.0.0 arrives, `@v1` starts being maintained the same way.

## A whole project

Naming files by hand stops scaling quickly, and it also blinds the analysis:
rules like `missing-join` follow a thread across translation units, so a file
analyzed alone yields less than the same file analyzed in its project. Pass a
compilation database instead.

**It has to be generated on the runner.** A `compile_commands.json` records
absolute paths and the toolchain that produced it; one committed from a
developer's machine names an SDK the Linux container does not have, and every
unit fails to compile. Generating it in the job is one step:

```yaml
      - name: Generate a compilation database
        run: cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

      - uses: CoreTrace/coretrace-concurrency-analyzer@v0
        with:
          compile-commands: build/compile_commands.json
          fail-on: error
```

For a project that is not CMake-based, [Bear](https://github.com/rizsotto/Bear)
produces the same file from any build: `bear -- make`.

Note that configuring the project must succeed on the runner, which means its
own build dependencies have to be installed first. That cost belongs to the
consuming project, not to this action — a project that needs LLVM to configure
still needs LLVM installed before this step, even though the analyzer itself
brings its own.

## Choosing what is fatal

`fail-on` decides, and defaults to `error`:

| Value | The job fails on |
| --- | --- |
| `none` | nothing — report only |
| `error` | errors |
| `warning` | errors and warnings |

Adopting the analyzer on an existing codebase usually goes better in two steps:
start at `none` so the alerts land in Code scanning without blocking anyone,
read them, and tighten to `error` once the backlog is triaged.

`rules` narrows what runs, which is also the coarse escape hatch for a rule that
does not suit the codebase:

```yaml
          rules: data-race,deadlock-lock-order
```

## Reading the results yourself

`errors` and `warnings` are exposed as outputs — but **they are empty whenever
the gate fails the job**, because GitHub does not evaluate a composite action's
outputs when it fails. That is precisely the run where findings exist. To act on
the counts, either read the SARIF the action wrote, or take the gate into your
own hands:

```yaml
      - uses: CoreTrace/coretrace-concurrency-analyzer@v0
        id: scan
        with:
          sources: src/worker.c
          fail-on: none          # never fails; outputs always populated
      - run: |
          echo "${{ steps.scan.outputs.errors }} error(s)"
          test "${{ steps.scan.outputs.errors }}" -eq 0
```

## When a finding is wrong

The analyzer reasons about what a program *could* do, so it will sometimes
report something a human can see is impossible. There is no suppression comment
and no baseline file; what exists is:

1. **Dismiss the single alert.** In Code scanning, open it and choose *Dismiss →
   False positive*. It stays dismissed across runs — the SARIF carries
   `partialFingerprints`, so the alert is tracked rather than recreated on every
   commit.
2. **Drop the rule**, if a whole rule misfits, with `rules:` above. Blunt: it
   also discards that rule's true findings.
3. **Report it**, so the next release does better.

### Reporting one

Open a [false positive
report](https://github.com/CoreTrace/coretrace-concurrency-analyzer/issues/new?template=false-positive.yml).
The form asks for what makes a report actionable, and the first item is the one
that matters:

- **A source file that reproduces it**, as small as you can make it. A rule is
  fixed by writing a test that fails on the old build and passes on the new one,
  and that test is your snippet. Without it a report is a description of a
  belief.
- The **rule id** (`DataRaceGlobal`, `MissingJoin`, …), shown on the alert and in
  the SARIF.
- The **version**: `docker run --rm ghcr.io/coretrace/coretrace-concurrency-analyzer:v0.2.2 --version`.
- Whether it was a single file or a compilation database, since the two paths
  reason differently.
- **Why it cannot happen.** This is the part only you can supply — the lock that
  is always held, the ordering the analyzer cannot see, the branch that is
  unreachable.

Findings that are *missed* rather than wrong are just as useful to hear about,
and the same form covers them.
