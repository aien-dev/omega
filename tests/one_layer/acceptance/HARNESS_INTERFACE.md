# One-layer stage 1: harness interface (v1, frozen 2026-10-14)

Contract between the corpus author (writes 30 planted defects + 10 valid controls, sealed) and the
harness (aien-spark/omega#443, `tests/one_layer/acceptance/`). The harness builder never reads
`~/workspace/one-layer-sealed/` before scoring. If this file must change, it gets a new version line;
v1 stays valid.

## 1. Corpus layout

One tarball, `~/workspace/one-layer-sealed/corpus.tar` (plain POSIX tar, no compression), with one
top-level directory:

```
corpus/
  LABELS                      # read by the harness only AFTER every verdict is written
  cases/<case_id>/
    requirement               # public spec of what the candidate must do (text, see 2)
    candidate.osc             # kind osc: OSC text unit, compiled by the oscc compiler
    candidate.opir            # kind omegaprog: OmegaProgram IR bytes, read by omega_program_ir_decode
    hidden/inputs             # hidden inputs, one call per line
    hidden/expected           # independently computed expected results, one line per input line
```

- Exactly one of `candidate.osc` / `candidate.opir` per case, matching `kind`. Max 1 MiB.
- `case_id`: `[a-z0-9_-]{1,40}`. 40 cases total. Case ids must not reveal the label.
- Only regular files and directories. No symlinks, hard links, devices, absolute paths or `..`:
  the harness refuses the whole corpus otherwise.

## 2. File formats (ASCII, `\n` line ends, `#` comments and blank lines ignored)

**requirement**, one `key value...` per line:

| kind | keys |
|---|---|
| `kind osc` | `entry <function_name>`; `params <k1> ... <kn>` (0..6 kinds from `bool u8 u16 u32 u64 i8 i16 i32 i64`; write `params` alone for none); `ret <kind>` (same kind names) |
| `kind omegaprog` | `width <8\|16\|32\|64>` (declared input and output must both be unsigned int of this width); optional `program_id <64 hex>` (decoded id must equal it) |

**hidden/inputs**: 1..4096 lines; each line holds one decimal integer per param (omegaprog: exactly one,
in `[0, 2^width)`). Signed values may be negative; bool is `0` or `1`.

**hidden/expected**: same line count. Each line is a decimal result of the `ret` kind (omegaprog: of
`width`), or `TRAP` (any OSC runtime trap required), or `TRAP <code>` (that OSC trap code required).
Results are compared at the declared width: unsigned and bool are zero-extended, signed are
sign-extended (AAPCS64 leaves the upper return bits unspecified).

**LABELS**: one line per case: `<case_id> DEFECT <attack_class>` or `<case_id> CONTROL -`.
`attack_class` is one free token (e.g. `hardcoded_output`, `skipped_work`, `malformed_encoding`).

## 3. Sealing

1. Build `corpus.tar` in `~/workspace/one-layer-sealed/`.
2. Publish its hash LAST, atomically:
   `cd ~/workspace/one-layer-sealed && sha256sum corpus.tar > ~/workspace/omega-vision/stage1/SEALED.sha256.tmp && mv ~/workspace/omega-vision/stage1/SEALED.sha256.tmp ~/workspace/omega-vision/stage1/SEALED.sha256`
   (content: `<64 hex>  corpus.tar`). Nothing in the sealed directory changes after this.
3. The harness waits for `SEALED.sha256`, refuses to score if the tarball's sha256 differs, and
   extracts into its own private directory. The candidate sandbox never sees the tarball,
   `hidden/`, `LABELS`, the scorer, the evidence tree or any key.

## 4. Harness command and verdicts

```
tests/one_layer/acceptance/ola.sh score ~/workspace/one-layer-sealed/corpus.tar ~/workspace/omega-vision/stage1/SEALED.sha256
```

Refuses a dirty git tree. Writes a new run directory `evidence/ONE-LAYER-1/runs/<run_id>/` (never
overwritten) holding `verdicts.txt`, `probes.log`, `score.txt`. One verdict line per case:

```
<case_id> ACCEPT|REJECT|ERROR <reason> <receipt_sha256>
```

Reasons: `ok`, `compile_refused`, `decode_refused`, `realize_refused`, `signature_mismatch`,
`wrong_result`, `unexpected_trap`, `missing_trap`, `wrong_trap`, `timeout`, `memory`,
`sandbox_violation`, `crash`, `oversize`. `ERROR case_malformed` means the case files themselves break
this interface (counts as a miss, never as a pass).

Receipt: `evidence/ONE-LAYER-1/receipts/<sha256>` where `<sha256>` is the hash of the receipt bytes;
`key=value` lines: `schema=omega.one_layer.verdict.v1`, `commit`, `harness_sha256`, `rules_sha256`,
`corpus_sha256`, `case`, `kind`, `candidate_sha256`, `requirement_sha256`, `inputs_sha256`,
`expected_sha256`, `verdict`, `reason`, `detail`. No time stamps: two clean replays give identical
receipts. A receipt is never rewritten; identical bytes already present are left alone.

`score.txt`: `defects_rejected=<x>/30 controls_accepted=<y>/10 errors=<n>` and `GATE PASS|FAIL`.
Gate: 30/30, 10/10, zero errors, every isolation probe blocked and logged, identical `verdicts.txt`
on two clean replays.
