#!/bin/bash
# e1_host_rerun.sh -- the E1 host tier (no GPU) on the checked-out commit, as one record.
#
#   tools/e1_host_rerun.sh run OUTDIR [PHYSICS_DIR]      run the six host lines, logs + summary into OUTDIR
#   tools/e1_host_rerun.sh record OUTDIR EVIDENCE_DIR    turn OUTDIR into a content-named record
#
# The six lines are the host tier of the E1 chip campaign (forge jobs E1S-*), run
# again on a different commit (the squash merge of the qualified candidate) so a
# host-class file that differs between the two commits is covered by evidence rather
# than by assertion. The record (schema AIEN_E1_HOST_RERUN_V1) names the commit, the
# tracked-tree cleanliness before and after, the six verdicts and the SHA-256 of each
# line's log; it is written as EVIDENCE_DIR/<sha256 of file>.json next to the logs as
# EVIDENCE_DIR/blobs/<sha256>.log. tools/e1_combine.sh requires every line PASS.
# Shell + coreutils + make + gcc + jq + tools/json_canon.c. No Python.
set -u
HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
LINES="GB10-COMPILE NUMERIC-HOST PROGRAM TRANSC MANIFEST HOSTALL"

run_all() {
    local L=$1 PHYS=${2:-${PHYSICS_DIR:-}} NV name s r
    [ -n "$PHYS" ] && [ -d "$PHYS/nvrm" ] || { echo "run: need PHYSICS_DIR (a checkout of aien-dev/physics at physics.lock)" >&2; exit 2; }
    NV=$PHYS/third_party/nvidia-open-580.173.02
    mkdir -p "$L"; cd "$HERE" || exit 2
    echo "HEAD $(git rev-parse HEAD) dirty_tracked=$(git status --porcelain --untracked-files=no | wc -l) start $(date -u +%FT%TZ)" > "$L/summary.txt"
    run() { name=$1; shift; s=$(date +%s); if bash -c "$*" > "$L/$name.log" 2>&1; then r=PASS; else r="FAIL(rc=$?)"; fi; echo "$name: $r ($(( $(date +%s) - s ))s)" >> "$L/summary.txt"; }
    run GB10-COMPILE "D=\$(mktemp -d /tmp/e1r-compile.XXXXXX); rc=0; for v in -DE1N_NONE -DOMEGA_C3_PROTECT_OFF; do for f in tests/test_omega_c3_ab_gb10.c src/omega_numeric_gb10.c src/omega_numeric_reduce_gb10.c src/omega_numeric_ldst_gb10.c src/omega_numeric_divsqrt_gb10.c; do gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off -fno-fast-math -pthread -Isrc -I$PHYS/nvrm -I$PHYS/m16 -I$NV/src/common/sdk/nvidia/inc -I$NV/kernel-open/common/inc -I$NV/kernel-open/nvidia-uvm -I$NV/src/nvidia/arch/nvalloc/unix/include \$v -c \$f -o \$D/x.o || { echo COMPILE_FAIL \$v \$f; rc=1; }; done; done; rm -rf \$D; test \$rc = 0 && echo GB10_COMPILE_OK; exit \$rc"
    run NUMERIC-HOST "make -k PHYSICS_DIR=$PHYS test-numeric-cpu test-numeric-transc-gb10-host test-divsqrt-host test-divsqrt-nvdisasm test-numeric-reduce-cpu test-numeric-reduce-nvdisasm test-ldst-host test-ldst-nvdisasm"
    run PROGRAM "make -k PHYSICS_DIR=$PHYS test-program-fp32 test-program-realize test-program-id test-visor-core"
    run TRANSC "make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-numeric-transc test-numeric-transc-digest test-numeric-transc-full"
    run MANIFEST "bash tests/test_chip_run_transc_manifest.sh && bash tests/test_chip_run.sh"
    run HOSTALL "make PHYSICS_DIR=$PHYS build/test_omega_numeric_transc_gb10_cpu && for o in SIN COS ERF GELU RSQRT; do ./build/test_omega_numeric_transc_gb10_cpu --host-all \$o || exit 1; done"
    echo "end $(date -u +%FT%TZ) dirty_tracked_after=$(git status --porcelain --untracked-files=no | wc -l)" >> "$L/summary.txt"
    echo DONE >> "$L/summary.txt"
    cat "$L/summary.txt"
}

record() {
    local L=$1 EV=$2 JC commit before after start end l v lines= logs= s body d
    [ -f "$L/summary.txt" ] && [ "$(tail -n1 "$L/summary.txt")" = DONE ] || { echo "record: $L/summary.txt is not finished (no DONE line)" >&2; exit 1; }
    JC=$(mktemp -d "${TMPDIR:-/tmp}/e1hr.XXXXXX"); trap "rm -rf '$JC'" EXIT
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$HERE/src" -o "$JC/json_canon" "$HERE/tools/json_canon.c" "$HERE/src/sha256.c" -lm || exit 1
    read -r _ commit before _ start < <(head -n1 "$L/summary.txt")
    before=${before#dirty_tracked=}; end=$(grep -E '^end ' "$L/summary.txt" | awk '{print $2}'); after=$(grep -E '^end ' "$L/summary.txt" | sed 's/.*dirty_tracked_after=//')
    [[ $commit =~ ^[0-9a-f]{40}$ ]] || { echo "record: bad HEAD line" >&2; exit 1; }
    mkdir -p "$EV/blobs"
    for l in $LINES; do
        v=$(grep -E "^$l: " "$L/summary.txt" | sed -E "s/^$l: ([^ ]+) .*/\1/"); [ -n "$v" ] || { echo "record: no line $l" >&2; exit 1; }
        [ -f "$L/$l.log" ] || { echo "record: no log for $l" >&2; exit 1; }
        s=$(sha256sum -- "$L/$l.log" | cut -c1-64)
        if [ -e "$EV/blobs/$s.log" ]; then cmp -s "$L/$l.log" "$EV/blobs/$s.log" || { echo "record: blob collision $s" >&2; exit 1; }; else cp -- "$L/$l.log" "$EV/blobs/$s.log"; chmod 0444 "$EV/blobs/$s.log"; fi
        lines+="\"$l\":\"$v\","; logs+="\"$l\":\"$s\","
    done
    body="{\"schema\":\"AIEN_E1_HOST_RERUN_V1\",\"omega_commit\":\"$commit\",\"tracked_tree_clean_before\":$([ "$before" = 0 ] && echo true || echo false),\"tracked_tree_clean_after\":$([ "$after" = 0 ] && echo true || echo false),\"started_utc\":\"$start\",\"finished_utc\":\"$end\",\"lines\":{${lines%,}},\"log_sha256\":{${logs%,}},\"summary_sha256\":\"$(sha256sum -- "$L/summary.txt" | cut -c1-64)\",\"digest_meaning\":\"file name is the SHA-256 of this file; integrity only, not authenticity\"}"
    printf '%s' "$body" | "$JC/json_canon" --pretty > "$JC/rec.json" || exit 1
    d=$(sha256sum -- "$JC/rec.json" | cut -c1-64)
    [ -e "$EV/$d.json" ] && { echo "record: $EV/$d.json already exists" >&2; exit 1; }
    cp -- "$JC/rec.json" "$EV/$d.json" && chmod 0444 "$EV/$d.json" && cp -- "$L/summary.txt" "$EV/blobs/$(sha256sum -- "$L/summary.txt" | cut -c1-64).txt"
    echo "E1 HOST RERUN RECORD: $EV/$d.json"; jq -c .lines "$EV/$d.json"
}

case ${1:-} in
    run) [ $# -ge 2 ] || { sed -n '4,5p' "$0" >&2; exit 2; }; run_all "$2" "${3:-}";;
    record) [ $# -eq 3 ] || { sed -n '4,5p' "$0" >&2; exit 2; }; record "$2" "$3";;
    *) sed -n '4,5p' "$0" >&2; exit 2;;
esac
