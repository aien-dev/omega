# genret: file-backed retrieval provider, CLI contract `genret-cli-v1`

GEN-4 (aien-spark/omega#479, master #475). Owner: Agent E. Consumers: the GEN runner (Agent C, through this
CLI only) and the Cortex provider (aien-spark/aien-sovereign-core#556), which must reproduce `selected.tsv`,
`excluded.tsv` and `suffix.txt` byte for byte from the same corpus and query. Every rule below is integer
arithmetic or byte comparison so a second implementation can match it exactly. Any change to an observable
output is a new contract version; v1 stays valid.

Arms C and D call the same command with the same `--k`, `--cap-bytes` and mode. The only difference is
`--kind raw` (C) versus `--kind abstraction` (D), and therefore which records are indexed.

## 1. Command

```
genret query --corpus <MANIFEST file> --manifest-sha256 <64 lowercase hex> --kind raw|abstraction
             --k <1..64> --cap-bytes <1..1048576> --requirement <file> --prompt <file> --out <new dir>
             [--exclude-family <family>]...  [--mode normal|irrelevant|empty|length-matched]
             [--target-bytes <1..cap>]         (required with, and only with, --mode length-matched)
genret version                                  prints "genret-cli-v1"
```

- Exit 0: success (an empty selection is a success). Exit 2: fatal (section 7); nothing is written and
  `--out` is not created. `--out` must not exist; genret creates it.
- Default mode: `normal`. Each option except `--exclude-family` at most once; unknown options are fatal.

## 2. Corpus input

- `MANIFEST`: one line per record, `<64 lowercase hex sha256 of the record file>  <relative path>\n` (two
  spaces, `sha256sum` format), at least 1 and at most 65536 lines, final newline required, no blank lines, no
  comments. Paths are relative to the manifest's directory; components match `[A-Za-z0-9._-]+`, none is `.`
  or `..`, no leading `/`. Duplicate paths are fatal. Files not listed are never read.
- sha256 of the MANIFEST bytes must equal `--manifest-sha256` (the pinned record-set digest).
- Record file format: owned by Agent D (GEN-3). Until it lands genret reads the provisional fixture format
  `gen-record v0` (section 2.1); genret depends only on the fields named there.

### 2.1 Provisional record format `gen-record v0` (fixtures only; replaced by Agent D's skeleton)

```
gen-record v0
<key> <value>          one line per key, any order, each exactly once (ancestry optional)
body
<body bytes to end of file>
```

At most 65536 bytes. Header lines: bytes 0x20..0x7E only. Keys and values:

| key | value | used for |
|---|---|---|
| id | `[a-z0-9_-]{1,64}`, unique in the manifest | tie-break, logs |
| kind | `raw` or `abstraction` | kind filter |
| split | `[a-z0-9_-]{1,64}` | only `corpus` is eligible |
| family | 1+ space-separated `[a-z0-9_-]{1,64}` | `--exclude-family` |
| validity | `[a-z]{1,16}` | only `verified` is eligible |
| receipt | 64 lowercase hex test-receipt id, or `none` | `verified` needs one |
| entry | `[A-Za-z_][A-Za-z0-9_]{0,63}` | indexed, rendered |
| params | zero or more space-separated `[a-z0-9]{1,16}` (line may be just `params`) | indexed, rendered |
| ret | `[a-z0-9]{1,16}` | indexed, rendered |
| desc | one line, 1..512 bytes | indexed, rendered |
| ancestry | 1+ space-separated ids (not checked by genret) | not used |

`body`: the bytes after the line `body\n` up to end of file; at least 1 byte, must end with `\n`.
A missing, repeated or unknown key, a value outside its pattern, or a bad body is fatal (`invalid_record`).

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
2. `split:<split>`: split is not `corpus`
3. `validity:<validity>`: validity is not `verified`
4. `unreceipted`: receipt is `none`
5. `family_excluded:<family>`: one of its families is named by `--exclude-family` (first such, record order)
6. `poisoned:bytes`: desc or body holds a byte other than `\n` and 0x20..0x7E;
   `poisoned:fence`: desc or body contains the three bytes "```" (could escape its code block)

Records that pass all six form the index: `N` records, `T` = sum of their `dl`, `df(t)` = number of index
records containing term `t`. Excluded records do not affect any score.

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
file absent or unreadable), `stale_digest` (record bytes differ from the manifest digest), `invalid_record`,
`duplicate_id`, `requirement_invalid`, `prompt_invalid`, `hidden_path`, `out_exists`, `write_failed`.
A record is never replaced by another one: integrity failures stop the run.

## 8. Outputs in `--out`

| file | content |
|---|---|
| `selected.tsv` | header `rank\tid\tdigest\tscore\tbytes\trole`; one row per selected record in suffix order; role `top` or `pad` |
| `excluded.tsv` | header `id\tdigest\tscore\treason`; every manifest record not selected, sorted by id (byte order); score `-` when not indexed |
| `suffix.txt` | the exact bytes the runner appends in the variable suffix (may be empty) |
| `query.txt` | the query terms (section 3) |
| `receipt.txt` | `key=value` lines, fixed order: `contract=genret-cli-v1`, `mode`, `kind`, `k`, `cap_bytes`, `target_bytes` (`-` if unused), `exclude_family` (comma list or `-`), `manifest_sha256`, `requirement_sha256`, `prompt_sha256`, `query_sha256`, `records`, `indexed`, `total_tokens` (T), `selected`, `pad` (`none`, `-` or the id), `suffix_bytes`, `suffix_sha256`, `selected_sha256`, `excluded_sha256` |
| `SHA256SUMS` | `sha256sum` lines of the five files above, in that file order |

Digests are lowercase hex sha256. No timestamps, paths or host data in any output.
