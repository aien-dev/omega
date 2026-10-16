# GEN record schema v1 (raw and abstraction records)

Status: v1, frozen 2026-10-15 (GEN-3, aien-spark/omega#478, part of #475). If this file must change it gets a
new version line and a new header number; v1 stays valid. Validator: `tools/gen/abstract/recheck.c`
(`make test-gen-abstract` builds `build/gen_abstract/recheck`).

One record is one small text file. Retrieval arm C serves `raw` records, arm D serves `abstraction`
records; both use the same field skeleton, only the body kind differs. A record is a suggestion shown to a
model, never trusted code: whatever a model writes after reading one still goes through the whole reader
chain (omegese emit, omega_emit, checker2 or the harness).

## 1. Bytes

- ASCII 0x20..0x7E and LF only. Every line ends with LF. No CR, no tab, no empty line, no line that starts
  or ends with a space, no two spaces in a row. At most 1024 bytes per line, 65536 bytes per file.
- Fields come in the fixed order below, each key exactly once unless marked `+` (one or more) or `*` (zero or
  more). A key line is `<key> <value>`. The body runs to the end of the file.
- No time stamps, host names or absolute paths anywhere; the same inputs give the same bytes.

## 2. The shared skeleton (both kinds)

| # | line | rule |
|---|---|---|
| 1 | `gen-record 1` | header and schema version |
| 2 | `id <ID>` | `raw-<task_id>` for kind raw; `abs-<h16>` for kind abstraction, `<h16>` = the first 16 hex digits of its `fingerprint` |
| 3 | `kind raw` or `kind abstraction` | |
| 4 | `split corpus` or `split probe` | `corpus`: from a task the extractor used (eligible for retrieval); `probe`: a transfer-probe task withheld from extraction (raw records only; not eligible, so arms C and D draw from the same tasks) |
| 5 | `family <fam>+` | family ids, strictly ascending, single spaces. `<fam>` = `gs1-` + the first 16 hex of sha256 of the task's FAMSIG v1 signature string (`gs1:...`, as `famsig canon` prints it, no newline), the signature taken from `tests/one_layer/generalization/corpus/META`. raw: exactly the task's family; abstraction: every family of its ancestry tasks |
| 6 | `entry <name>` | the function name (raw: the task's entry; abstraction: the helper) |
| 7 | `params( <word>)*` | the parameter types in order, task-requirement spelling (`bool u8 u16 u32 u64 i8 i16 i32 i64`); an array parameter is the three words `view|edit|own <scalar> <len>`; the line is just `params` when there are none |
| 8 | `ret <type>` | return type, requirement spelling, or `void` |
| 9 | `signature <fn line>` | the Omegese function line, exactly as in the body: `fn <name> (<param> <type>)* to <type>`; must agree with lines 6 to 8 |
| 10 | `desc <text>` | one line, 1..160 bytes |
| 11 | `receipt <64 hex>` | the test receipt id: sha256 of the receipt file (section 4) |
| 12 | `validity verified` or `validity unverified` | what the producing tool claims; only `verify` (section 5) can confirm `verified` |
| 13 | `ancestry <task_id> <64 hex>` + | corpus task and sha256 of its `solution.omegese`; strictly ascending by task id |
| 14 | `source <path> <64 hex>` + | every file the record was derived from, path relative to the repository root (`[A-Za-z0-9_./-]`, no `.` or `..` part, no leading `/`), sha256 of the file; strictly ascending by path |
| .. | kind abstraction only: section 3 lines | |
| last | `body`, then the body lines to the end of the file | at least one line, at most 400; the Omegese source |

- `task_id`: `[a-z0-9_-]{1,40}`. Hex is lowercase.
- In the signature and body, Omegese types use their one-token spelling (`bool uchar ushort uint ulong byte
  short int long`, arrays `view|edit|own <scalar> <len>`, len 1..64); lines 7 and 8 use the requirement
  spelling, so `uint` <-> `u32` and so on.
- **raw**: exactly one `ancestry` line, whose task id is the id's suffix; the body is the task's whole verified
  solution (helper functions, then the entry); `signature` equals the body's LAST `fn` line (the entry).
- **abstraction**: `split corpus`; the body is exactly one function (line 1 is the only `fn` line, the last
  line is `end`); `signature` equals body line 1; `ret` is a scalar.
- Field names follow the retrieval provider's provisional `gen-record v0` (tools/gen/retrieval/CONTRACT.md
  section 2.1, commit c0c55920) where they do not conflict: `id kind split family entry params ret desc
  receipt validity ancestry body`. Differences from v0: header `gen-record 1`; fixed key order; `ancestry`
  and `source` (and the section 3 keys `limit counter differs`) repeat, one value per line; `desc` at most
  160 bytes; extra keys `signature source` and section 3.

## 3. Abstraction extension (kind abstraction, after `source`, before `body`, in this order)

| line | rule |
|---|---|
| `pre STATED+TESTED <value>` | one-line precondition: an Omegese value of type bool over the parameter names (`true` = no precondition) |
| `post STATED+TESTED <value>` | one-line postcondition: an Omegese bool value over the parameter names and `res` (the returned value) |
| `example <args> -> <value>` | one example call: decimal arguments, one per scalar parameter and `len` per array parameter (its elements in order), and the result |
| `limit <text>` + | a limit of the helper, one line each, 1..160 bytes |
| `counter <args> -> <value>` or `-> TRAP <code>` * | a counterexample: arguments where `pre` is false and the helper's result breaks `post` (or it traps with that code 1..14) |
| `fingerprint <64 hex>` | canonical IR fingerprint of the helper (section 6) |
| `differs <task_id> <64 hex>` + | canonical IR fingerprint of EVERY corpus solution (extraction and probe tasks), strictly ascending by task id; none may equal `fingerprint` |

Further rules: at least two `ancestry` lines with distinct task ids AND distinct solution digests. `pre` and
`post` carry the tag `STATED+TESTED`: STATED = written by the corpus author in the helper's spec file, TESTED =
run by the extractor on the record's generated inputs, the example and every counterexample, with the result
in the receipt. Nothing in v1 is proved; the tag `PROVED` is reserved for a property checker2 itself proves
(`checker2 prove-check`) and is refused by v1 tools. A helper parameter may not be named `res`.

## 4. Test receipt

A receipt is a text file under the record set's `receipts/<64 hex>`, named by its own sha256 (the `receipt`
line). Bytes as section 1. Line 1 `gen-receipt 1`, line 2 `record <ID>`, then `<key> <value>` lines
written by the extractor (tool source digests, emit and checker2 results, every functional check with the
sha256 of its inputs and expected files and its verdict). A receipt is a log of checks that were run, not a
signature: `verify` re-runs every check and requires the same bytes.

## 5. Record set and status

```
records/
  MANIFEST                   every record file, `sha256sum` format (`<64 hex>  <path>`, two spaces), paths
                             relative to records/, ascending by path: the genret `--corpus` manifest
  SET.sha256                 sha256 of MANIFEST (the record-set digest frozen before a run, genret `--manifest-sha256`)
  raw/<id>.rec               one per corpus task
  abstraction/<id>.rec       one per helper that passed every check
  receipts/<64 hex>          one per record
  REJECTIONS                 one line per helper group that did not become a record, with its reason
```

`recheck` checks the format (sections 1 to 3); with `--root` it also checks every `source` digest against
the file and that the receipt file exists and hashes to its name. `verify` (tools/gen/abstract/verify) re-runs
the whole derivation of one record and prints `VERIFIED <id>` or `BLOCKED <id> <reason>`. A consumer treats a
record as verified only when it is listed in a MANIFEST whose sha256 is the frozen one, or `verify` says so.

## 6. Canonical IR fingerprint

`omegese emit` -> `omega_emit emit` -> OSC binary IR, after every function name in the op list is replaced by
`fn<k>_` (k = 0, 1, ... in program order; the `"name"` and `"func"` keys). The fingerprint is the sha256 of
those IR bytes. Parameter and local names never reach the IR (registers are numbered), so helpers that differ
only by names (alpha-equivalent) have one fingerprint. A helper's fingerprint is that of the one-function
program made of it; a corpus solution's fingerprint is that of the whole solution.
