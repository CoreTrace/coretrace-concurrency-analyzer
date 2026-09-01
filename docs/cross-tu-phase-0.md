# Cross-TU groundwork — phase 0 findings

Status: measurement and de-risking only. No cross-TU mode is implemented.

## Why this phase exists

The cross-TU design exchanges per-function summaries between translation units
rather than linking them. Three of its assumptions decide whether that works,
and all three were cheaper to measure than to argue about.

## Finding 1 — the ODR assumption holds, but exclusion is the wrong response

The design excludes functions defined in more than one module from the
inter-module call graph, assuming they are ODR noise. Measured on this
repository's own library target, 29 translation units, 10 836 external
definitions:

| | |
|---|---:|
| defined in more than one TU | 4 103 (37 %) |
| of those, weak / ODR | 4 103 |
| of those, strong (assumption violated) | 0 |

The assumption is sound: there are no ambiguous duplicates. But 37 % is not
noise at the margin. Excluding those functions removes roughly a third of the
call graph, and for concurrency that means any worker reached through an inline
header function leaves the thread context.

The response should be to **check the linkage** rather than to exclude: a
`linkonce_odr` or `weak_odr` definition is identical in every TU by
construction, so picking one is safe. Only a strong duplicate is ambiguous, and
there are none. `buildModuleSummaryIndex` already records `linkage()` and
`notEligibleToImport`, where the current approach only counts definitions.

Reproduce with `scripts/measure-odr.sh` after a build.

### Measuring this correctly is harder than it looks

Three earlier attempts gave the opposite answer:

- `llvm-nm` reports `T` (strong) for weak definitions on Mach-O. Use Apple's
  `nm -m`, or measure on ELF. This alone reported "4 687 strong duplicates".
- Undefined symbols count as definitions unless filtered: `__Unwind_Resume`
  appeared "defined" in 34 objects.
- Objects from different link targets must not be pooled. The same source
  compiled into a library and into a test yields the same strong symbol twice
  without ever being linked together.
- `"non-external"` contains `"external"`, so a naive substring filter keeps
  local labels such as `GCC_except_table*`.

## Finding 2 — debug info cannot separate project headers from system headers

The data race checker used to weigh approximations by whether their location
fell under the translation unit's source directory. Measured on both platforms,
a project header and a standard library header are recorded identically:

```
!DIFile(filename: "sub/helper.hpp",                directory: "")   # project
!DIFile(filename: "/usr/include/c++/14/bits/...",  directory: "")   # system
```

The compile unit's `directory` is the compiler's working directory, so it moves
with wherever the analyzer is run. There is no portable signal here.

The rule was replaced by one that does not look at paths: an approximation —
a coarse call effect, or an alias-guessed identity — is reported when it is the
only evidence about a symbol, and dropped once an observed conflict on the same
symbol exists. This removes the multi-TU hazard rather than patching it, since
nothing in it depends on a single source root.

## Finding 3 — what a cross-TU mode still has to decide

Left for phase 1, deliberately not implemented while nothing merges modules:

- **Symbol identity.** `GlobalValue::getGlobalIdentifier()` qualifies
  local-linkage symbols with their module, and `getGUID()` hashes that. Joining
  globals by raw name would make two unrelated `static int counter` a single
  object, which for a race detector is a false positive rather than a lost
  annotation.
- **ABI compatibility.** `MemoryRegion` reasons in byte offsets. Merging facts
  from translation units with different `DataLayout` or target triple would
  compare offsets that do not describe the same layout.

## Platform coverage

`scripts/ci-linux.sh` reproduces the Linux CI job; `scripts/ci-matrix.sh` runs
it across architectures through buildx. The axis that has actually caught
defects is the standard library implementation, libc++ against libstdc++;
architecture has not yet changed a verdict.

The project requires libstdc++ 13 or newer, because a dependency includes
`<format>`. That rules out Debian 12 and Ubuntu 22.04 as matrix entries.
