<div align="center">

# CoreTrace Concurrency Analyzer

**Find data races, deadlocks and thread-lifetime bugs in C and C++ before they find you.**

A static analyzer that compiles your code to LLVM IR and reasons about threads, locks,
`fork` and signal handlers — no instrumentation, no test run, no flaky reproduction.

[![cmake-ctest](https://github.com/CoreTrace/coretrace-concurrency-analyzer/actions/workflows/cmake-ctest.yml/badge.svg)](https://github.com/CoreTrace/coretrace-concurrency-analyzer/actions/workflows/cmake-ctest.yml)
[![Release](https://img.shields.io/github/v/release/CoreTrace/coretrace-concurrency-analyzer?sort=semver)](https://github.com/CoreTrace/coretrace-concurrency-analyzer/releases)
[![License: Apache 2.0](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![LLVM 20](https://img.shields.io/badge/LLVM-20-262D3A?logo=llvm)](https://llvm.org)
[![GitHub Action](https://img.shields.io/badge/GitHub%20Action-ready-2088FF?logo=githubactions&logoColor=white)](docs/github-action.md)

[Quick start](#quick-start) · [Rules](#what-it-catches) · [CI](#in-ci) · [How it works](#how-it-works) · [Docs](#documentation)

</div>

---

## See it in action

```c
int shared_counter = 0;

void* increment(void* arg) {
    for (int i = 0; i < 10000; i++)
        shared_counter++;              // two threads, no lock
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    pthread_create(&t1, NULL, increment, NULL);
    pthread_create(&t2, NULL, increment, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
}
```

```text
$ coretrace_concurrency_analyzer race.c --analyze

Function: increment
	severity: ERROR
	ruleId: DataRaceGlobal
	cwe: CWE-362
	symbol: shared_counter
	[!!!Error] unsynchronized concurrent access to global 'shared_counter'
	     ↳ first access: read at race.c:10:23 in increment (thread entries: increment)
	     ↳ conflicting access: write at race.c:10:23 in increment (thread entries: increment)
	     ↳ possible conflict kinds: read/write, write/write
	     ↳ no common recognized lock protects the conflicting accesses
```

The same report is available as **JSON** and **SARIF**, so it lands directly in GitHub Code Scanning.

## Quick start

**With Docker** — nothing to install but a container runtime:

```bash
docker run --rm -v "$PWD:/work" \
  ghcr.io/coretrace/coretrace-concurrency-analyzer:v0.7.1 file.c --analyze
```

**On a whole project** — reuse the compilation database your build already produces:

```bash
coretrace_concurrency_analyzer --compile-commands=build/compile_commands.json
```

**From source** (LLVM/Clang 20, CMake ≥ 3.21) — see [Building](#building).

## What it catches

| Rule | `--rules=` | Catches |
|---|---|---|
| Data race | `data-race` | shared globals accessed concurrently with no common lock |
| Lock-order deadlock | `deadlock-lock-order` | lock-order inversions and self-deadlock |
| Missing join | `missing-join` | `pthread` / `std::thread` handles never joined nor detached |
| Condition wait | `condition-wait` | a wait that does not recheck its predicate after waking |
| Weak publication | `weak-publication` | data published through an atomic flag without release/acquire |
| Thread arg escapes frame | `thread-arg-escape` | a thread handed a pointer into its creator's stack |
| Thread arg freed early | `thread-arg-freed` | memory freed before the thread using it is joined |
| Thread-local escape | `thread-local-escape` | a thread-local used after its thread has ended |
| Fork after threads | `fork-after-thread` | `fork` in a threaded program with no `exec` in the child |
| Unreaped child | `unreaped-child` | children that are never `wait`ed for |
| Unsafe signal handler | `unsafe-signal-handler` | handlers reaching non async-signal-safe calls |

All rules run by default. Each one documents what it proves and what it deliberately
does not in [docs/rules.md](docs/rules.md).

## In CI

```yaml
permissions:
  contents: read
  security-events: write   # for the Code Scanning upload

steps:
  - uses: actions/checkout@v4
  - uses: CoreTrace/coretrace-concurrency-analyzer@v0
    with:
      compile-commands: build/compile_commands.json
      fail-on: error
```

The action runs the published image (it starts in seconds), uploads SARIF to Code Scanning,
then fails the job if needed. Exit codes separate *“found something”* (`2`) from
*“could not analyze”* (`1`), so a crashed tool never passes for a clean tree.
Full guide: [docs/github-action.md](docs/github-action.md).

## How it works

```text
C / C++ ──clang──▶ LLVM IR (in memory) ──▶ facts ──▶ interprocedural propagation ──▶ rules ──▶ text · JSON · SARIF
                                            (threads, locks,   (thread reachability,
                                             accesses, forks)   lifecycle, held locks)
```

Sources are compiled unoptimized so the analysis sees the program as written. In project mode,
seven kinds of facts cross translation-unit boundaries ([docs/cross-tu-mode.md](docs/cross-tu-mode.md)).

**Known limits** — shared state reached through heap or stack pointers is not tracked yet (only
globals are), and thread entries passed as pointer-to-member are not resolved. Details in
[docs/rules.md](docs/rules.md#limits).

## Use it as a library

```cpp
llvm::LLVMContext context;
ctrace::concurrency::InMemoryIRCompiler compiler;
auto result = compiler.compile({.inputFile = "sample.cpp"}, context);

ctrace::concurrency::SingleTUConcurrencyAnalyzer analyzer;
auto report = analyzer.analyze(*result.module);
```

Embed it with CMake `FetchContent` (tag `v0.7.1`); a complete consumer lives in
[`extern-project/`](extern-project/). API reference: [docs/api.md](docs/api.md).

## Building

```bash
LLVM=/opt/homebrew/opt/llvm@20   # or /usr/lib/llvm-20 on Debian/Ubuntu
cmake -S . -B build \
  -DLLVM_DIR=$LLVM/lib/cmake/llvm \
  -DClang_DIR=$LLVM/lib/cmake/clang \
  -DCLANG_EXECUTABLE=$LLVM/bin/clang \
  -DCLANG_RESOURCE_DIR=$LLVM/lib/clang/20
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Documentation

| | |
|---|---|
| [Rules & limits](docs/rules.md) | what each rule establishes and does not |
| [CLI reference](docs/cli.md) | options, output formats, exit codes |
| [GitHub Action](docs/github-action.md) | workflows, inputs, outputs, false positives |
| [Project mode](docs/cross-tu-mode.md) | whole-program analysis from `compile_commands.json` |
| [Performance](docs/performance.md) | cost on a real 49-unit project |
| [Architecture](docs/architecture.md) | layers, backend, extension points |

## Contributing

Issues and PRs welcome — see [CONTRIBUTING.md](CONTRIBUTING.md). Part of the
**CoreTrace** toolset, alongside
[coretrace-stack-analyzer](https://github.com/CoreTrace/coretrace-stack-analyzer).

Licensed under [Apache 2.0](LICENSE).
