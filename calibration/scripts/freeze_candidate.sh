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
# are not committed: a copy goes to ~/aien-data/turing-cal/candidates/ ($TXA_BIG_STORE), their bytes are
# regenerated deterministically from dev seeds 1-7 by `make turing-exp001-a-candidates`, and they must hash to
# the recorded SHA-256 before use. runtime_sha256 lists runtime_digest.sh --lines when all binaries are built.
# Also recorded: profile SHA-256, sidecar content, SHA-256 of every shared-background file, and
# independent_scorer_source_sha256 (rule below). The manifest names NO commit: the commit that adds the frozen
# manifest is the freeze commit C_f (two-step freeze, BLINDING_PROTOCOL.md section 2); sealed seeds are derived
# from C_f, and a later freeze-receipt commit (freeze_receipt.sh C_f) records it.
# independent_scorer_source_sha256 = SHA-256 of the LF-terminated lines "<sha256>  <path>" for every file under
# tools/turing_verify_indep/, path relative to that directory, sorted by path (LC_ALL=C byte order).
#
# Default (draft) mode: writes the manifest with "status": "draft".
# --freeze: additionally requires a clean worktree, check_profile.sh --freeze passing (no FILL_AT_FREEZE, sidecar
# matches) and calibration/experiments/EXP-001/preregistration.json at "status": "frozen", and refuses if any sealed data entry exists (EXP-001R: other than the burned EXP-001 root) (sealed data may only exist after C_f).
# Writes "status": "frozen".
# Refuses if the candidate directory holds anything other than the seven expected files, or if any sealed
# path is passed. It never reads sealed data. No Python: POSIX sh, od, sha256sum.
set -eu
dir=$(cd "$(dirname "$0")/../.." && pwd)
. "$dir/calibration/scripts/tc_exp.sh"
freeze=0
if [ "${1:-}" = "--freeze" ]; then freeze=1; shift; fi
cdir="${1:-$dir/build/turing-exp001-a/candidates}"
out="$dir/$EXP_DIR/candidate_manifest.json"
store="$dir/$EXP_CAND_STORE"
toml="$dir/$EXP_PROFILE"
side="$dir/$EXP_SIDECAR"
die() { echo "freeze_candidate: REFUSED: $*" >&2; exit 1; }

case "$cdir" in *sealed*|*turing-cal/sealed*) die "candidate directory looks like sealed data: $cdir" ;; esac
names="B0_uniform B1_order0 B2_order1 B3_heuristic M_candidate M_mem M_mem_seed1"
for n in $names; do [ -f "$cdir/$n.tym" ] || die "$cdir/$n.tym missing (run make turing-exp001-a-candidates)"; done
extra=$(ls "$cdir" | grep -v -x -e B0_uniform.tym -e B1_order0.tym -e B2_order1.tym -e B3_heuristic.tym \
    -e M_candidate.tym -e M_mem.tym -e M_mem_seed1.tym || true)
[ -z "$extra" ] || die "unexpected files in $cdir: $extra"
[ -f "$toml" ] || die "profile missing"


if [ "$freeze" = 1 ]; then
    [ -z "$(git -C "$dir" status --porcelain)" ] || die "worktree not clean"
    sh "$dir/calibration/scripts/check_profile.sh" --freeze || die "check_profile --freeze failed"
    grep -q "^  \"status\": \"frozen\"," "$dir/$EXP_DIR/preregistration.json" || die "preregistration.json is not status frozen (set it in the same commit, before this step)"
    sealed="${TC_SEALED_ROOT:-$HOME/aien-data/turing-cal/sealed}"
    _bad=$(tc_sealed_clear "$sealed") || die "sealed data already exist under $sealed (freeze must come first):$_bad"
fi

# u(file, byte offset, nbytes) -> unsigned big-endian integer
u() { od -An -t u1 -j "$2" -N "$3" "$1" | tr -s ' \n' '  ' | awk '{v=0; for(i=1;i<=NF;i++) v=v*256+$i; printf "%.0f", v}'; }
# awk is used only as a calculator above; no data is interpreted by anything but the header layout.

mkdir -p "$store" "$dir/$EXP_DIR"
psha=$(sha256sum "$toml" | cut -c1-64)
sidec=""
[ -f "$side" ] && sidec=$(cut -c1-64 "$side")
{
    printf '{\n  "schema": "turing.cal.candidate_manifest.v1",\n'
    printf '  "experiment": "%s",\n' "$EXP_ID"
    printf '  "status": "%s",\n' "$([ "$freeze" = 1 ] && echo frozen || echo draft)"
    [ "$freeze" = 1 ] && printf '  "frozen_at": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "profile_path": "%s",\n' "$EXP_PROFILE"
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
            if [ -f "$store/$n.tym" ]; then cmp -s "$f" "$store/$n.tym" || die "$n differs from the committed $store/$n.tym (no refit)"; else cp "$f" "$store/$n.tym"; fi
            loc="git:$EXP_CAND_STORE/$n.tym"
        else
            big="${TXA_BIG_STORE:-$HOME/aien-data/turing-cal/candidates}"
            mkdir -p "$big"
            cp "$f" "$big/$n.tym.tmp" && mv "$big/$n.tym.tmp" "$big/$n.tym"
            [ "$(sha256sum "$big/$n.tym" | cut -c1-64)" = "$fsha" ] || die "$n: copy in $big does not hash to $fsha"
            loc="outside git: copy at ~/aien-data/turing-cal/candidates/$n.tym; regenerate with make turing-exp001-a-candidates (deterministic from dev seeds 1-7); verify file_sha256 before use"
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
        calibration/docs/CODER_SPEC.md calibration/docs/UNCERTAINTY_PROTOCOL.md calibration/docs/FAILURE_REPORTING.md \
        calibration/docs/BLINDING_PROTOCOL.md $EXP_PREREG \
        $EXP_DIR/preregistration.json $EXP_PROFILE \
        calibration/scripts/power_simulation.c $EXP_POWER \
        calibration/docs/EVALUATOR.md calibration/docs/DATA_FORMAT.md \
        tools/turing_cal_eval.c; do
        if [ -f "$dir/$p" ]; then h=$(sha256sum "$dir/$p" | cut -c1-64); else h="MISSING"; fi
        [ "$first" = 1 ] || printf ',\n'
        first=0
        printf '    "%s": "%s"' "$p" "$h"
    done
    printf '\n  },\n  "independent_scorer_source_sha256": "%s",\n' "$(sh "$dir/calibration/scripts/indep_source_digest.sh")"
    printf '  "runtime_sha256": {\n'
    rl=$(sh "$dir/calibration/scripts/runtime_digest.sh" --lines 2>/dev/null || true)
    if [ -n "$rl" ]; then
        printf '%s\n' "$rl" | sed 's/^\([0-9a-f]*\)  \(.*\)$/    "\2": "\1",/'
        printf '    "runtime_digest": "%s"' "$(printf '%s\n' "$rl" | sha256sum | cut -c1-64)"
    else
        printf '    "runtime_digest": "not computed (build all seven runtime binaries; see runtime_digest.sh)"'
    fi
    printf '\n  }\n}\n'
} > "$out.tmp"
mv "$out.tmp" "$out"
if [ "$freeze" = 1 ]; then
    [ -z "$(grep MISSING "$out" || true)" ] || die "a shared-background file is missing; manifest left at $out for inspection"
fi
echo "freeze_candidate: wrote $out ($([ "$freeze" = 1 ] && echo frozen || echo draft))"
