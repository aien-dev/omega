# FB-1 cut 5: native gqa + paged attention (no CUDA)

Status: host battery PASS, host IR simulator PASS (45/45), chip gate PASS (45/45, receipt ~/workspace/evidence-out/FB1-CUT5-b016322, PR #261).
Files: `src/omega_gpu_attention_api.{h,c}`, `tests/gpu_attention_test.c`,
`tools/run_gpu_attention_chip.sh`. Library: `libomega_gpu.a`.

## DOCS READ (recorded with `docs-read omega ...` before the codegen edits)

- `clcec0.h` (mesa copy `~/workspace/.ref/mesa/src/nouveau/headers/nvidia/classes/clcec0.h:262`):
  `NVCEC0_INVALIDATE_SHADER_CACHES 0x021c`; bits 0 (instruction), 4 (data), 12 (constant)
  are set before every launch, as cut 1b found necessary when code addresses are reused.
- `nvos.h` ~1112: `NVOS32_ATTR2_GPU_CACHEABLE_YES` "for system memory this will not be
  coherent with direct CPU mappings" -> every buffer the host reads back or the chip reads
  after a host write is `nvrm_alloc_gpu_uncached`.
- `clcec0qmd.h:252` `NVCEC0_QMDV05_00_SHARED_MEMORY_SIZE_SHIFTED7 MW(1162:1152)` and
  `src/omega_blackwell_qmd.c` word 36 (`0x04b44808`): the QMD declares 1024 bytes of shared
  memory and one barrier. This is why head_dim is fixed at 64 in this cut (below).
- PTX ISA `ex2.approx.f32` (max 2 ulp over the full range) and the bf16 definition (top 16
  bits of an f32): bf16 -> f32 is a 16-bit shift, done with IMAD x * 0x10000 (low half) and
  LOP3 AND 0xffff0000 (high half of a 32-bit pair).

## Oracle (aien-sovereign-core `crates/aien-inference-abi/src/backend.rs`, main 8b5caed)

- `gqa_attention`: f32 KV `[seq][num_kv_heads][head_dim]`, query head h reads kv head
  `h / (num_q_heads / num_kv_heads)`, score = q.k / sqrt(head_dim), softmax over the whole
  history (decode: one query, no mask), seq_len 0 -> zeros, `inv = 1 / max(sum, 1e-12)`.
- `paged_attention`: bf16 pool in aien-kv-cache `KvLayout` order (block, layer, plane K=0 V=1,
  token, head); token t is `block_ids[t / block_size]` slot `t % block_size`; tokens whose
  block index is past `block_ids` are dropped; context 0 or no blocks -> zeros.
- `paged_attention_batch`: `context_lens[s] <= 0` -> 0; the row is
  `block_tables[s*max .. (s+1)*max]` (empty if it runs past the array); negative entries are
  removed and the rest close up.

Documented deviation: a block id `>= layout.num_blocks` is refused with `BAD_ARGS`; the
oracle reads zeros for it. A layer index past `num_layers` is refused the same way.

## Kernel

One CTA per (query head = CTAID.X, sequence = CTAID.Y), 64 threads, grid
`(num_q_heads, sequences per launch)` with at most 64 CTAs per launch (I42 envelope).
Shared memory map (1024 bytes): `[0,256)` q, `[256,512)` scores then probabilities,
`[512,768)` per-token byte offsets, `[768,1024)` warp partials.

The context is walked in chunks of 64 tokens with online softmax:

1. Phase A, thread = token: block-table lookup, `K.q` over 64 dims in f32 FFMA (K read as
   32-bit pairs with LDG.E: two f32 dims or two bf16 dims per load), `s = dot * scale` with
   `scale = log2(e) / sqrt(64)` folded in so EX2 gives e^x.
2. Chunk max: SHFL.DOWN tree per warp, two warp partials through shared memory, BAR.SYNC.
   `alpha = EX2(m_old - m_new)`, `p = EX2(s - m_new)`, chunk sum the same way,
   `L = L * alpha + sum`, `acc *= alpha`.
3. Phase B, thread = dimension: `acc += p_t * V[t][tid]` over the chunk's tokens (V bf16 by
   LDG.E.U16, f32 by LDG.E).
4. Epilogue: `out = acc * RCP(L)`. The first chunk starts with `m = -inf`, so `alpha = 0`
   wipes the initial `L = 1`, `acc = 0`. Context 0 jumps straight to the epilogue (zeros).

Both kernels are 152 instructions, 42 GPRs, 6 UGPRs (budget 64 GPRs). Kernels are
specialised on `{kv f32 or bf16, log2 block size, log2 gqa ratio}` and cached (8 slots).
The gqa path stages `[K plane | V plane]` as one 4096-slot block, so context <= 4096 there.
The paged path stages the referenced blocks' layer slice renumbered in table order; the
kernel itself walks the (renumbered) block table.

## Tolerances

- Standard cases: `2e-4 * |want| + 2e-5`. Budget: f32 sums over <= 2048 terms
  (~1.2e-4 relative), MUFU.EX2 (2 ulp), MUFU.RCP (1 ulp), bf16 inputs are exact in f32.
  Observed worst in the simulator (exact EX2/RCP): 0.0033 of the tolerance.
- Large-score case (q scaled by 200, scores ~ +-200 natural): `1e-3 * |want| + 1e-4`.
  Observed worst in the simulator: 0.041 of the tolerance.

## Mutants (deliberately wrong kernels the test must catch)

| mutant | what it breaks | where it shows |
|---|---|---|
| NO_MAX | no running-max subtraction | only when EX2 overflows: the large-score case (softmax is shift invariant on unit-scale data) |
| KV_HEAD | reads kv head `(h / ratio) ^ 1` | every context length including 1 |
| SLOT | K read from slot `(t + 1) % block_size`, V from slot t | every context >= 2 (with one token the softmax weight is 1 whatever K says). Shifting both K and V would only permute matched pairs inside a full block, which softmax cannot see, so the mutant is K-only |
| NO_RESCALE | accumulator not multiplied by alpha when the max moves | context > 64 (more than one chunk) |
| Q_ROW | q loaded from query-head row `h ^ 1` (kv head and output row stay `h`): a wrong query-head-row address | every context (heads h and h^1 come out swapped); needs num_q_heads even |
| OUT_ROW | the correct result for head `h` stored in flattened output row `h ^ 1`: a wrong output-head slot (merge order) | every context; needs num_q_heads even |

The enum ends in `OMEGA_GPU_ATTN_MUTANT_COUNT`; every battery (host-only digests, simulator,
chip, JSON `mutants` summary) enumerates `1 .. COUNT-1` and fails if any mutant never ran or
was ever missed, so a mutant added to the enum without a battery entry fails the gate. Host-only
also checks that every mutant's kernel digest differs from the baseline and from every other
mutant's. Names come from `omega_gpu_attention_mutant_name` (table checked against COUNT).

Negative control: the comparison must reject the oracle with kv heads swapped. Repeat run
on a cache hit must be bit-identical.

## Staging dedupe per launch (OM-2)

The paged path stages the referenced blocks' layer slice into a GPU-uncached staging buffer.
Within one batched launch the staged slot is now keyed by the physical block id: a block that
several sequences reference (a shared prompt prefix) is copied once and every row of the
renumbered block table points at the same slot. `OmegaGpuAttnInfo` reports the effect:
`kv_blocks_logical` (table entries walked), `kv_blocks_unique` (slices actually copied),
`kv_bytes_staged` (bytes copied) and `kv_bytes_naive` (what per-reference staging would have
copied); `kv_source` is `OMEGA_GPU_ATTN_KV_STAGED` (0) in this cut, there is no resident KV
(see "Limits"). `omega_gpu_attention_test_set_dedupe(false)` restores per-reference staging
as a negative control; the test proves the legacy counters equal the naive numbers and that the
deduped output is bit-identical to the legacy output. Simulator evidence, 4 sequences sharing
10 scattered prefix blocks with private tails {1,2,3,3}: 8q/2kv (one launch) 19 unique of 49
logical, 155648 of 401408 bytes; 32q/8kv (two launches, dedupe is per launch) 29 of 49,
950272 of 1605632 bytes.

## Head and kv-head scaling sweep (`gpu_attention_test --sweep --out <json>`)

q heads {1,2,4,8,16,24,32,64} x every divisor as kv heads x context {256, 2048}, bs 16,
scattered block ids with a shared prefix across 4 sequences. Non power-of-two ratios (the 24-head
rows) must be refused with `BAD_ARGS` and are recorded as `unsupported_by_kernel_envelope`.
Supported points: oracle parity on the first call, then N timed calls (20 on chip, 1 under
`--sim`), recording median/p90 call and kernel milliseconds, launches, CTAs, threads per CTA,
q/out widths and the staging counters above. Schema `OMEGA_GPU_ATTENTION_SWEEP_V1`; the file
says `host_simulator` and that hardware counters are unavailable (no invented counters). The
sweep asserts no timing thresholds; the 3 ms gate stays in `--timing`.

## Host IR simulator (`gpu_attention_test --sim`)

Executes the allocated IR program (physical registers, so the register allocator's loop
handling is under test) for every CTA: 64 threads, 1 KB shared, host pointers as addresses,
SHFL and BAR as lockstep points. It found the one kernel bug before chip time: the kv-head
byte offset was added to the V base twice (once inside the per-token offset phase A leaves
in shared memory, once in the V column base), so every head mapped to a kv head other
than 0 read V from the wrong head. It also found a kernel-cache key compared with memcmp
over struct padding (cache misses at random). `ATTN_SIM_TRACE=<ctaid.x>` prints thread 0
of that CTA instruction by instruction.

## Limits of this cut

- head_dim 64 only (TinyLlama, Llama-3.2-1B). Larger heads need a bigger shared-memory
  declaration in the QMD (word 36) and a second thread-per-token tiling.
- Staging copy of q, the referenced KV blocks (once per physical block per launch, OM-2) and
  the tables per call. No resident KV: the pool lives in an anonymous CPU mmap whose
  "device address" is the CPU virtual address, not a GPU VA this channel can address, and
  GPU-cached system memory is not coherent with CPU mappings (nvos.h), so a resident copy
  needs an Omega-owned GPU-uncached mirror plus exact block generations from the pool.
- Serialising instruction schedule (every instruction waits on everything pending), as in
  cut 4: correctness first, throughput later.
- Context <= 4096 on the contiguous gqa path.
- UNVERIFIED on chip until the gate run: forward predicated `@P0 BRA`, bf16 K loads as
  32-bit pairs (the 16-bit widening itself is cut 4's), one launcher opening the device
  while another API in the same process holds it.
