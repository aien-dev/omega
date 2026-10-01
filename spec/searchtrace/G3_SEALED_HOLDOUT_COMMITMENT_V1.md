# G3 Sealed-Holdout Commitment Format V1

Status: format only. No training, no real held-out data. Implementation:
`src/searchtrace/st_holdout.{h,c}`; tests: `tests/searchtrace/test_st_holdout.c`.

## Purpose

A held-out task set is committed to publicly before any learner runs. The
task-set bytes and a secret 32-byte salt stay sealed. A later reveal proves
the revealed set is byte-for-byte the committed one. The salt prevents
brute-force confirmation of guessed task sets from the public record.

## Inputs

- `taskset`: opaque bytes (canonical task-set file). Only rule applied:
  `task_count` = number of lines (LF-separated; final line may lack LF) whose
  first five bytes are `task ` (ASCII, lowercase, one space). A task set with
  zero such lines is refused.
- `salt`: exactly 32 bytes, generated from a CSPRNG, kept sealed until reveal.
- `holdout_id`: label matching `[A-Za-z0-9._-]{1,64}`.

## Commitment

    commitment = SHA-256( "omega.g3.holdout.commit.v1" 0x00
                          || salt[32]
                          || u64be(taskset_len)
                          || u32be(task_count)
                          || taskset )

    salt_digest = SHA-256( "omega.g3.holdout.salt.v1" 0x00 || salt[32] )

Choices: domain strings are NUL-terminated so no prefix ambiguity; length and
count are fixed-width big-endian so the preimage is unambiguous; the salt
digest is domain-separated from the commitment so it cannot be confused with
it. Publishing `salt_digest` lets the reveal check the salt first and give a
specific refusal; it leaks nothing useful because the salt is 256 random bits.

## Record (canonical ASCII, LF only, fixed order, no trailing spaces)

    OMEGA-G3-HOLDOUT-COMMIT v1
    domain omega.g3.holdout.commit.v1
    holdout_id <label>
    task_count <decimal, 1..4294967295, no leading zero>
    taskset_len <decimal, u64, no leading zero>
    salt_digest <64 lowercase hex>
    commitment <64 lowercase hex>
    end <64 lowercase hex>

`end` = SHA-256 of every record byte before the `end ` line (including the
LF of the `commitment` line). The record ends with exactly one LF after the
end hex; nothing follows. The file is content-addressed:
`g3-commit-<sha256 of the whole record, lowercase hex>.txt`.

## Parser refusals (`st_holdout_parse`)

Wrong magic or version; wrong domain; missing, extra or reordered field; hex
not 64 chars, non-hex char, uppercase hex; bad label; zero tasks; decimal with
leading zero, non-digit or out of range; `taskset_len < 5 * task_count`
(each counted line holds at least `task `); truncated record (no end line, or
line without LF); end digest mismatch; trailing bytes after the end line; any
CR byte; any NUL byte; record longer than 1024 bytes.

## Reveal (`st_holdout_reveal`)

Checks in order, refusing on the first failure: salt digest, task-set
length, task count, commitment. Digest comparisons are constant-time.

## Reveal receipt (`st_holdout_reveal_receipt`)

    OMEGA-G3-HOLDOUT-REVEAL v1
    domain omega.g3.holdout.commit.v1
    holdout_id <label>
    commitment <hex>
    record_digest <hex: sha256 of the commitment record file>
    task_count <n>
    taskset_len <n>
    repo_commit <40 lowercase hex>
    verdict PASS
    end <sha256 of the preceding receipt bytes>

Only PASS receipts exist; a failed reveal emits no receipt (the CLI prints the
refusal reason and exits 2). The function does not re-verify: callers must
get 0 from `st_holdout_reveal` first (the CLI does). A repo commit that is not
exactly 40 lowercase hex is refused. File name:
`g3-reveal-<sha256 of the whole receipt>.txt`.

## CLI (`st_holdout_cli`, argv[0] = subcommand)

- `commit <holdout_id> <taskset_file> <salt_file> <out_dir>`: salt file must be
  exactly 32 bytes; writes `<out_dir>/g3-commit-<digest>.txt`, prints the path.
  Same bytes already present: OK (idempotent). Different bytes: refused.
- `reveal <record_file> <taskset_file> <salt_file> <out_dir>`: needs
  `OMEGA_REPO_COMMIT` (40 lowercase hex); verifies, writes the receipt, prints
  the path.
- `verify <record_file>`: parse only.

Exit codes: 0 ok, 2 refusal (including I/O failure), 1 usage. Files are
written to a temp name, fsynced, then hard-linked into place (never
overwritten).

## Known-answer vector

salt = bytes 0x00..0x1f, taskset = `task alpha\ntask beta\n` (21 bytes, 2
tasks), holdout_id = `g3-kat-1`. Computed independently with `printf` +
`sha256sum`:

- salt_digest `731be661741f6bbf7b8ed2dc8150a6a35238d7b133817bdf1347415bff4ec4a4`
- commitment `7008574e68b49d88b0debe5101eec7823a0f11a99833cb5e6aedc9262822b20d`
- end `f241a91082e4ee19e4612c4fb7a3c6108182449c515bee35be6797bd8ef387ee`
- record digest `20ba763127e9ffd0b563365d1db13f7d3b813f39c9d3b215c1447a48e9f39a28`

## Out of scope

Salt generation, sealed storage of the task set and salt, signing the record,
publishing it to a timestamped location, and any real held-out data.
