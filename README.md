# OMEGA

Omega is the reaction runtime and the compiler for Omega Systems Core, written in C. It defines what computation means without committing that meaning to one machine representation: every program has a content-addressed semantic identity, and realizations (CPU, GB10 GPU) are derived from it and verified against it.

```text
SILICON -> ATLAS -> PHYSICS/FORGE -> OMEGA -> AIEN        (full picture: aien-architecture)
```

Omega was decided by Drake on 2026-09-29 to be both the reaction runtime and the compiler for Omega Systems Core ([decision record](docs/adr/OMEGA-SYSTEMS-CORE-0000.md), frozen). This retires the earlier "not a compiler" wording.

## Current state

Research-grade and pre-alpha. Plainly:

- **Omega Systems Core compiler:** OSC-0 is frozen as a design. OSC-1 and OSC-2 are implemented with host receipts and are NOT QUALIFIED. OSC-3 is in progress ([OSC-3-DESIGN](docs/osc/OSC-3-DESIGN.md)). It is not self-hosting and there is no general Omega compiler yet; the old Milestone 6 "self-host" result was a fixed-output self-copy check, not compilation ([note](docs/osc/OSC-1-SELF-HOST-STATEMENT.md)).
- **Reaction runtime:** the R1 to R16 qualification ladder passed on builds that predate the composition modules (omega#126). The current living build is IMPLEMENTED / NOT QUALIFIED until re-qualified. Composition, Capability Graph, Skill Router, J-Space and Cortex run on the host; the Fabric interface is a host-only loopback with no network; none of this is on silicon as a qualified system.
- **Numerics (E1):** 2 of 6 exit requirements met; E1 is not closed. GB10 reductions and division/square root have chip receipts; see [E1_GAP_TABLE](docs/numeric/E1_GAP_TABLE.md).
- **Hardware:** results on the DGX Spark GB10 are tied to the exact commit in each receipt. Host and emulator passes are never reported as hardware qualification.

The live status is owned by [aien-architecture](https://github.com/aien-dev/aien-architecture): [CURRENT_EXECUTION_PLAN.md](https://github.com/aien-dev/aien-architecture/blob/main/CURRENT_EXECUTION_PLAN.md) and [doctrine/ROADMAP.md](https://github.com/aien-dev/aien-architecture/blob/main/doctrine/ROADMAP.md). This file does not restate numbers that drift. Historical milestone receipts (for example Milestone 4, `OMEGA_SEMANTICS`, 12 of 12 gates) stay in `evidence/` unchanged as history.

## Core ideas

- **SMART is not TRUSTED, INTELLIGENCE is not AUTHORITY.** Trust comes from verified invariants, never from capability.
- **Meaning is permanent; representation is disposable.** Equivalent programs collapse to one `SEMANTIC_ID` (SHA-256 of the canonical encoding); different programs get different ones.
- **Pure computation is separate from effects**, and effects need capability-bounded, receipt-producing transactions.

## How this fits with the other repositories

[aienos](https://github.com/aien-dev/aienos) is the kernel Omega is meant to run on. [physics](https://github.com/aien-dev/physics) provides FORGE machine realization and the GB10 native path (pinned in `physics.lock`). [aien-architecture](https://github.com/aien-dev/aien-architecture) owns status and sequencing. ARGUS-0/1 (a defensive plane), Physics Zero, DIRAC-0 and the Evolution Arena are specifications and plans only.

## Standing rules

C is the target language, with assembly only where measured. No Python anywhere (tools are C or shell). No CUDA toolkit or CUDA library dependence: the GB10 is driven natively. No systemd. Offline, in-house builds. Never overwrite failed experiments or old receipts. Verdict words: PASS, FAIL, NOT_RUN, BLOCKED_HARDWARE, BLOCKED_OPERATOR, MISSING_IMPLEMENTATION.

## Build and verify

Needs gcc and make. Host suites need no GPU and no physics checkout:

```bash
make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-compiler-quick   # compiler, quick suite
make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-numeric-cpu      # E1 CPU numerics
make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-realize
```

The full omegatool build (`make`) links the `physics` repository at the commit in `physics.lock`. Some older milestone gate scripts rewrite their receipts in place, so do not run them on a clean checkout you mean to keep; prefer the `make test-*` targets above. Targets that touch the GB10 need the DGX Spark, run one at a time, and are never killed mid-run. CI definitions in `.github/workflows/` list the suites that run on every push.

## Repository layout

`src/` core and C compiler (`src/compiler/`), `src/runtime/` reaction runtime, `spec/` and `docs/` specifications, `tests/` gate suites, `evidence/` content-addressed receipts, `tools/` harnesses.

## Contributing

Open a pull request with the exact command you ran and its output. Receipts are content-addressed, name the exact commit and refuse a dirty tree. Do not edit existing evidence in place. External review is expected before merge. See [CONTRIBUTORS.md](CONTRIBUTORS.md). Contact: aien@aienos.com.
