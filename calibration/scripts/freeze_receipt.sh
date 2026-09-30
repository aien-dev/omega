#!/usr/bin/env bash
# freeze_receipt.sh: step 2 of the two-step freeze (BLINDING_PROTOCOL.md section 2). Writes
# calibration/experiments/EXP-001/freeze_receipt.json with TURING_PROFILE_V1_FROZEN = PASS for the freeze
# commit C_f, or refuses (exit 2) and writes nothing.
#
#   freeze_receipt.sh C_F [--no-fetch] [--out FILE]
#
# C_f is the commit that added the frozen candidate manifest, profile value and sidecar (step 1). It names no
# commit inside itself; this receipt is the first file that records C_f, and it is committed AFTER C_f (the
# receipt commit changes nothing the seeds or the evaluator depend on). Every check reads C_f through git, never
# the worktree:
#   1 C_f is a full 40-hex commit and an ancestor of origin/main (fetched first unless --no-fetch). C_f must
#     reach main by a real merge or a fast-forward; a squash or rebase makes a new commit and C_f is lost.
#   2 candidate_manifest.json and preregistration.json at C_f both have "status": "frozen".
#   3 the profile at C_f holds no FILL_AT_FREEZE; its SHA-256 equals the sidecar and the manifest profile_sha256.
#   4 runtime_digest in the profile at C_f equals runtime_sha256.runtime_digest in the manifest at C_f.
#   5 independent_scorer_source_sha256 in the manifest equals indep_source_digest.sh over C_f's tree.
#   6 every shared_background_sha256 entry equals the SHA-256 of that file at C_f.
#   7 no sealed data exists yet for C_f (sealed/<C_f> and its .failed-* are absent).
# No Python: bash, git, sha256sum, sed.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel)"
. "$here/tc_exp.sh"
cf="${1:-}"
shift || true
fetch=1 out="$repo/$EXP_DIR/freeze_receipt.json"
while [ $# -gt 0 ]; do
    case "$1" in
    --no-fetch) fetch=0; shift ;;
    --out) out="$2"; shift 2 ;;
    *) echo "freeze_receipt: unknown option '$1'" >&2; exit 2 ;;
    esac
done
die() { echo "freeze_receipt: REFUSED: $*" >&2; exit 2; }
M=$EXP_DIR/candidate_manifest.json
P=$EXP_PROFILE
S=$EXP_SIDECAR
at() { git -C "$repo" show "$cf:$1"; }

[[ "$cf" =~ ^[0-9a-f]{40}$ ]] || die "C_f must be a full 40-hex commit id"
git -C "$repo" cat-file -e "$cf^{commit}" 2>/dev/null || die "commit $cf not found"
[ "$fetch" = 1 ] && git -C "$repo" fetch -q origin main
om="$(git -C "$repo" rev-parse --verify -q 'origin/main^{commit}')" || die "no origin/main"
git -C "$repo" merge-base --is-ancestor "$cf" "$om" || die "check 1: $cf is not an ancestor of origin/main ($om)"

man="$(at "$M")" || die "check 2: $M missing at C_f"
grep -q '^  "status": "frozen",' <<<"$man" || die "check 2: manifest at C_f is not frozen"
pre="$(at $EXP_DIR/preregistration.json)" || die "check 2: preregistration.json missing at C_f"
grep -q "^  \"status\": \"frozen\"," <<<"$pre" || die "check 2: preregistration.json at C_f is not status frozen"
jv() { sed -n "s/^ *\"$1\": \"\([^\"]*\)\".*/\1/p" <<<"$man" | head -1; }

prof="$(at "$P")" || die "check 3: profile missing at C_f"
grep -q FILL_AT_FREEZE <<<"$prof" && die "check 3: profile at C_f still holds FILL_AT_FREEZE"
ph="$(at "$P" | sha256sum | cut -c1-64)"
sh_="$(at "$S" | cut -c1-64)" || die "check 3: sidecar missing at C_f"
[ "$ph" = "$sh_" ] || die "check 3: profile SHA-256 $ph != sidecar $sh_"
[ "$ph" = "$(jv profile_sha256)" ] || die "check 3: manifest profile_sha256 differs from the profile at C_f"

prd="$(sed -n 's/^runtime_digest = "\([0-9a-f]\{64\}\)".*/\1/p' <<<"$prof")"
[ -n "$prd" ] || die "check 4: profile runtime_digest is not a 64-hex value"
[ "$prd" = "$(jv runtime_digest)" ] || die "check 4: profile runtime_digest $prd != manifest $(jv runtime_digest)"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
git -C "$repo" archive "$cf" calibration/scripts/indep_source_digest.sh tools/turing_verify_indep | tar -x -C "$tmp"
idg="$(sh "$tmp/calibration/scripts/indep_source_digest.sh")"
[ "$idg" = "$(jv independent_scorer_source_sha256)" ] || die "check 5: independent scorer source at C_f is $idg, manifest says $(jv independent_scorer_source_sha256)"

nbg=0
while read -r rel want; do
    got=MISSING
    if git -C "$repo" cat-file -e "$cf:$rel" 2>/dev/null; then got="$(at "$rel" | sha256sum | cut -c1-64)"; fi
    [ "$got" = "$want" ] || die "check 6: shared background $rel at C_f is $got, manifest says $want"
    nbg=$((nbg + 1))
done < <(sed -n '/"shared_background_sha256": {/,/}/s/^ *"\([^"]*\)": "\([^"]*\)",\{0,1\}$/\1 \2/p' <<<"$man")
[ "$nbg" -gt 0 ] || die "check 6: no shared background entries"

sealed="${TC_SEALED_ROOT:-$HOME/aien-data/turing-cal/sealed}"
for x in "$sealed/$cf" "$sealed/$cf".*; do [ -e "$x" ] && die "check 7: sealed data for $cf already exists ($x)"; done

ct="$(git -C "$repo" show -s --format=%ct "$cf")"
[ ! -e "$out" ] || die "$out already exists (one receipt per freeze)"
cat >"$out" <<EOF
{
  "schema": "turing.cal.freeze_receipt.v1",
  "TURING_PROFILE_V1_FROZEN": "PASS",
  "freeze_commit": "$cf",
  "freeze_commit_time_utc": "$(date -u -d "@$ct" +%Y-%m-%dT%H:%M:%SZ)",
  "origin_main_checked": "$om",
  "profile_sha256": "$ph",
  "candidate_manifest_sha256": "$(at "$M" | sha256sum | cut -c1-64)",
  "runtime_digest": "$prd",
  "independent_scorer_source_sha256": "$idg",
  "shared_background_entries_checked": $nbg,
  "sealed_data_present": false,
  "written_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "next": "commit this receipt (a later commit than C_f), then generate_sealed_data.sh --commit $cf --profile-digest $ph"
}
EOF
echo "freeze_receipt: TURING_PROFILE_V1_FROZEN = PASS for $cf -> $out"
