# AT-0 Agent 4 isolation and reproducibility report

## Isolation status: UNISOLATED

The qualification run described by `results/qualification.json` is
**UNISOLATED**. No claim of independently blinded qualification is made.

Facts:

- Evaluator, candidate engine (omega `research/atemporal/at0/model`,
  merged omega#360), reference oracle and runner are developed and executed on
  the same host (the Spark, one Linux user account, one shared filesystem).
- The hidden qualification cases live outside the repository in
  `~/at0-private/agent4/` on that host. Any process running as that account
  can read them. The files are withheld from the public repository and from
  the candidate developer's briefs, but their non-access by other agents on
  this machine cannot be proven.
- `HIDDEN_COMMITMENT.txt` publishes the SHA-256 of each hidden file. This
  commitment establishes **data identity** (the files judged later are the
  files committed to now, and they were fixed before the candidate was run).
  It does **not** establish non-access and is not presented as doing so.
- The isolation gate (`gates/isolation.sh`) is a symbol-level scan of compute
  objects for clock, random, environment, file, network, process and thread
  entry points, with a positive control (an object that calls `clock_gettime`,
  which the gate must reject) and a negative control (a pure arithmetic object,
  which the gate must accept). It is evidence about the compiled candidate, not
  about the people and agents around it. Dynamic loading, inline syscalls and
  data smuggled through the case file itself are outside what a symbol scan can
  see; the wall-clock independence control (same case, different time zone,
  a pause, an extra environment variable; values and verdict_id must be
  identical, evidence digests must differ) covers part of that gap at run time.

What true isolation would require, for a later run that wants to claim it:
a separate account or machine holding the hidden set; candidate binaries
transferred by digest only; the hidden cases revealed to the candidate only
after a freeze commit of the candidate is published; and the evaluator run by
someone other than the candidate's author. None of those were available for
this run.

## Reproducibility

Everything in this directory is deterministic.

| item | value |
|---|---|
| language | C11 plus POSIX shell; no Python, no external libraries |
| compiler | gcc 13.3.0 (Ubuntu 13.3.0-6ubuntu2~24.04.1) |
| flags | `-std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L` |
| sanitizer build | `-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all`, run over codec, refusals, synthesis and verification |
| platform | Linux 7.0.0-1019-nvidia, aarch64 |
| long double | 16 bytes, IEEE binary128 (113-bit mantissa); shadow oracle bound stated as ESTIMATED 1e-20 |
| decision arithmetic | 4096-bit integers over the common denominator 2^1100 x 10^80; exact, no rounding |
| threads | 1; no timing, randomness or environment dependence anywhere in the evaluator |
| external code linked | omega `src/sha256.c` only |
| frozen contract commit | aien-architecture `044c9d11256d8642f80eedd42cbae8763faf63f5` |
| charter commit | aien-architecture `00e9f2308666f74e58f524b177b2a31eccdc69ba` |
| spec draft | aien-architecture PR #176 head `0efd1a14cbdf117bc694b556bb61d031cf80c8f9` |

To reproduce: `make && sh run.sh --self-only` from this directory on any POSIX
system with a C11 compiler. The known-answer digests, the hand table, the
60-case manifest, the 38 discriminating mutant pairs and the two isolation
controls are all checked without any candidate present. On a platform whose
`long double` is only 64 bits the shadow bound of 1e-20 would be false; the
shadow would then need its stated bound raised, and `run.sh` should be read
with that in mind. No such platform was used here.

## Scope of any PASS

A PASS in `results/qualification.json` means the AT-0 implementation conformed
to the frozen AT0_CASE_V1 / AT0_RESULT_V2 contracts on the cases run, and that
its numbers agree with an independent recomputation within stated bounds.
It is software conformance. It is not a scientific discovery verdict and must
not be cited as evidence about the physics of time.
