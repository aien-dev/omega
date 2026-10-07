# omega#324 GB10 evidence, 2026-10-07 (session d6d82f, windows approved by merge control 3649dd)

Code under test: omega f1d57c5 (#324 head: c0369e6 + code commit e09adff + crumb recompile). Physics 6d7cf0d (physics.lock). Driver 580.173.02.
Nothing patched or rerun after the runs. Investigation record (sealed): ~/workspace/investigations/2026-10-07-hd128-ctx256-stall.

## hd64 regression on the #324 head (hd64-regression/), hold b 02:54:45Z-02:54:55Z
- tools/run_gpu_attention_chip.sh rc 0: run.log PASS 122/0, sweep PASS 73/0, timing PASS 2/0, sim.log PASS 122/0. device.txt: omega f1d57c5, 0 uncommitted paths. Kernel log for the hold: no Xid.
- Disclosure: session 711736 ran a CPU-only cargo compile 02:54:32Z-02:54:56Z, overlapping the whole hold. Parity is unaffected; the timing gate result is possibly CPU-contended.

## Red/green for the cause (red-green-probe/), hold a 02:44:25Z-02:54:27Z
Builds side by side, one variable: baseline = omega 9a20ca8 (omega#323 head, untouched); padded = probe commit c7663bb = 9a20ca8 + e09adff (this PR's code commit) only. Camera: va_camera.c (link-time wrap of nvrm_alloc / nvrm_alloc_gpu_uncached / nvrm_free; code buffer found by content; no change in the launch path). Sequence per process: hd128 gqa_f32 32q/8kv ctx 1, 17, 256.
- RED 1/1 (1-red-baseline.log): ctx1 rc 0, ctx17 rc 0, ctx256 rc -4 (CHIP_FAIL) after the 600 s host wait. Kernel log 02:44:26Z: Xid 31, name=va_camera_base, GPCCLIENT_GCC faulted @ 0x10_04134000, FAULT_PTE ACCESS_TYPE_VIRT_READ. Camera: code_va 0x1004133000, alloc 0x1000 (1408 B after the 2688 B of code); the page after it (0x1004134000 = the fault address) was mapped by another buffer after ctx1 and unmapped after the ctx17 scratch regrowth.
- GREEN 5/5 (2-green-padded-1..5.log): ctx 1, 17, 256 all rc 0, unwritten 0; ctx256 max_abs_err 2.25e-7 vs an f64 CPU reference; code alloc 0x2000, code_end+2047 mapped.
- hd128 full battery on the probe build (3-hd128-padded.log): PASS 126/0. This is the probe, not omega#323's final head.
- Kernel log for hold a (5-kernel-log.txt): exactly one Xid, the baseline's.

## Claims
- OBSERVED: with the unpadded code buffer, the hd128 kernel faults reading the page after its code when that page is unmapped; with the 2 KB tail it does not (1/1 vs 5/5, same process sequence, one variable).
- DOCUMENTED (secondary): Mesa NVK src/nouveau/vulkan/nvk_device.c: "the I-cache pre-fetches and NVIDIA has informed us overallocating shaders BOs by 2K is sufficient".
- INFERRED: the reader is the instruction prefetcher (GCC client). DOCS SILENT on which unit GPCCLIENT_GCC is and on the QMD PROGRAM_PREFETCH_SIZE unit.
- LIMITS: one red trial is not a rate; 5/5 green is not a rate either; hd64 kernels were never observed to fault (same latent gap, fixed here too).
