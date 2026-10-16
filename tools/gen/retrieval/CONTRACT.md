# genret: file-backed retrieval provider, CLI contract `genret-cli-v2`

GEN-4 (aien-spark/omega#479, master #475). Owner: Agent E. Consumers: the GEN runner (Agent C, through this
CLI only) and the Cortex provider (aien-spark/aien-sovereign-core#556), which must reproduce `selected.tsv`,
`excluded.tsv` and `suffix.txt` byte for byte from the same corpus and query. Every rule below is integer
arithmetic or byte comparison so a second implementation can match it exactly. Any change to an observable
output is a new contract version.

Version history: v1 (commit c0c55920) read a provisional record format `gen-record v0`; it was never used on
a real corpus and is superseded. v2 reads Agent D's record schema v1 (`gen-record 1`,
tools/gen/abstract/RECORD_SCHEMA.md, sha256 9be17b6bc6f97c7420ced722bb258c420944bd87461097163df0992112ae6b53)
and validates every record with D's `recheck`. Changes from v1: new required `--recheck`; reasons
`unreceipted` and `poisoned:bytes` removed (recheck refuses such records, which is fatal); new fatal reason
`recheck_failed`; `contract=genret-cli-v2` in the receipt. Scoring, ranking, rendering and output files are
unchanged.

Arms C and D call the same command with the same `--k`, `--cap-bytes` and mode. The only difference is
`--kind raw` (C) versus `--kind abstraction` (D), and therefore which records are indexed.

## 1. Command

```
genret query --corpus <records/MANIFEST> --manifest-sha256 <64 lowercase hex> --recheck <recheck binary>
             --kind raw|abstraction --k <1..64> --cap-bytes <1..1048576>
             --requirement <file> --prompt <file> --out <new dir>
             [--exclude-family <family>]...  [--mode normal|irrelevant|empty|length-matched]
             [--target-bytes <1..cap>]         (required with, and only with, --mode length-matched)
genret version                                  prints "genret-cli-v2"
```

- Exit 0: success (an empty selection is a success). Exit 2: fatal (section 7); nothing is written and
  `--out` is not created. `--out` must not exist; genret creates it.
- Default mode: `normal`. Each option except `--exclude-family` at most once; unknown options are fatal.
- `--recheck`: Agent D's validator (`make gen-abstract` builds `build/gen_abstract/recheck`).

## 2. Corpus input

- Layout: RECORD_SCHEMA.md section 5. `--corpus` is `records/MANIFEST`; `--manifest-sha256` is the frozen
  record-set digest (`records/SET.sha256`). Record paths and `receipts/` resolve against the MANIFEST's
  directory (made absolute).
- `MANIFEST`: one line per record, `<64 lowercase hex sha256 of the record file>  <relative path>\n` (two
  spaces, `sha256sum` format), at least 1 and at most 65536 lines, final newline required, no blank lines, no
  comments. Path components match `[A-Za-z0-9._-]+`, none is `.` or `..`, no leading `/`. Duplicate paths are
  fatal. Files not listed are never read.
- sha256 of the MANIFEST bytes must equal `--manifest-sha256`; every record's bytes must equal its manifest
  digest (checked when read, and again after recheck has read the file).
- Every listed record is passed to `recheck --receipts <MANIFEST dir>/receipts <record>...` (batches of 128,
  manifest order). recheck checks schema v1 sections 1 to 3 and that the receipt file exists, is this
  record's and hashes to its id. It must exit 0 and print exactly `OK <id>` per record, in order, with the id
  genret read; exit 1 is fatal `invalid_record` with recheck's first REFUSED line; anything else is fatal
  `recheck_failed`. `source` digests (`recheck --root`) are not checked: the manifest pin covers the record
  bytes, and D's `verify` covers derivation.
- Fields genret reads from a record: `id kind split family validity entry params ret desc` (first line
  `gen-record 1`, each of these once) and the body (the bytes after the line `body\n` to end of file). All
  other key lines (signature, receipt, ancestry, source, pre, post, example, limit, counter, fingerprint,
  differs) are neither indexed nor rendered, so C and D show the same skeleton.

## 3. Query (public requirement + prompt only)

- `--requirement`: at most 4096 bytes, no NUL; exactly one line each matching `^entry( |$)`, `^params( |$)`,
  `^ret( |$)` (the same lines `tools/exp445/reader.sh user_msg` shows the model). Query text from it = the
  three values (the line after the key and one space), nothing else from the file.
- `--prompt`: 1..4096 bytes, only `\n` and 0x20..0x7E (HARNESS_INTERFACE_V2 section 3). Query text = all of it.
- A requirement or prompt path whose resolved absolute path has a component named `hidden` is fatal
  (`hidden_path`). genret opens no other task file.
- Query terms = the DISTINCT tokens (section 4) of the requirement values and the prompt, sorted by byte
  order; written to `query.txt`, one per line.

## 4. Tokenization

Lowercase ASCII `A-Z` to `a-z`; a token is a maximal run of bytes in `[a-z0-9]`; every other byte separates.
Record document text = the values of entry, params, ret, desc, then the body (each tokenized separately and
the token lists concatenated; field boundaries separate tokens). `dl` = number of tokens (with repeats).

## 5. Eligibility and indexing

For every manifest record, in this order, the first failing check is its exclusion reason:

1. `kind:<kind>`: kind differs from `--kind`
2. `split:<split>`: split is not `corpus` (schema v1: `probe` records are never eligible)
3. `validity:<validity>`: validity is not `verified` (schema v1: `unverified`)
4. `family_excluded:<family>`: one of its families is named by `--exclude-family` (first such, record order)
5. `poisoned:fence`: desc or body contains the three bytes "```" (could escape its code block)

Records that pass all five form the index: `N` records, `T` = sum of their `dl`, `df(t)` = number of index
records containing term `t`. Excluded records do not affect any score. A record is eligible only through a
MANIFEST whose sha256 is the pinned one (RECORD_SCHEMA.md section 5).

## 6. Scoring, ranking, selection

Integer BM25, k1 = 6/5, b = 3/4, log base 2, scores in units of 2^-16. For index record r and query term t
with `tf` = occurrences of t in r (only terms with tf > 0 and therefore df >= 1 contribute):

```
idf(t)    = log2q16(2N + 2, 2 df(t) + 1)
part(t,r) = floor( idf(t) * tf * 44 * T / (20 * T * tf + 6 * T + 18 * dl(r) * N) )     (128-bit intermediate)
score(r)  = sum of part(t,r) over the distinct query terms t

log2q16(a, b):   requires a >= b >= 1, a < 2^32; 64-bit unsigned arithmetic
  k = 0; while a >= 2b: b = 2b, k = k + 1
  y = floor(a * 2^30 / b)                      (2^30 <= y < 2^31)
  r = k * 2^16
  for i = 15 down to 0: y = floor(y * y / 2^30); if y >= 2^31: y = floor(y / 2), r = r + 2^i
  return r
```

(This is BM25 with idf = ln(1 + (N - df + 0.5)/(df + 0.5)), rescaled by 1/ln 2 and multiplied out by 20T.)

Rendering (exact bytes; kind noun K = `solutions to earlier tasks` for raw, `helper functions` for abstraction):

```
H        = "\nRetrieved records: verified " K ", most relevant first.\n"
block(i) = "\nRecord " i ".\n\n" "entry " entry "\n" "params" (" " param)* "\n" "ret " ret "\n" "\n" desc "\n\n```\n" body "```\n"
suffix   = "" when nothing is selected, else H block(1) ... block(n)
```

`i` is the decimal position in the suffix (1-based). `bytes` of a record = length of its block.

Order and selection by mode:

- `normal`: index records with score 0 are excluded (`no_match`). The rest are ordered by score descending,
  then id ascending (unsigned byte compare, a proper prefix sorts first). Walk that order; stop at the first
  record that would make the count exceed k or the suffix length exceed cap-bytes. That record and every
  later one are excluded with `beyond_k` (count already k) or `cap_bytes`. Records are never truncated.
- `irrelevant` (negative control): order ALL index records by score ascending, then id ascending; the same
  walk, k and cap. Score-0 records are not excluded.
- `empty` (negative control): nothing selected; every index record is excluded with `ablation_empty`.
  Corpus checks and fatal rules are unchanged.
- `length-matched` (preregistered ablation): `normal`, then one pad record: the first record logged
  `beyond_k` or `cap_bytes`, in rank order, whose block at position n+1 keeps the suffix length (with H if
  n = 0) at most `--target-bytes`. It is appended with role `pad`; if none fits, nothing is appended
  (`pad=none` in the receipt). `--target-bytes` must be <= `--cap-bytes`; intended use: C padded to D's
  realised `suffix_bytes`.

## 7. Fatal (exit 2, one stderr line `genret: FATAL <reason> <detail>`, no output)

`usage`, `manifest_unreadable`, `manifest_malformed`, `manifest_digest_mismatch`, `record_missing` (listed
file absent or unreadable), `stale_digest` (record bytes differ from the manifest digest), `invalid_record`
(recheck refused it, or genret cannot read a field it needs), `recheck_failed` (recheck missing, crashed, or
its output does not match the records), `duplicate_id`, `requirement_invalid`, `prompt_invalid`,
`hidden_path`, `out_exists`, `write_failed`. A record is never replaced by another one: integrity failures
stop the run.

## 8. Outputs in `--out`

| file | content |
|---|---|
| `selected.tsv` | header `rank\tid\tdigest\tscore\tbytes\trole`; one row per selected record in suffix order; role `top` or `pad` |
| `excluded.tsv` | header `id\tdigest\tscore\treason`; every manifest record not selected, sorted by id (byte order); score `-` when not indexed |
| `suffix.txt` | the exact bytes the runner appends in the variable suffix (may be empty) |
| `query.txt` | the query terms (section 3) |
| `receipt.txt` | `key=value` lines, fixed order: `contract=genret-cli-v2`, `mode`, `kind`, `k`, `cap_bytes`, `target_bytes` (`-` if unused), `exclude_family` (comma list or `-`), `manifest_sha256`, `requirement_sha256`, `prompt_sha256`, `query_sha256`, `records`, `indexed`, `total_tokens` (T), `selected`, `pad` (`none`, `-` or the id), `suffix_bytes`, `suffix_sha256`, `selected_sha256`, `excluded_sha256` |
| `SHA256SUMS` | `sha256sum` lines of the five files above, in that file order |

Digests are lowercase hex sha256. No timestamps, paths or host data in any output.
