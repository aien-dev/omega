#!/usr/bin/env bash
# Turing calibration (CAL-0 / EXP-001): prove the sealed test data is separate
# from all development data, and was made after the freeze. Writes overlap_audit.json.
#
# usage: verify_holdout_separation.sh --sealed-dir DIR --out FILE [--dev-list FILE] [--repo DIR]
#
#   --sealed-dir  ~/aien-data/turing-cal/sealed/<commit> (written by generate_sealed_data.sh)
#   --out         where overlap_audit.json goes (normally
#                 calibration/experiments/EXP-001/overlap_audit.json)
#   --dev-list    text file, one development trace.ctr path per line. Default: every
#                 trace.ctr under ~/aien-data/crumbline (all burned development data,
#                 both conditions, all runs). Each trace.ctr's sibling ledger.jsonl
#                 is used for the ledger checks.
#   --repo        git repository (worktree or .git directory) holding the freeze
#                 commit; default: the checkout holding this script. Needed inside
#                 the Auditor jail, where the script runs from a git-archive export.
# The overlap helper is build/turing-cal/turing-cal-overlap in the tree holding this
# script (override: TC_OVERLAP_TOOL); in a read-only export it must be prebuilt.
#
# Gates (all must hold; exit 0 = PASS, 1 = FAIL, 2 = could not run):
#   G1 complete        COMPLETE exists and equals SHA-256(seed_commitment.json)
#   G2 integrity       every kept file in manifest.json has its recorded SHA-256
#   G3 seeds           every seed re-derives from the rule, none is burned (0..10, 20260927)
#   G4 after_freeze    every sealed file's mtime is later than the freeze commit's committer time, and the
#                      candidate manifest at that commit is "status": "frozen" (the commit is C_f)
#   G5 single_link     every sealed file has exactly one hard link
#   G6 crumb_digest    no ledger crumb_digest shared between dev and sealed
#   G7 sealed_digest   no ledger sealed_digest (hidden held-out set) shared
#   G8 trace_stream    no ledger trace_stream_digest shared, except degenerate stream
#                      digests (a digest repeated across distinct crumbs inside ONE
#                      ledger file is a content-empty stream; these are listed, not hidden)
#   G9 crumb_block     no crumb whose CTR1 record-body block (bytes 0..214 of every
#                      record, SHA-256) occurs on both sides (tools/turing_cal_overlap.c)
# Reported, not gated: first_state_overlap, record_body_overlap (see the C tool).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
toolrepo="$(cd "$here/../.." && pwd)"
repo="$(git -C "$here" rev-parse --show-toplevel 2>/dev/null || true)"
sealed="" out="" devlist=""
die() { echo "verify_holdout_separation: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
    case "$1" in
    --sealed-dir) sealed="$2"; shift 2 ;;
    --out) out="$2"; shift 2 ;;
    --dev-list) devlist="$2"; shift 2 ;;
    --repo) repo="$2"; shift 2 ;;
    *) die "unknown option '$1'" ;;
    esac
done
[ -d "$sealed" ] && [ -n "$out" ] || die "--sealed-dir DIR and --out FILE are required"
sha() { sha256sum -- "$1" | cut -d' ' -f1; }
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

tool="${TC_OVERLAP_TOOL:-$toolrepo/build/turing-cal/turing-cal-overlap}"
[ -x "$tool" ] || make -s -C "$toolrepo" turing-cal-overlap >/dev/null 2>&1 || die "turing-cal-overlap not built at $tool (build it before binding a read-only tree)"
[ -n "$repo" ] || die "--repo is required when this script is not inside a git checkout"

sc="$sealed/seed_commitment.json"
[ -f "$sc" ] && [ -f "$sealed/manifest.json" ] || die "seed_commitment.json / manifest.json missing"
jstr() { sed -n "s/^  \"$1\": \"\([^\"]*\)\".*/\1/p" "$sc" | head -1; }
commit="$(jstr freeze_commit)"
digest="$(jstr profile_digest)"
[[ "$commit" =~ ^[0-9a-f]{40}$ ]] || die "bad freeze_commit in seed_commitment.json"
ctime="$(git -C "$repo" show -s --format=%ct "$commit")" || die "freeze commit $commit not in $repo"

fails=()
gate() { # name ok(0/1) detail
    printf '    "%s": {"pass": %s, "detail": "%s"}' "$1" "$([ "$2" = 1 ] && echo true || echo false)" "$3"
    [ "$2" = 1 ] || fails+=("$1")
}

# G1
g1=0
[ -f "$sealed/COMPLETE" ] && [ "$(cat "$sealed/COMPLETE")" = "$(sha "$sc")" ] && g1=1

# G2
g2=1 nchk=0
while IFS=$'\t' read -r p h; do
    nchk=$((nchk + 1))
    [ -f "$sealed/$p" ] && [ "$(sha "$sealed/$p")" = "$h" ] || { g2=0; echo "integrity: $p" >&2; }
done < <(sed -n 's/.*"path": "\([^"]*\)", "sha256": "\([0-9a-f]*\)".*"kept": true}.*/\1\t\2/p' "$sealed/manifest.json")
[ "$nchk" -gt 0 ] || g2=0

# G3
g3=1 nseed=0
while read -r g j s; do
    nseed=$((nseed + 1))
    h="$(printf '%s' "turing.cal.sealed.v1|$commit|$digest|g$g|$j" | sha256sum | cut -c1-16)"
    want="$((16#$(printf '%x' $((16#${h:0:1} & 7)))${h:1}))"
    [ "$want" = "$s" ] || { g3=0; echo "seed rule mismatch g$g/$j" >&2; }
    { [ "$s" -le 10 ] || [ "$s" = 20260927 ]; } && g3=0
done < <(sed -n 's/.*"group": \([0-9]*\), "index": \([0-9]*\), "seed": \([0-9]*\)}.*/\1 \2 \3/p' "$sc")
[ "$nseed" -gt 0 ] || g3=0

# G4 / G5
early="$(find "$sealed" -type f ! -newermt "@$ctime" | wc -l)"
multi="$(find "$sealed" -type f -links +1 | wc -l)"
# The seed commitment must name the freeze commit C_f: the candidate manifest there says "status": "frozen".
frozen=0
git -C "$repo" show "$commit:calibration/experiments/EXP-001/candidate_manifest.json" 2>/dev/null | grep -q "^  \"status\": \"frozen\"," && frozen=1
g4=$([ "$early" = 0 ] && [ "$frozen" = 1 ] && echo 1 || echo 0)
g5=$([ "$multi" = 0 ] && echo 1 || echo 0)

# Dev set
if [ -z "$devlist" ]; then
    devlist="$work/dev.txt"
    find "$HOME/aien-data/crumbline" -name trace.ctr -type f | LC_ALL=C sort >"$devlist"
fi
find "$sealed" -name trace.ctr -type f | LC_ALL=C sort >"$work/sealed.txt"
ledgers() { while IFS= read -r t; do l="$(dirname "$t")/ledger.jsonl"; [ -f "$l" ] && echo "$l"; done <"$1"; }
ledgers "$devlist" >"$work/dev_ledgers.txt"
ledgers "$work/sealed.txt" >"$work/sealed_ledgers.txt"
[ -s "$work/dev_ledgers.txt" ] && [ -s "$work/sealed_ledgers.txt" ] || die "no ledgers found"
keys() { # key listfile -> sorted unique values
    xargs -d '\n' grep -ho "\"$1\":\"[0-9a-f]*\"" <"$2" | sed 's/.*:"\([0-9a-f]*\)"/\1/' | LC_ALL=C sort -u
}
shared() { LC_ALL=C comm -12 <(keys "$1" "$work/dev_ledgers.txt") <(keys "$1" "$work/sealed_ledgers.txt"); }
n6="$(shared crumb_digest | wc -l)"
n7="$(shared sealed_digest | wc -l)"
# Degenerate stream digests: repeated inside one ledger file (distinct crumbs, same stream).
cat "$work/dev_ledgers.txt" "$work/sealed_ledgers.txt" | while IFS= read -r l; do
    grep -o '"trace_stream_digest":"[0-9a-f]*"' "$l" | LC_ALL=C sort | uniq -d
done | sed 's/.*:"\([0-9a-f]*\)"/\1/' | LC_ALL=C sort -u >"$work/degenerate.txt"
shared trace_stream_digest | LC_ALL=C comm -23 - "$work/degenerate.txt" >"$work/ts_shared.txt"
n8="$(wc -l <"$work/ts_shared.txt")"
degen="$(paste -sd, "$work/degenerate.txt")"

# G9
set +e
ctr1="$("$tool" "$devlist" "$work/sealed.txt")"
trc=$?
set -e
[ "$trc" -le 1 ] || die "turing-cal-overlap failed (rc $trc)"
num() { printf '%s' "$ctr1" | sed -n "s/.*\"$1\":\([0-9]*\).*/\1/p"; }
n9="$(num crumb_block_overlap)"

mkdir -p "$(dirname "$out")"
{
    echo "{"
    echo "  \"schema\": \"turing.cal.overlap_audit.v1\","
    echo "  \"freeze_commit\": \"$commit\","
    echo "  \"freeze_commit_time_unix\": $ctime,"
    echo "  \"profile_digest\": \"$digest\","
    echo "  \"seed_commitment_sha256\": \"$(sha "$sc")\","
    echo "  \"dev_list_sha256\": \"$(sha "$devlist")\","
    echo "  \"dev_trace_files\": $(wc -l <"$devlist"),"
    echo "  \"sealed_trace_files\": $(wc -l <"$work/sealed.txt"),"
    echo "  \"degenerate_trace_stream_digests\": \"$degen\","
    echo "  \"ctr1\": $ctr1,"
    echo "  \"gates\": {"
    gate complete "$g1" "COMPLETE matches seed_commitment.json"; echo ","
    gate integrity "$g2" "$nchk kept files re-hashed"; echo ","
    gate seeds "$g3" "$nseed seeds re-derived"; echo ","
    gate after_freeze "$g4" "$early files not newer than commit time; manifest frozen at commit: $frozen"; echo ","
    gate single_link "$g5" "$multi files with extra hard links"; echo ","
    gate crumb_digest "$([ "$n6" = 0 ] && echo 1 || echo 0)" "$n6 shared"; echo ","
    gate sealed_digest "$([ "$n7" = 0 ] && echo 1 || echo 0)" "$n7 shared"; echo ","
    gate trace_stream "$([ "$n8" = 0 ] && echo 1 || echo 0)" "$n8 shared (degenerate excluded)"; echo ","
    gate crumb_block "$([ "$n9" = 0 ] && echo 1 || echo 0)" "$n9 shared"; echo
    echo "  },"
    echo "  \"result\": \"$([ ${#fails[@]} = 0 ] && echo PASS || echo FAIL)\","
    echo "  \"failed\": \"${fails[*]:-}\","
    echo "  \"generated_utc\": \"$(date -u +%Y-%m-%dT%H:%M:%SZ)\""
    echo "}"
} >"$out"
echo "overlap audit: $([ ${#fails[@]} = 0 ] && echo PASS || echo "FAIL (${fails[*]})") -> $out"
[ ${#fails[@]} = 0 ]
