#!/bin/sh
# Source guard: every page the host POLLS or reads for the resident seat's completion must be GPU-uncached.
# NVIDIA nvos.h NVOS32_ATTR2_GPU_CACHEABLE_YES: "For system memory this will not be coherent with direct CPU
# mappings"; spark-hardware-manual s4.1: "Any word the host polls ... must be allocated nvrm_alloc_gpu_uncached".
# Evidence (CAND-1 window 2 and the R15 seat A/B, 2026-10-05): with a GPU-cacheable marker page the seat's leave
# marker reached the host ~1.05 s late in 15 of 16 trials and never within the wait in 3 of 64 (~102 s stalls);
# with a GPU-uncached page it arrived in 13 to 20 ms in 16 of 16, no stall.
# Two guards: (1) the marker page is allocated uncached; (2) the semaphore word the host reads right after the
# marker (gates rx_gpu_seat_finish) lives on that same uncached page, not on the cached QMD page.
# The check reads src/runtime/rx_resident_gpu.c; mutants prove it can say no.
cd "$(dirname "$0")/.." || exit 1
F=${SEAT_SRC:-src/runtime/rx_resident_gpu.c}; fail=0
guard() { # file -> 0 when both pages are uncached
    grep -Eq 'nvrm_alloc_gpu_uncached\([^;]*&job->marker_mem\)' "$1" || return 1
    grep -Eq '[^_]nvrm_alloc\([^;]*&job->marker_mem\)' "$1" && return 1
    grep -Eq 'sem_va = marker_mem\.va \+ 0x[0-9a-fA-F]+;' "$1" || return 1
    grep -Eq '\(uint8_t \*\)marker_mem\.cpu \+ 0x[0-9a-fA-F]+\)' "$1" || return 1
    grep -Eq 'sem_va = qmd_mem\.va|\(uint8_t \*\)qmd_mem\.cpu \+ 0x2000' "$1" && return 1
    return 0
}
chk() { # name file expect(0=guard passes,1=guard must refuse)
    guard "$2"; got=$?
    if [ "$got" = "$3" ]; then echo "[+] $1"; else echo "[-] $1 (guard exit $got, want $3)"; fail=$((fail + 1)); fi
}
W=$(mktemp -d) || exit 1; trap 'rm -rf "$W"' EXIT
chk "seat marker page and semaphore are GPU-uncached" "$F" 0
sed 's/nvrm_alloc_gpu_uncached(\(&ctx->rm, 0x1000, &job->marker_mem\))/nvrm_alloc(\1)/' "$F" > "$W/m1.c"
cmp -s "$F" "$W/m1.c" && { echo "[-] mutant 1 did not change the source"; fail=$((fail + 1)); }
chk "mutant: cacheable marker page is refused" "$W/m1.c" 1
sed 's/&job->marker_mem/\&job->marker_mem_x/' "$F" > "$W/m2.c"
chk "mutant: marker allocation not found is refused" "$W/m2.c" 1
sed 's/sem_va = marker_mem\.va + 0x20;/sem_va = qmd_mem.va + 0x2000;/' "$F" > "$W/m3.c"
cmp -s "$F" "$W/m3.c" && { echo "[-] mutant 3 did not change the source"; fail=$((fail + 1)); }
chk "mutant: semaphore back on the cached QMD page is refused" "$W/m3.c" 1
sed 's/(uint8_t \*)marker_mem\.cpu + 0x20)/(uint8_t *)qmd_mem.cpu + 0x2000)/' "$F" > "$W/m4.c"
cmp -s "$F" "$W/m4.c" && { echo "[-] mutant 4 did not change the source"; fail=$((fail + 1)); }
chk "mutant: host reads the semaphore from the cached page is refused" "$W/m4.c" 1
echo "failures $fail"; [ "$fail" = 0 ]
