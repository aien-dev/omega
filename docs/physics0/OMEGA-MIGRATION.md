# PD-0 substrate: what is in the Omega language today, what moves later

Status at this commit (OSC-3 slice of the Omega Systems Core compiler; `docs/osc/OSC-1-SELF-HOST-STATEMENT.md` and `docs/osc/OSC-3-DESIGN.md` for what the language can express: integer scalars `u8..u64`, `i8..i64`, `bool`, fixed arrays of at most 64 cells, structs (OSC-2), bounded loops, no recursion, no bytes or strings, no I/O, no 128-bit integers).

## In Omega now

`tests/physics0/osc/pd0_helpers.osc` (same header format as the OSC golden corpus in `tests/compiler/progs/`): 10 functions, 62 lines.

| Function | Spec item | C twin |
| --- | --- | --- |
| `mul(a, b)` | 2.1 product rescale `(a*b)/1e6`, truncating | `pd0_mul` (`src/physics0/pd0_rng.c`) |
| `const_draw(lo, hi, u)` | 3 constant draw | `pd0_const` |
| `noise_map(u1..u4, sigma)` | 3 Irwin-Hall noise from four uniforms | `pd0_noise` |
| `l0_s0`, `l0_s1` | 4.1 L0 kinematics tick | `pd0_gen_step` case L0 |
| `l1_s0`, `l1_s1` | 4.1 L1 spring tick | `pd0_gen_step` case L1 |
| `l2_s1` | 4.1 L2 damped spring velocity tick | `pd0_gen_step` case L2 |
| `cube(s0)` | the L4 monomial `s0*s0*s0` | `pd0_monomial` with exponent 3 |
| `description_bits(...)` | 6.2 description length rule | `pd0_relation_description_bits` |

Differential test `tests/physics0/test_pd0_osc.c` (gate `PHYSICS0_OSC_DIFF`): the file is compiled by the OSC front end, every function runs through the reference interpreter and through the native AArch64 code (`osc_cg` + `osc_native`), and both results are compared with the C twin on 20 000 seeded input sets per function (200 000 runs, 0 mismatches at this commit, plain and ASan/UBSan). Inputs stay inside the world box (|v| <= 10.0 units, constants <= 9.0, `u` <= 2.0).

Line count at this commit (`wc -l`, comments included): Omega 62 lines (one `.osc` file) versus C 1 334 lines under `src/physics0/` and 925 lines of test-only C and shell under `tests/physics0/`. The Omega share of the substrate is the pure integer arithmetic of the spec; everything that touches bytes, hashes, processes or files is C.

## Why the rest is not in Omega yet, item by item

| Piece | Blocker in the language today | Moves when |
| --- | --- | --- |
| `pd0_mul` with 128-bit intermediate | no `i128`; `a*b` traps OVERFLOW for |a*b| >= 2^63. Inside the world box the i64 product is at most ~1e14, so the Omega `mul` agrees, but the general spec rule needs the wide product | OSC gains a wide multiply or an `i128` type |
| `pd0_gen_step` as one function over a state array | arrays of `i64` are expressible, but the step needs the level selector and constants as parameters (at most 6 scalar parameters, `OSC_MAX_PARAMS`) and no struct parameters | struct or array parameters beyond 6 scalars, or a module with globals |
| `pd0_relation_step` (sum over terms of coef * monomial) | the relation is a struct of arrays of structs; OSC-2 structs cannot hold arrays of structs and there are no array parameters across functions with bounds checks over 64 cells | nested aggregates or byte slices |
| SplitMix64 and FNV-1a | `u64` wrapping arithmetic exists; the stream needs the tag as bytes (`fnv1a64("const")`) | bytes / strings |
| PD0REC1 / PDLAW1 encode and decode, SHA-256 chain | bytes, slices, a SHA-256 in Omega | bytes plus a verified Omega SHA-256 |
| world server (pipe), `pd0-world` binary | no I/O, no processes | capabilities / effects (OSC-0 decision 5 says never inside the language core; an Omega runtime seam is needed) |
| least squares and NRMSE in the calibration code | floating point; the calibration is test-only anyway | stays C (or integer rational arithmetic if a future owner wants it bit-reproducible) |

## Rules kept

- The spec's canonical bytes are all integers; the Omega helpers are integer only, so a future Omega world can reproduce the same record bytes bit for bit.
- The `.osc` file carries `// expect-run:` headers in the golden corpus format so it can be moved into `tests/compiler/progs/` when the compiler lane wants it as a golden program.
- No production C was "migrated": the Omega functions are second implementations checked by differential test, not claims of self-hosting (OSC-1 statement, unchanged).
