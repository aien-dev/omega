# Decode-graph performance receipt (`omega-decode-graph-receipt/1`)

Lane P1 of aien-architecture#233. `tools/dgraph_receipt.c` turns one existing run of the decode-graph
chip test into one machine-readable receipt bound to the commit that was measured, and checks such
receipts. It reads files only: no GPU, no `libomega_gpu`, no new counters. This change does not touch
GPU numerics, barriers, prefetch, submission, memory ownership or any hot path: it only reads the
numbers that `omega_gpu_decode_graph` and `gpu_decode_graph_test` already produce.

## Where the numbers come from

| receipt field | source | clock |
|---|---|---|
| `per_token.graph.launches` | `OmegaGpuDecodeGraphStats.launches`: "launches recorded for the token" | count |
| `per_token.graph.submits` | `.submits`: "submissions (1 normally; = launches with flush_each)" | count |
| `per_token.graph.chip_ns` | `.chip_ns`: "sum of the list submissions' first-submit-to-last-marker time" | **host** |
| `per_token.graph.record_ns` | `.record_ns`: "host: begin_token to the submit (recording)" | host |
| `per_token.graph.total_ns` | `.total_ns`: "host: begin_token to end_token return" | host |
| `per_token.graph.sems_done` | `.sems_done`: "launch semaphores found DONE" (not written by the test: `null` + note) | count |
| `per_token.graph.host_overhead_ns` | derived: `total_ns - chip_ns` | host |
| `stamped_token.span_ns`, `stages[].device_ns` | `omega_gpu_decode_graph_stamps()`: "Launch i took stamps[i + 1] - stamps[i] ns" | **device** (PTIMER) |
| `stamped_token.chip_ns` | the stamped token's `.chip_ns` | host |
| `stamped_token.unattributed_ns` | derived: `chip_ns - span_ns` | host minus device |
| `per_token.flush_each.*` | the same list submitted one launch per submission (TEST ONLY `set_flush_each`) | as above |

Quotes are from `src/omega_gpu_decode_graph.h`. The stats are copied from the `--out` JSON of
`tests/gpu_decode_graph_test.c`; the run's barrier, prefetch and cached-weights settings from its log
("dgraph prefetch" and "fp8 knobs" lines); commit, dirty flag, host and GPU from the runner's
`device.txt`; binary identity from `binaries.sha256`.

### Host clock vs device clock

- **`chip_ns` is host time, not device time.** `omega_gpu_cmdlist_submit()` reads the platform clock
  (`omega_gpu_session_now_ns()`, `CLOCK_MONOTONIC` on Linux, `omega_gpu_platform_linux.c`) just before
  `queue_submit` and again after both completion-marker words were seen and synced. It holds the device
  work plus the submit call, the front-end start and the host's marker poll.
- **The device clock appears only with stamps on.** `set_stamps(1)` writes a GPU PTIMER timestamp
  before the first launch and after every launch, "taken after its WAIT_FOR_IDLE"
  (`omega_gpu_cmdlist.h`). The test runs one extra token this way; its `span_ns` is device time and
  always fits inside that token's host `chip_ns` (the checker enforces `span_ns <= chip_ns`).
- `unattributed_ns = chip_ns - span_ns` is the part of the host submit span the device stamps do not
  cover.
- `host_overhead_ns = total_ns - chip_ns`: host work outside the submit span (recording, the attention
  counter check, output sync, token read, lock).

### Serial vs overlapping time

- `chip_ns` is a serial sum: one host span per submission, and submissions never overlap. A graph
  token is one submission; a `flush_each` token is one per launch.
- With prefetch off, every barrier mode except the TEST ONLY `none` puts a WAIT_FOR_IDLE before each
  stamp, so launches run one after another. The stage times are a serial partition of `span_ns`: they
  add up to it exactly (the checker enforces `stage_sum_ns == span_ns`).
- With prefetch on (`AIEN_OMEGA_DGRAPH_PREFETCH=1`), each layer adds one o_proj prefetch launch marked
  independent (no barrier before it) and tagged `attn`. Its stamp is the front-end time while the
  attention may still be running. So the `attn` stage is the attention-plus-prefetch window. Overlap
  is **visible but not measurable**: no counter splits that window.
- `overlap_measurable` is `false` in every receipt this tool writes, and the `reason` field says why.

## Statuses

`VERIFIED | EXPERIMENTAL | HISTORICAL | NOT_RUN | FAIL | SUPERSEDED`. Receipts rebuilt from old
evidence logs are **HISTORICAL**: the chip was not run to make them. `VERIFIED` needs a clean tree and a
PASS run. `EXPERIMENTAL` needs a PASS run. `NOT_RUN` carries `per_token: null`, `stamped_token: null` and
a `not_run_reason`, so it holds no numbers.

## Making a receipt from a run

From the repo root, after `make dgraph-receipt`:

```
build/dgraph_receipt/dgraph_receipt \
  --run-dir evidence/OWN30B-REQUAL-MAIN-710d438c-20261015/gates/decode_graph \
  --stats   evidence/OWN30B-REQUAL-MAIN-710d438c-20261015/gates/decode_graph/receipt-main.json \
  --log     evidence/OWN30B-REQUAL-MAIN-710d438c-20261015/gates/decode_graph/main.log \
  --status HISTORICAL --repo aien-spark/omega \
  --model "synthetic Qwen3-30B-A3B-shaped decode step, random weights" \
  --precision "FP8 e4m3 weights (qkv, o, gate_up, down), BF16 lm_head, f32 activations" \
  --mode "decode, one token per command list (omega_gpu_decode_graph_token), batch 1" \
  --runner "tools/run_gpu_decode_graph_chip.sh EVIDENCE_DIR main" \
  --out receipt.json
build/dgraph_receipt/dgraph_receipt --check receipt.json --evidence-root .
```

The generator runs the checker on its own output first and refuses to write a receipt the checker
rejects. It also refuses a log that lacks the run's settings and a stats file and log from different
runs. The exact commands behind the golden fixtures are in `tests/dgraph_receipt/run.sh`.

## What the checker rejects

Missing fields, wrong types, unknown top-level fields, a schema other than `/1`, a status outside the
enum, a commit that is not 40 lowercase hex digits, a non-`OWNER/NAME` repo, placeholder strings
anywhere (`""`, `TBD`, `unknown`, `TODO`, `N/A`, `none`, ...), zero tokens, derived numbers that do not
match their inputs (`host_overhead_ns`, `unattributed_ns`, `stage_sum_ns`), stages that do not add up to
`span_ns`, `span_ns > chip_ns`, `sems_done` neither equal to `launches` nor `null` with a note,
`VERIFIED` on a dirty tree, measurements on a `NOT_RUN` receipt, absolute or `..` evidence paths, and,
with `--evidence-root`, evidence paths that do not exist. `make test-dgraph-receipt` proves each rule
with a fixture in `tests/dgraph_receipt/fixtures/` (table: `tests/dgraph_receipt/negatives.txt`).

## Reproduced numbers (HISTORICAL)

These are the only numbers here. They are copies of existing evidence, not new claims:

- `tests/dgraph_receipt/fixtures/good-historical-710d438c-main.json`, from
  `evidence/OWN30B-REQUAL-MAIN-710d438c-20261015/gates/decode_graph/` (omega 710d438c, clean tree, GB10,
  2 layers, 20 tokens, prefetch off). Per-token medians: 22 launches in 1 submission, total 3 978 675 ns,
  chip (host clock) 3 975 395 ns, record 2 464 ns. Stamped token: device span 3 989 568 ns inside a host
  chip span of 4 076 853 ns.
- `tests/dgraph_receipt/fixtures/good-historical-710d438c-prefetch.json`, from
  `evidence/OWN30B-REQUAL-MAIN-710d438c-20261015/gates/l2d_prefetch/` run `4-cached-prefetch` (cached
  weights, prefetch on, 10 tokens, 24 launches).

## Limitations

- Synthetic step on random weights, one fixed position, the same token every rep: this is not a
  serving or end-to-end tokens-per-second figure.
- Per-token values are medians taken field by field, so `host_overhead_ns` is a difference of medians.
- The stamped token is one sample.
- No aggregate totals or percentiles: the test writes medians only.
- When `make` did not rebuild the test, `build.log` has no compile line. The receipt then says so and
  names the binary by sha256.

## Gaps (counters that do not exist; none added here)

- `sems_done` per token: the test's `--out` writer (`tests/gpu_decode_graph_test.c`) does not write it.
- The method-stream build time: `OmegaGpuCmdListStats.host_ns` is filled by `omega_gpu_cmdlist_submit()`,
  but `submit_now()` in `src/omega_gpu_decode_graph.c` does not add it to `OmegaGpuDecodeGraphStats`.
- Submit call vs wait inside `chip_ns`: `omega_gpu_cmdlist_submit()` times `queue_submit` through the
  marker poll as one span. Splitting them needs a clock read after `queue_submit` returns.
- Per-token distributions: `gpu_decode_graph_test` keeps the per-rep arrays but writes only
  `median_u64()` of each.
- Attention vs prefetch inside the overlapped window: with an independent launch, `omega_gpu_cmdlist_submit()`
  emits the stamp before it with no WAIT_FOR_IDLE, so no stamp marks the end of the attention alone.
