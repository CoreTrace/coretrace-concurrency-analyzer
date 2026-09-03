# Security Policy

## Supported versions

The project is below 1.0.0, so only the most recent minor line receives fixes.
When a new minor is released, the previous one stops being supported.

| Version | Supported |
| ------- | --------- |
| 0.1.x   | yes       |
| < 0.1   | no (no such release) |

## Reporting a vulnerability

Report privately through GitHub, at
[Security → Report a vulnerability](https://github.com/CoreTrace/coretrace-concurrency-analyzer/security/advisories/new).
Please do not open a public issue for something you believe is exploitable.

A report is most useful with the version or commit, the platform and LLVM
version, an input that reproduces the problem, and what you observed versus
what you expected.

This is a small project. Expect an acknowledgement within about a week, and an
assessment of whether the report is accepted once it has been reproduced. If a
fix is warranted it ships in a new patch release, and the advisory is published
with credit to the reporter unless they ask otherwise. If a report is declined,
you will be told why.

## What is in scope

The analyzer **compiles the sources it is given** before analysing them, which
is the part worth attention. Anything that lets a crafted input escape that
compilation step — writing outside the working directory, executing code beyond
what the compiler legitimately runs, or reading files the caller never named —
is in scope, as is anything of that kind reachable through
`compile_commands.json` in cross-translation-unit mode.

## What is not in scope

**Passing untrusted compiler arguments.** `--compile-arg` and
`extraCompileArgs` are forwarded to the compiler without sanitization, by
design. This is documented under
[Trust Model](README.md#trust-model-for---compile-arg--extracompileargs): use
the tool with trusted arguments, or put an allowlist in front of it. Reports
that consist of passing a hostile `--compile-arg` describe that documented
behaviour rather than a defect.

**Vulnerabilities in LLVM or Clang.** The analyzer builds on them but does not
ship them; report those to the LLVM project.

**Wrong analysis results.** A missed race or a false positive is a correctness
bug, not a vulnerability. Open a normal issue.

## Analysing code you do not trust

Because analysing a file means compiling it, treat running the analyzer over
untrusted sources as running a compiler over them, and isolate it accordingly.
The published container image is one way to do that:

```sh
docker run --rm --network none -v "$PWD:/work:ro" \
  ghcr.io/coretrace/coretrace-concurrency-analyzer:v0.1.1 suspicious.c --analyze
```
