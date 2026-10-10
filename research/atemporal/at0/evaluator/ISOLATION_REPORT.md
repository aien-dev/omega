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
62-case manifest, the 41 discriminating mutant pairs and the two isolation
controls are all checked without any candidate present. On a platform whose
`long double` is only 64 bits the shadow bound of 1e-20 would be false; the
shadow would then need its stated bound raised, and `run.sh` should be read
with that in mind. No such platform was used here.

## Known limits of the isolation gate

The symbol scan reads both the static and the dynamic symbol table, so a
stripped executable that imports a clock through the dynamic loader is still
caught, and `syscall` is on the forbidden list. It cannot see inline system
calls, raw counter reads (CNTVCT_EL0 on aarch64, rdtsc on x86-64) or symbols
resolved at run time without `dlsym`. Those are run-time behaviours, and the
wall-clock independence control is the only check that touches them. A
candidate that wants a stronger claim must be reviewed at source level.

Portability: `run.sh` needs GNU coreutils (`sha256sum`), `nm` from binutils and
a C11 compiler named by `CC` (default `cc`). On a platform whose `long double`
has a 64-bit mantissa (x86-64) the shadow states a 1e-14 bound instead of
1e-20; the evaluator refuses to run the shadow below 64 bits. Every run,
including `--self-only`, rewrites `results/qualification.json`; that file is the
deliverable and is meant to be committed with the run it describes.

## Scope of any PASS

A PASS in `results/qualification.json` means the AT-0 implementation conformed
to the frozen AT0_CASE_V1 / AT0_RESULT_V2 contracts on the cases run, and that
its numbers agree with an independent recomputation within stated bounds.
It is software conformance. It is not a scientific discovery verdict and must
not be cited as evidence about the physics of time.

## Independent sign-off on the Rust isolation adaptation (AT0-G2), 2026-10-09

Reviewed by Agent 4 against omega main 7843a74: `research/atemporal/at0/oracle/isolation.sh`
(Agent 2), `research/atemporal/at0/integration/isolation_check.sh` (Agent 5) and this
directory's `gates/isolation.sh`. Method: built the oracle compute crate alone as an
rlib three times (clean, `std::time::SystemTime::now` mutant, `extern "C" clock_gettime`
mutant) with the oracle's own flags, unpacked the members and ran all three scanners.

| scanner | clean | std::time mutant | extern clock mutant |
|---|---|---|---|
| integration/isolation_check.sh (Agent 5) | clean, exit 0 | HIT `_RNvMs5_NtCs..._3std4timeNtB5_10SystemTime3now` | HIT `clock_gettime` |
| oracle/isolation.sh object rule (nm -u regex) | clean | **no hit** (regex has no Rust std path) | hit |
| oracle/isolation.sh source rule (grep) | clean | hit (`std::time`) | hit (`extern "C"`) |
| evaluator gates/isolation.sh before D5 | clean | **no hit** (grepped `std..time`) | hit |
| evaluator gates/isolation.sh after D5 | clean | hit | hit |

Findings:

1. Agent 5's scanner is adequate for named symbols in C objects and Rust rlib
   members under both Rust mangling schemes. Signed off.
2. The oracle's own object rule is blind to a Rust-level clock at the symbol
   level; it catches the std::time mutant only through its source grep. That is
   acceptable for G2 because the source rule is part of the same gate and two
   independent scanners (Agent 5's and this one) now also catch it at the object
   level. Recommendation to Agent 2, not blocking: add `3std4time|SystemTime|Instant`
   to the banned regex so the object rule stands on its own.
3. My own gate had the same blind spot (D5). Fixed; three Rust controls run on
   every `run.sh` and are recorded as GATE-ISOLATION-RUST-* in qualification.json.
4. Limits that no symbol scan removes, restated: inline `svc`, raw counter reads
   (CNTVCT_EL0), clocks reached through function pointers resolved without
   `dlsym`. The wall-clock independence control (Agent 5's C3, this harness's
   CAND-WALLCLOCK-INDEPENDENCE) is the only run-time check on those.

Sign-off: the Rust adaptation of the isolation gate is fit for AT0-G2 on this
evidence. UNISOLATED still applies to the whole qualification, as above.
