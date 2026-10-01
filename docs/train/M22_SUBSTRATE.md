# M22 optimizer substrate and E5 training provenance (Lane 21)

Status: **M22 NOT QUALIFIED.** This is the transactional parameter-state
substrate and an SGD path over plain byte buffers. There is no tensor or
autodiff integration yet; that waits for M20 OMEGA_TENSOR and M21
OMEGA_AUTODIFF. Plan reference: aien-architecture `CURRENT_EXECUTION_PLAN.md`
sections E4 (M22) and E5 (training provenance).

## What exists

| Piece | File | Plan item |
|---|---|---|
| Generation store (shadow, validate, one atomic switch, recovery) | `src/train/tg_store.[ch]` | E4: shadow state, validation before one atomic switch, OLD or NEW |
| SGD updater (float32, optional heavy-ball momentum) | `src/train/tg_sgd.[ch]` | E4: SGD first |
| Two-tier provenance | `src/train/tg_store.[ch]` | E5 |
| Exit tests, plain + ASan/UBSan | `tests/train/test_train.c`, `make test-train` | E4/E5 exit gate (substrate level) |
| Receipt | `tests/train/train_receipt.sh`, `make train-receipt` -> `evidence/M22/receipts/` | |

## On-disk layout (one directory per store)

- `gen-<20 digits>.bin`: one immutable generation. 152-byte little-endian
  header (magic `OMTRGEN1`, version, generation, step, parameter bytes,
  optimizer bytes, dispatch-log length and chain head at commit, previous
  generation digest, digest) followed by parameter bytes and optimizer bytes.
  Digest = SHA-256 over the first 120 header bytes + parameters + optimizer.
- `CURRENT`: the pointer, exactly 95 bytes: `OMTRCUR1 <generation> <digest hex>\n`.
  A reader checks the generation file's digest against the pointer.
- `dispatch.log`: tier (a) records, 192 bytes each, hash-chained.
- `refusals.log`: one line per refused operation (`REFUSE <code> gen=<g> <why>`).
- `LOCK`: `flock` for the single writer.

## Commit protocol

1. Refuse if the shadow is not open (double switch), its base is not the
   current generation (stale), or sizes differ. Run the validation callback;
   refuse on reject. Every refusal is appended to `refusals.log`.
2. Append the shadow's dispatch records to `dispatch.log`, chained, `fsync`.
3. Compute the full state digest (tier b; the only place the model is hashed).
4. Write `shadow-N.tmp`, `fsync`, read it back and verify the digest.
5. `rename` to `gen-N.bin`, `fsync` the directory.
6. Write `CURRENT.tmp`, `fsync`, `rename` to `CURRENT` (the one atomic
   switch), `fsync` the directory.

Recovery (`tg_open`): verify the generation `CURRENT` names (size, digest,
pointer digest), delete `*.tmp` and generation files newer than `CURRENT`,
truncate `dispatch.log` to the committed length and verify its chain against
the committed head. A generation that does not verify is refused, never
silently rolled back.

## Provenance tiers (E5)

- (a) Per dispatch: step, op id, two scalar arguments (SGD: learning rate and
  momentum bit patterns; gradient reference id), up to three input references
  (buffer id, offset, length) and the base generation's digest, by reference.
  Cost is one SHA-256 over 192 bytes per record, independent of model size
  (tested at 1 Ki and 256 Ki parameters).
- (b) Full parameter + optimizer digest, only at committed generations,
  linked by previous-digest into a verifiable history (`tg_verify_history`).

## What the tests prove (host, single core)

- Crash at each of 8 injected points inside commit (child process `_exit`):
  points before the pointer rename recover to exactly OLD; points after it
  recover to exactly NEW; digests verified; redoing the step from OLD gives
  the same NEW digest. Torn-file variants (not-yet-current files truncated
  after the crash) also recover to OLD.
- A lock-free reader racing 150 commits never sees a torn or unverified state
  and never sees the generation go backwards.
- Refusals: corrupted shadow, truncated generation (payload and header),
  payload digest mismatch, pointer digest mismatch, validation reject, double
  switch, stale shadow, tampered and truncated dispatch log, second writer,
  re-create, bad SGD arguments.
- Replay: the same 25-step SGD sequence from G0 gives a byte-identical G25
  digest and dispatch chain head, in two stores and across the -O2 and
  ASan/UBSan -O1 builds; each committed step re-derived from its base
  generation plus its dispatch record matches the committed payload byte for
  byte.
- SGD (plain and momentum) cuts a fixed quadratic loss by more than 10x in
  40 committed steps.

## Not done / limits

- **Not a power-loss test.** Faults are process crashes; `fsync` ordering is
  by construction and code review, not by cutting power.
- **Gradients are hand-written** (quadratic loss). No autodiff (M21), no
  tensor API (M20); no GPU path.
- **Adam/AdamW deferred** (plan E4: "SGD first; Adam/AdamW after"). The
  optimizer state is opaque bytes, so they fit the same substrate later.
- Reproducibility is claimed for the same binary and for the two test builds
  on this host (AArch64, `-ffp-contract=off`), not across ISAs or compilers.
- No garbage collection of old generations (all are kept; history is
  verifiable end to end).
- Dispatch records are held with the shadow and written at commit, so work
  that is never committed leaves no tier (a) record.
- If the pointer rename succeeds but the directory `fsync` after it fails,
  the commit returns `E_IO`, readers already see NEW, and the writer refuses
  every further commit until the store is reopened (recovery decides).
  This path is not exercised by a test (it needs a failing `fsync`).
