# Joined thread phases

Issue #39: thread entries started and joined inside a helper, or across two loops, remain concurrent with entries from a later phase. The original task scan only compares concrete spawns and dominating joins in one function. The lifecycle collector treats a creation in a loop as unresolved unless its resolution is in the same loop.

## Contract

A completion proof identifies a point after **every instance** produced by a spawn site has been joined. Detach is ownership resolution, not completion. Unknown paths, incomplete ranges, overwritten handles and recursive/unresolved contexts must retain may-concurrency. Normal-return semantics match the existing MissingJoin analysis; exceptions that can reach a normal return are included by graph reachability.

## Owning boundaries

`thread_completion_analysis` proves scalar and loop completion over immutable LLVM IR. Both the lifecycle collector and task analysis consume this proof. It owns matching handles, complete indexed ranges, induction/bound validation and completion barriers; neither rule checker recognizes helper names or source filenames.

`contained_entry_analysis` propagates concrete or parameter-bound entries through direct calls. Each occurrence retains its call frames and a completion point per frame. At the common caller, a completed phase must precede the next spawn. An entry pair is excluded only when **every observed pair of occurrences** is ordered; a second overlapping call vetoes the exclusion. Worker-reachable roots and unresolved/recursive expansions cannot supply a never-overlap proof. The expansion budget is a conservative precision limit: overflow discards new exclusions.

The existing `sequencedEntryPairs` fact remains the rule-facing interface, shared by race and lock-order queries. No public API or diagnostic schema changes. IR pointers remain local to the unit build and owned strings remain the persistent fact identity.

## Loops and standard library boundary

The initial proof supports local arrays traversed by matching unit-step counted loops with immutable equal bounds and index paths. A join must execute on every iteration; early exits that can return normally invalidate coverage. Reusing a scalar handle for multiple creations is not an indexed range.

For a fixed-size `std::vector` of pthread handles, indexed creation followed by a full begin/end range traversal has the same contract. The model recognizes standard library operations, not application names: construction with the same bound, indexing, begin/end, iterator dereference/increment/comparison. Container mutations or unrecognized iterator operations invalidate the proof. Both libc++ and libstdc++ are validation targets. Growable containers, arbitrary iterator adapters and inter-unit helper bodies remain conservative.

LLVM 20 documentation consulted:

- https://releases.llvm.org/20.1.0/docs/LoopTerminology.html — loop headers, latches, exiting blocks and zero-iteration loops; dominance of one join instruction cannot prove all elements were joined.
- https://releases.llvm.org/20.1.0/docs/NewPassManager.html — reuse per-function analyses and respect their lifetime/invalidation boundary.
- https://releases.llvm.org/20.1.0/docs/ProgrammersManual.html — context isolation across concurrently analyzed units.

## Regression matrix

Silent: scalar helpers, nested parameterized helpers, distinct template trampolines. Array/vector phases retain races between workers **within** each phase while losing only the cross-phase race. Positive controls: no join, detach, conditional join, shorter join range, early break, overwritten handle, and an extra overlapping invocation of an otherwise contained entry.

The old loop reproduction starts two workers per phase which both increment the same global. It is not a wholly race-free program: expecting zero races would weaken the test. Its correct race count is two (one per phase), with no MissingJoin warning.
