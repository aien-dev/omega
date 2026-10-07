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

Single vs batched parity and repeat (L6-KV, 2026-10-05): for the 3-sequence batch (padded
table entries, an empty context, a context truncated by its table) and both shared-prefix
batches, each sequence is run alone through `omega_gpu_paged_attention_bf16` with the batch's
rules applied by hand (negatives removed, ctx <= 0 -> 0) and must equal its batched row bit for
bit; then the batch is called again and must be a kernel cache hit, bit-identical, with the same
staging counters. A throwaway run comparing each sequence against the neighbouring row failed
all 11 single-call checks (the check has teeth). Host simulator: 122 checks (was 108).

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

- head_dim 64 (TinyLlama, Llama-3.2-1B) on chip; head_dim 128 (Qwen3-4B) is host-simulator only, see
  the head_dim 128 section below. Other head dims are refused (TOO_LARGE).
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

## head_dim 128 (2026-10-07, CPU phase: GB10 parity NOT_RUN)

Qwen3-4B: 32 query heads, 8 KV heads (GQA 4:1), head_dim 128. head_dim is now a kernel
parameter (`KSpec.hd`, 64 or 128) instead of a constant; the head_dim 64 program is the same
bytes as before (see Evidence). Status: host battery, host IR simulator and host warp
simulator PASS at head_dim 128; nothing has run on the chip.

### Where 64 was assumed (omega main c0369e6, before this change) and what changed

| Assumption | File:line (c0369e6) | head_dim 128 |
|---|---|---|
| `HD = 64`, threads per CTA = head_dim | `omega_gpu_attention_api.h:76`, `.c:215`, `.c:612` (launch `threads_x`), `.c:627` | `threads_x = KSpec.hd`; QMD CTA width 128 |
| head_dim refused unless 64 | `.c:663`, header comments `.h:43`, `.h:70` | 64 and 128 accepted, anything else TOO_LARGE |
| shared layout 4 x 256 B: q 0, s/p 256, token offsets 512, warp partials 768 | `.c:216-219`, `.c:240-242`, `.c:286` | 4 x 512 B: 0, 512, 1024, 1536 (`4hd`, `8hd`, `12hd`) |
| QMD shared size 1024 (default) | `omega_blackwell_qmd.c:117` (`shared_bytes == 0` gives 8 x 128 B), session launch `omega_gpu_session.c:167` | launcher passes `shared_bytes = 16 * hd`: 1024 (same QMD word as before) or 2048 |
| 2 warps: partials read at 768 and 896 (`c768`, `c896`) | `.c:241-242`, `.c:381`, `.c:401` | 4 warps: partials of warps 2 and 3 at +256 and +384, two extra LDS and two extra FMNMX / FADD per reduction (emitted only for hd 128) |
| chunk = 64 tokens: `base += 64`, phase B loop bound 64 (`c64`) | `.c:241`, `.c:423`, `.c:428` | `c64` holds `hd` (chunk size) |
| dim-pair inner loop bound 32 (`c32`) | `.c:241`, `.c:361` | `c32` holds `hd / 2` |
| q / out row stride `HD * 4` | `.c:253`, `.c:261`, `.c:688`, `.c:705`, `.c:730` | `head_dim` argument |
| score scale log2(e) / sqrt(64) | `.c:658`, `.c:705`, `.c:814` | `score_scale(head_dim)` |
| paged layout must have head stride `HD * 2` | `.c:726` | `head_dim * 2` |
| kernel cache key lacks head_dim | `.c:528` | key includes `hd` |
| per-thread / warp simulators: 64 threads, 1024 B shared | `tests/gpu_attention_test.c:535-536`, `:792` | sized from `--hd`; shared bound is the QMD declaration (16 * hd) |
| Not 64-dependent, unchanged | 32-lane SHFL tree (`.c:376-378`), 16 argument words, `MAX_CTAS 64`, `MAX_Q_HEADS 64`, GPR budget 64 | Qwen3-4B 32 heads: 2 sequences per launch |

### Limits for one head_dim 128 CTA (GB10, SM 12.1)

| Resource | Need | Limit | Source |
|---|---|---|---|
| Shared memory per block | 2048 B | SM 12.x SMEM capacity 0 to 100 KB per SM; 49152 B per block is shown for a CC 10.0 example | CUDA Programming Guide, Compute Capabilities appendix (Context7 `/websites/nvidia_cuda_cuda-programming-guide`): "Compute Capability 12.x has 128 KB of Unified Data Cache and SMEM capacities from 0 KB up to 100 KB"; "sharedMemPerBlock (49152 bytes)" (printed for CC 10.0; the 12.x per-block row was not returned: DOCS SILENT on the exact 12.1 per-block figure) |
| Registers per block | 128 threads x 64 GPR = 8192 | 64K 32-bit registers per block, 255 per thread | same appendix: "The maximum number of 32-bit registers per thread block is 64K, and per thread is 255" |
| Threads per block | 128 | DOCS SILENT (the retrieved snippets give 2048 threads per SM, not the per-block cap) | 64 threads already run on the chip (receipt FB1-CUT5-cd80bf4); QMD field is 16 bits wide (`clcec0qmd.h:248`, `NVCEC0_QMDV05_00_CTA_THREAD_DIMENSION0 MW(1103:1088)`) |
| QMD shared field | 2048 B = 16 units of 128 B | field is 11 bits, `NVCEC0_QMDV05_00_SHARED_MEMORY_SIZE_SHIFTED7 MW(1162:1152)` (`clcec0qmd.h:254`); MIN/MAX/TARGET SM_CONFIG fields (`:255-257`) keep the value omega already sends (word 36) | the header gives field widths only: DOCS SILENT on what SM_CONFIG code 9 allows; UNKNOWN until the chip run (`OMEGA_BW_QMD_MAX_SHARED_BYTES` 16384 is omega's own bound) |
| BAR.SYNC with 128 threads | one barrier, all CTA threads | QMD declares 1 barrier (`omega_blackwell_qmd.c:112`) | INFERRED: since Volta the barrier counts arriving threads (see `tests/bw_warp_sim.h` header); not chip-verified for sm_121 |

### Evidence (CPU only, 2026-10-07, omega main c0369e6 + this change)

- head_dim 64 byte identity: the SHA-256 of the encoded kernel (and instruction count) for six
  shapes (bf16 and f32 KV, log2 gqa 3, 2 and 0) was recorded from a build of c0369e6 and is
  pinned in `tests/gpu_attention_test.c` (`k_pins64`); `--host-only` checks all six. Pin
  negative control: a flipped digest bit must not match.
- head_dim 128 encodes: 168 instructions (160 at head_dim 64), 48 GPRs (46), decodes with
  nvdisasm SM121 with no undecodable instruction and the expected instruction set.
- `./build/gpu_attention_test --hd 128 --sim` runs the whole battery (f32 contiguous and paged
  bf16 and batched, Qwen3-4B 32q/8kv and 32q/4kv, contexts 1 to 2048 with 127/128/129 around a
  chunk, large scores, MHA, shared-prefix dedupe, every mutant) through the per-thread
  simulator against the f64 oracle at the same tolerance as head_dim 64; `--hd 128 --divergent
  --sim` runs it through the warp simulator (BSSY/BSYNC regions, both fragment orders).
- New head_dim 128 checks (`hd128_checks`): uniform scores (sum over four warp partials) and a
  dominant token above the f32 exponent range in each of the four warps and in later partial
  chunks (max over four warp partials). Mutation test (not committed): removing the two extra
  FMNMX fails the dominant-token check (1024 wrong outputs); removing the two extra FADD
  fails the uniform and the random-data cases.
- Shared-memory negative control: the simulators run the head_dim 128 kernel under a 1024 byte
  declaration and must stop (error 3, "shared access past the declared size").

### GB10 parity plan (later, coordinated window; NOT_RUN)

See the pull request body for the commands, shapes, tolerance and hold estimate.
