# genret: file-backed retrieval provider, CLI contract `genret-cli-v3`

GEN-4 (aien-spark/omega#479, master #475). Owner: Agent E. Consumers: the GEN runner (Agent C, through this
CLI only) and the Cortex provider (aien-spark/aien-sovereign-core#556), which must reproduce `selected.tsv`,
`excluded.tsv` and `suffix.txt` byte for byte from the same corpus and query. Every rule below is integer
arithmetic or byte comparison so a second implementation can match it exactly. Any change to an observable
output is a new contract version.

Version history:
- v1 (commit c0c55920) read a provisional record format `gen-record v0`. It was never used on a real corpus.
- v2 (commit a4878d69) reads Agent D's record schema v1 (`gen-record 1`, tools/gen/abstract/RECORD_SCHEMA.md,
  sha256 9be17b6bc6f97c7420ced722bb258c420944bd87461097163df0992112ae6b53) and validates every record with D's
  `recheck`.
- v3 (this file), after the PR #485 review:
  - One heading for both kinds, so C and D suffixes differ only in record content (section 6).
  - Length-matched pads with as many whole records as it takes to reach the target, and reports why it
    stopped (section 6).
  - New required `--recheck-sha256`. genret refuses any other validator binary (new fatal reason
    `recheck_untrusted`) and names the validator in the receipt.
  - Re-review fold-in, with outputs for valid inputs unchanged: genret is bound to the recheck binary
    built in the same make run (section 2), and directories are fsynced around the final rename.
  - Task files are opened once and read from the same descriptor.
  - Outputs appear atomically.
  - New fatal reason `internal`.
  - New receipt keys `recheck_sha256`, `recheck_source_sha256`, `pad_reason`, `unpadded_bytes` and
    `length_gap`. `pad` is now a count.

  Scoring, ranking, tokenization, eligibility and the record block format are unchanged from v2.

Arms C and D call the same command with the same `--k`, `--cap-bytes`, mode and rendering. The only
difference is `--kind raw` (C) versus `--kind abstraction` (D), and therefore which records are indexed.

## 1. Command

```
genret query --corpus <records/MANIFEST> --manifest-sha256 <64 lowercase hex>
             --recheck <recheck binary> --recheck-sha256 <64 lowercase hex>
             --kind raw|abstraction --k <1..64> --cap-bytes <1..1048576>
             --requirement <file> --prompt <file> --out <new dir>
             [--exclude-family <family>]...  [--mode normal|irrelevant|empty|length-matched]
             [--target-bytes <1..cap>]         (required with, and only with, --mode length-matched)
genret version                                  prints "genret-cli-v3"
```

- Exit 0: success (an empty selection is a success). Exit 2: fatal (section 7). `--out` is not created.
- `--out` must not exist. genret writes all files into a new sibling directory `<out>.tmp.<pid>` and
  renames it to `--out` without replacing anything (Linux `renameat2` with `RENAME_NOREPLACE`), so `--out`
  either does not exist or holds every output file. Each file, the temp directory, and after the rename the
  parent directory are fsynced. If that last fsync fails, `--out` is removed again and the run is fatal
  `write_failed`. If genret is killed before the rename, a `<out>.tmp.<pid>` directory may remain. It is
  never `--out`.
- Default mode: `normal`. Each option except `--exclude-family` at most once. Unknown options are fatal.
- `--recheck`: Agent D's validator. `make gen-retrieval` builds `build/gen_retrieval/recheck` from the
  pinned `recheck.c`, and writes its sha256 to `build/gen_retrieval/recheck.sha256`. That value is what
  callers pass as `--recheck-sha256`. The genret from the same make run accepts only that binary
  (section 2).

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
- Validator pin: before reading any task or record file, genret opens `--recheck` once and hashes its
  bytes. If the sha256 differs from `--recheck-sha256`, the run stops with fatal `recheck_untrusted`. If the
  file is missing or unreadable, the reason is `recheck_failed`. Every batch executes that same open file
  (`fexecve`), so the program that runs is the one that was hashed.
- Validator binding: the make run that builds genret first checks `recheck.c` against its pinned sha256,
  builds `build/gen_retrieval/recheck` from it, and then compiles both digests into genret: the source digest
  and the digest of that binary. genret also refuses (`recheck_untrusted`) any `--recheck` whose sha256 is
  not the built-in binary digest, even when it matches `--recheck-sha256`. The receipt records
  `recheck_sha256` (the binary that ran) and `recheck_source_sha256` (the verified `recheck.c` it was built
  from), so both are true claims.
- Test builds: `genret_anyrecheck` is compiled without the binary binding so the tests can drive fake
  validators. Its receipt always says `recheck_source_sha256=unverified`. Its other outputs equal the
  production build's, and the tests check that. Experiment runs use `genret` only.
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
- Each task file is opened once. genret takes the resolved absolute path of that open file
  (`/proc/self/fd/N`), refuses it if any component is exactly `hidden` (fatal `hidden_path`), and refuses
  anything but a regular file (`requirement_invalid` / `prompt_invalid`). It then reads from the same
  descriptor, so a symlink swapped in after the check changes nothing.
  - The match is byte-exact and case-sensitive: `Hidden` is not refused.
  - A hard link to a hidden file placed outside `hidden/` cannot be detected this way.
  - genret opens no other task file.
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

Only the fence is screened here. `desc` is rendered as plain prose outside the code block (section 6), so
its wording reaches the model unfenced. Its content is owned by the record producers and by recheck's
schema checks (one printable line), not by genret.

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

Rendering (exact bytes, the same for both kinds: a C suffix and a D suffix with the same number of records
differ only in the entry, params, ret, desc and body values):

```
H        = "\nVerified reference code from earlier tasks, most relevant first.\n"            (66 bytes)
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
- `length-matched` (preregistered ablation: "C with more raw solutions, length-matched").
  `--target-bytes` is the other arm's realised `suffix_bytes`, normally D's at the same k and cap, and must
  be <= `--cap-bytes`. First the `normal` walk; call its suffix length U (`unpadded_bytes`).
  - If U >= target: nothing is added. The receipt shows `pad=none` and `pad_reason=c_not_shorter`, and the
    selection and suffix are exactly `normal`'s.
  - If U < target: walk the records logged `beyond_k` or `cap_bytes` in rank order (rank k+1, k+2, ...).
    Skip any whose block at the next position (with H if nothing is selected yet) would push the suffix
    past `--cap-bytes`; a skipped record keeps its reason. Append the others whole, with role `pad`. Stop
    as soon as the suffix length is >= target (`pad_reason=reached`), or when the candidates run out
    (`pad_reason=exhausted`, still short of the target). `pad` = the number appended, or `none`.
  - The receipt always gives `unpadded_bytes`, `target_bytes`, the final `suffix_bytes` and
    `length_gap` = suffix_bytes - target_bytes, signed. When reached, `length_gap` is the overshoot (>= 0,
    smaller than the last pad's block). For c_not_shorter it is >= 0. For exhausted it is < 0.
  - Known property, by design: the pads are C's next-most-relevant raw records. Padded C therefore gets
    more records and more on-topic content, not only more bytes. Its gain cannot be put down to length
    alone. That bias favours C, which keeps a "D still wins" result conservative. The length-neutral
    control is `irrelevant`.

## 7. Fatal (exit 2, one stderr line `genret: FATAL <reason> <detail>`, no output)

- `usage`, `manifest_unreadable`, `manifest_malformed`, `manifest_digest_mismatch`
- `record_missing`: a listed file is absent or unreadable
- `stale_digest`: record bytes differ from the manifest digest
- `invalid_record`: recheck refused the record, or genret cannot read a field it needs
- `recheck_failed`: recheck is missing, crashed, or its output does not match the records
- `recheck_untrusted`: the `--recheck` file's sha256 is not `--recheck-sha256`
- `duplicate_id`, `requirement_invalid`, `prompt_invalid`, `hidden_path`, `out_exists`, `write_failed`
- `internal`: out of memory, or an internal consistency check failed

A record is never replaced by another one: integrity failures stop the run.

## 8. Outputs in `--out`

| file | content |
|---|---|
| `selected.tsv` | header `rank\tid\tdigest\tscore\tbytes\trole`; one row per selected record in suffix order; role `top` or `pad` |
| `excluded.tsv` | header `id\tdigest\tscore\treason`; every manifest record not selected, sorted by id (byte order); score `-` when not indexed |
| `suffix.txt` | the exact bytes the runner appends in the variable suffix (may be empty) |
| `query.txt` | the query terms (section 3) |
| `receipt.txt` | `key=value` lines in a fixed order (listed below) |
| `SHA256SUMS` | `sha256sum` lines of the five files above, in that file order |

`receipt.txt` keys, in this order:
1. `contract=genret-cli-v3`
2. `mode`
3. `kind`
4. `k`
5. `cap_bytes`
6. `target_bytes` (`-` if unused)
7. `exclude_family` (comma list or `-`)
8. `manifest_sha256`
9. `recheck_sha256`
10. `recheck_source_sha256`
11. `requirement_sha256`
12. `prompt_sha256`
13. `query_sha256`
14. `records`
15. `indexed`
16. `total_tokens` (T)
17. `selected`
18. `pad` (`-` unless length-matched; else `none` or the count of pad records)
19. `pad_reason` (`-`, `c_not_shorter`, `reached` or `exhausted`)
20. `unpadded_bytes` (`-` unless length-matched)
21. `suffix_bytes`
22. `length_gap` (`-` unless length-matched; else signed decimal)
23. `suffix_sha256`
24. `selected_sha256`
25. `excluded_sha256`

Digests are lowercase hex sha256. No timestamps, paths or host data in any output.
