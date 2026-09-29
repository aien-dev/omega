#!/bin/sh
# freeze_candidate.sh: hash every EXP-001 candidate model and the profile into
# calibration/experiments/EXP-001/candidate_manifest.json.
#
#   sh calibration/scripts/freeze_candidate.sh [--freeze] [CANDIDATE_DIR]
#
# CANDIDATE_DIR (default build/turing-exp001-a/candidates) must hold the seven .tym files written by
# `make turing-exp001-a-candidates` (dev seeds 1-7 only). For each file the manifest records: file SHA-256,
# model digest SHA-256('turing.ymodel.v0' || 0x00 || bytes), byte size, L(M) from the TYM0 header
# (104 + 16(K-1) + rows x (keybits + 16(K-1))), and where the frozen bytes live. Small codes are copied into
# calibration/experiments/EXP-001/candidates/ (committed); the two memorization controls (76 MB and 45 MB)
# are not committed: their bytes are regenerated deterministically from dev seeds 1-7 by
# `make turing-exp001-a-candidates` and must hash to the recorded SHA-256 before use.
# Also recorded: profile SHA-256, sidecar content, SHA-256 of every shared-background file, git HEAD.
#
# Default (draft) mode: writes the manifest with "status": "draft".
# --freeze: additionally requires a clean worktree, check_profile.sh --freeze passing (no FILL_AT_FREEZE, sidecar
# matches), and refuses if any sealed data directory exists for HEAD. Writes "status": "frozen".
# Refuses if the candidate directory holds anything other than the seven expected files, or if any sealed
# path is passed. It never reads sealed data. No Python: POSIX sh, od, sha256sum.
set -eu
dir=$(cd "$(dirname "$0")/../.." && pwd)
freeze=0
if [ "${1:-}" = "--freeze" ]; then freeze=1; shift; fi
cdir="${1:-$dir/build/turing-exp001-a/candidates}"
out="$dir/calibration/experiments/EXP-001/candidate_manifest.json"
store="$dir/calibration/experiments/EXP-001/candidates"
toml="$dir/calibration/profiles/Turing-profile-v1.0.toml"
side="$dir/calibration/profiles/Turing-profile-v1.0.sha256"
die() { echo "freeze_candidate: REFUSED: $*" >&2; exit 1; }

case "$cdir" in *sealed*|*turing-cal/sealed*) die "candidate directory looks like sealed data: $cdir" ;; esac
names="B0_uniform B1_order0 B2_order1 B3_heuristic M_candidate M_mem M_mem_seed1"
for n in $names; do [ -f "$cdir/$n.tym" ] || die "$cdir/$n.tym missing (run make turing-exp001-a-candidates)"; done
extra=$(ls "$cdir" | grep -v -x -e B0_uniform.tym -e B1_order0.tym -e B2_order1.tym -e B3_heuristic.tym \
    -e M_candidate.tym -e M_mem.tym -e M_mem_seed1.tym || true)
[ -z "$extra" ] || die "unexpected files in $cdir: $extra"
[ -f "$toml" ] || die "profile missing"

head=$(git -C "$dir" rev-parse HEAD)
if [ "$freeze" = 1 ]; then
    [ -z "$(git -C "$dir" status --porcelain)" ] || die "worktree not clean"
    sh "$dir/calibration/scripts/check_profile.sh" --freeze || die "check_profile --freeze failed"
    [ ! -e "$HOME/aien-data/turing-cal/sealed/$head" ] || die "sealed data already exist for $head"
fi

# u(file, byte offset, nbytes) -> unsigned big-endian integer
u() { od -An -t u1 -j "$2" -N "$3" "$1" | tr -s ' \n' '  ' | awk '{v=0; for(i=1;i<=NF;i++) v=v*256+$i; printf "%.0f", v}'; }
# awk is used only as a calculator above; no data is interpreted by anything but the header layout.

mkdir -p "$store"
psha=$(sha256sum "$toml" | cut -c1-64)
sidec=""
[ -f "$side" ] && sidec=$(cut -c1-64 "$side")
{
    printf '{\n  "schema": "turing.cal.candidate_manifest.v1",\n'
    printf '  "experiment": "EXP-001",\n'
    printf '  "status": "%s",\n' "$([ "$freeze" = 1 ] && echo frozen || echo draft)"
    printf '  "git_head": "%s",\n' "$head"
    printf '  "profile_path": "calibration/profiles/Turing-profile-v1.0.toml",\n'
    printf '  "profile_sha256": "%s",\n' "$psha"
    printf '  "profile_sidecar_sha256": "%s",\n' "$sidec"
    printf '  "profile_sidecar_matches": %s,\n' "$([ "$psha" = "$sidec" ] && echo true || echo false)"
    printf '  "fit_data": "exp-20260927-rep10 control seeds 1-7, manifest sha256 6efc04b525af60706bd0f3176013fe234d8e8a52e2554cff3add8a4e16171dca",\n'
    printf '  "candidates": [\n'
    first=1
    for n in $names; do
        f="$cdir/$n.tym"
        bytes=$(wc -c < "$f" | tr -d ' ')
        fsha=$(sha256sum "$f" | cut -c1-64)
        mdig=$({ printf 'turing.ymodel.v0\000'; cat "$f"; } | sha256sum | cut -c1-64)
        K=$(u "$f" 5 1); kb=$(u "$f" 8 1); rows=$(u "$f" 9 4)
        lm=$((104 + 16 * (K - 1) + rows * (kb + 16 * (K - 1))))
        [ $(((lm + 7) / 8)) = "$bytes" ] || die "$n: header implies $lm bits but file has $bytes bytes"
        if [ "$bytes" -lt 1048576 ]; then
            cp "$f" "$store/$n.tym"
            loc="git:calibration/experiments/EXP-001/candidates/$n.tym"
        else
            loc="regenerate:make turing-exp001-a-candidates (deterministic from dev seeds 1-7); verify file_sha256 before use"
        fi
        [ "$first" = 1 ] || printf ',\n'
        first=0
        printf '    {"name": "%s", "file": "%s.tym", "bytes": %s, "file_sha256": "%s", "model_digest": "%s", "K": %s, "key_bits": %s, "rows": %s, "lm_bits": %s, "location": "%s"}' \
            "$n" "$n" "$bytes" "$fsha" "$mdig" "$K" "$kb" "$rows" "$lm" "$loc"
    done
    printf '\n  ],\n  "shared_background_sha256": {\n'
    first=1
    for p in src/turing/ty_model.c src/turing/ty_model.h src/turing/ty_ctr1.c src/turing/ty_ctr1.h \
        src/turing/ty_math.c src/turing/ty_math.h calibration/docs/MODEL_DESCRIPTION_ENCODING.md \
        calibration/docs/CODER_SPEC.md calibration/profiles/Turing-profile-v1.0.toml; do
        if [ -f "$dir/$p" ]; then h=$(sha256sum "$dir/$p" | cut -c1-64); else h="MISSING"; fi
        [ "$first" = 1 ] || printf ',\n'
        first=0
        printf '    "%s": "%s"' "$p" "$h"
    done
    printf '\n  },\n  "runtime_sha256": {\n'
    first=1
    for b in ${TXA_RUNTIME_BINS:-$dir/build/turing-exp001-a/turing-cal-candidates}; do
        [ -f "$b" ] || continue
        [ "$first" = 1 ] || printf ',\n'
        first=0
        printf '    "%s": "%s"' "$(basename "$b")" "$(sha256sum "$b" | cut -c1-64)"
    done
    printf '\n  }\n}\n'
} > "$out.tmp"
mv "$out.tmp" "$out"
if [ "$freeze" = 1 ]; then
    [ -z "$(grep MISSING "$out" || true)" ] || die "a shared-background file is missing; manifest left at $out for inspection"
fi
echo "freeze_candidate: wrote $out ($([ "$freeze" = 1 ] && echo frozen || echo draft))"
