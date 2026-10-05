#!/bin/sh
# Source guard: every page the host POLLS for the resident seat's completion must be GPU-uncached.
# NVIDIA nvos.h NVOS32_ATTR2_GPU_CACHEABLE_YES: "For system memory this will not be coherent with direct CPU
# mappings"; spark-hardware-manual s4.1: "Any word the host polls ... must be allocated nvrm_alloc_gpu_uncached".
# Evidence (CAND-1 window 2 and the R15 seat A/B, 2026-10-05): with a GPU-cacheable marker page the seat's leave
# marker reached the host ~1.05 s late in 16 of 16 trials and never within the wait in 3 of 64 (~102 s stalls);
# with a GPU-uncached page it arrived in 13 to 20 ms in 16 of 16, no stall.
# The check reads src/runtime/rx_resident_gpu.c; mutants prove it can say no.
cd "$(dirname "$0")/.." || exit 1
F=${SEAT_SRC:-src/runtime/rx_resident_gpu.c}; fail=0
chk() { # name file expect(0=guard passes,1=guard must refuse)
    if grep -Eq 'nvrm_alloc_gpu_uncached\([^;]*&job->marker_mem\)' "$2" && ! grep -Eq '[^_]nvrm_alloc\([^;]*&job->marker_mem\)' "$2"; then got=0; else got=1; fi
    if [ "$got" = "$3" ]; then echo "[+] $1"; else echo "[-] $1 (guard exit $got, want $3)"; fail=$((fail + 1)); fi
}
W=$(mktemp -d) || exit 1; trap 'rm -rf "$W"' EXIT
chk "seat marker page is GPU-uncached" "$F" 0
sed 's/nvrm_alloc_gpu_uncached(\(&ctx->rm, 0x1000, &job->marker_mem\))/nvrm_alloc(\1)/' "$F" > "$W/m1.c"
cmp -s "$F" "$W/m1.c" && { echo "[-] mutant 1 did not change the source"; fail=$((fail + 1)); }
chk "mutant: cacheable marker page is refused" "$W/m1.c" 1
sed 's/&job->marker_mem/\&job->marker_mem_x/' "$F" > "$W/m2.c"
chk "mutant: marker allocation not found is refused" "$W/m2.c" 1
echo "failures $fail"; [ "$fail" = 0 ]
