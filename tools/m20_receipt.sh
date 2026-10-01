#!/bin/bash
# tools/m20_receipt.sh -- reproducible, content-addressed M20 OMEGA_TENSOR receipt.
#
#   tools/m20_receipt.sh --tensor-log FILE --mutations-log FILE
#       [--evidence-dir DIR] [--repo DIR] [--chip-evidence FILE]
#       [--binary FILE]... [--started-utc T] [--finished-utc T]
#
# Inputs: the host gate logs of `make test-tensor` and `make
# test-tensor-mutations`, optionally the GB10 chip evidence JSON written by
# tests/run_tensor_chip.sh (only its path and bytes are used, never its code),
# optional binaries to hash. Row labels come from the qualification table in
# the committed docs/tensor/M20_OMEGA_TENSOR.md of the omega tree (--repo,
# default: the tree holding this script).
#
# Table labels: "PASS (CPU)" or "host PASS" -> host PASS; "PASS (GB10)" or
# "GB10 PASS" -> GB10 PASS; NOT_RUN; MISSING_IMPLEMENTATION; "WIRED" and
# "PASS (on main)" -> REFERENCE (recorded, never counted). Anything else is
# refused.
#
# Rules (each line carries a MUT:<name> marker; tools/m20_receipt_mutations.sh
# breaks each one and requires tests/test_m20_receipt.sh to fail):
#   refused (exit 2, no receipt): dirty tree before; evidence dir inside the
#   tree; unknown label; host PASS rows while the host logs do not show PASS;
#   a GB10 PASS row without --chip-evidence, or whose evidence cell does not
#   name the sha256 of that file (first 64-hex string in the cell); an existing receipt with the same digest.
#   NOT QUALIFIED (exit 1, receipt written): any NOT_RUN or
#   MISSING_IMPLEMENTATION row; no host/GB10 PASS row at all; tree dirty or
#   HEAD moved after the inputs were stored; the test-only mid-run hook set.
#   QUALIFIED (exit 0) otherwise.
# Output: <evidence-dir>/<sha256>.json (mode 0444, created exclusively, never
# overwritten) and blobs/<sha256>.{log,json,bin}. Body is jq -S (sorted keys);
# no clock is read, timestamps appear only when passed in, so the same inputs
# give byte-identical JSON.
# Shell + coreutils + git + jq. No Python.
set -u
SELF_DIR=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$SELF_DIR/..
EVID=$HOME/workspace/evidence-out/M20-OMEGA-TENSOR
TLOG=""; MLOG=""; CHIP=""; STARTED=""; FINISHED=""
BINS=()
TABLE_REL=docs/tensor/M20_OMEGA_TENSOR.md

refuse() { echo "m20_receipt: REFUSED: $*" >&2; exit 2; }
need() { [ -n "${2:-}" ] || refuse "$1 needs a value"; }
while [ $# -gt 0 ]; do
    case $1 in
        --repo) need "$1" "${2:-}"; REPO=$2; shift 2 ;;
        --evidence-dir) need "$1" "${2:-}"; EVID=$2; shift 2 ;;
        --tensor-log) need "$1" "${2:-}"; TLOG=$2; shift 2 ;;
        --mutations-log) need "$1" "${2:-}"; MLOG=$2; shift 2 ;;
        --chip-evidence) need "$1" "${2:-}"; CHIP=$2; shift 2 ;;
        --binary) need "$1" "${2:-}"; BINS+=("$2"); shift 2 ;;
        --started-utc) need "$1" "${2:-}"; STARTED=$2; shift 2 ;;
        --finished-utc) need "$1" "${2:-}"; FINISHED=$2; shift 2 ;;
        -h|--help) sed -n '2,34p' "$0"; exit 0 ;;
        *) refuse "unknown argument $1" ;;
    esac
done
command -v jq > /dev/null || refuse "jq not found"
[ -f "$TLOG" ] || refuse "--tensor-log file missing"
[ -f "$MLOG" ] || refuse "--mutations-log file missing"
[ -z "$CHIP" ] || [ -f "$CHIP" ] || refuse "--chip-evidence file $CHIP missing"
for b in ${BINS[@]+"${BINS[@]}"}; do [ -f "$b" ] || refuse "--binary file $b missing"; done

TOP=$(git -C "$REPO" rev-parse --show-toplevel 2> /dev/null) || refuse "$REPO is not a git tree"
TOP=$(cd -P "$TOP" && pwd)
COMMIT=$(git -C "$TOP" rev-parse HEAD) || refuse "cannot read HEAD"
[ -z "$(git -C "$TOP" status --porcelain)" ] || refuse "omega tree dirty before receipt"  # MUT:DIRTY_REFUSED
EVID_ABS=$(realpath -m "$EVID") || refuse "cannot resolve evidence dir"
case "$EVID_ABS/" in "$TOP/"*) refuse "evidence dir $EVID_ABS lies inside the omega tree" ;; esac  # MUT:EVID_OUTSIDE
TABLE=$TOP/$TABLE_REL
[ -f "$TABLE" ] || refuse "qualification table $TABLE_REL missing"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/m20-receipt.XXXXXX") || refuse "cannot create work dir"
trap 'rm -rf "$WORK"' EXIT
ROWS=$WORK/rows.tsv; REASONS=$WORK/reasons.txt; BINTSV=$WORK/bins.tsv
: > "$ROWS"; : > "$REASONS"; : > "$BINTSV"
NQ=0
add_nq() { NQ=1; printf '%s\n' "$1" >> "$REASONS"; }
trim() { printf '%s' "$1" | sed 's/^[[:space:]]*//; s/[[:space:]]*$//'; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

# Host logs PASS: both test binaries (plain + ASan/UBSan) report N pass, 0 fail,
# the no-hooks check passed, and the mutation sweep passed with nothing missed.
host_logs_pass() {
    local n
    n=$(grep -cE '^M20 OMEGA_TENSOR CPU tests: [1-9][0-9]* pass, 0 fail$' "$TLOG")
    [ "$n" -ge 2 ] || return 1
    ! grep -qE '^M20 OMEGA_TENSOR CPU tests: [0-9]+ pass, [1-9][0-9]* fail' "$TLOG" || return 1
    grep -q '^test-tensor-no-hooks: PASS' "$TLOG" || return 1
    grep -q '^tensor mutation sweep: PASS' "$MLOG" || return 1
    ! grep -qE 'sweep: FAIL|NOT CAUGHT|NOT APPLIED|does not build' "$MLOG"
}

TLOG_SHA=$(sha "$TLOG"); MLOG_SHA=$(sha "$MLOG"); TABLE_SHA=$(sha "$TABLE")
CHIP_SHA=""; [ -z "$CHIP" ] || CHIP_SHA=$(sha "$CHIP")

# Table rows: the pipe table after "## Qualification checklist", up to the
# next heading or bold verdict line.
awk '/^## Qualification checklist/ {on = 1; next}
     on && (/^## / || /^\*\*/) {exit}
     on && /^\|/ {if ($0 ~ /^\|[-: |]*\|[[:space:]]*$/) next; print}' "$TABLE" > "$WORK/table.txt"
COUNTED=0; HAS_HOST=0; HAS_GB10=0
while IFS= read -r line; do
    line=${line#|}; line=${line%|}
    item=$(trim "${line%%|*}"); rest=${line#*|}
    status=$(trim "${rest%%|*}"); evcell=""
    [ "${rest#*|}" = "$rest" ] || evcell=${rest#*|}
    [ "$item" = Item ] && continue
    label=""; hw=""; ev=""
    case $status in
        "PASS (CPU)"|"host PASS") label="host PASS"; hw=host ;;
        "PASS (GB10)"|"GB10 PASS") label="GB10 PASS"; hw=GB10 ;;
        NOT_RUN|MISSING_IMPLEMENTATION) label=$status; hw=none ;;
        WIRED|"PASS (on main)") label=REFERENCE; hw=none ;;
        *) refuse "row '$item': unknown label '$status'" ;;  # MUT:UNKNOWN_LABEL
    esac
    case $label in
        NOT_RUN|MISSING_IMPLEMENTATION) add_nq "row '$item' is $label" ;;  # MUT:NOTRUN_FORCES_NQ
        "host PASS") HAS_HOST=1; COUNTED=$((COUNTED + 1)); ev="$TLOG_SHA,$MLOG_SHA" ;;
        "GB10 PASS")
            HAS_GB10=1; COUNTED=$((COUNTED + 1))
            ehash=$(printf '%s' "$evcell" | grep -oE '[0-9a-f]{64}' | head -1)
            [ -n "$CHIP" ] || refuse "row '$item' is GB10 PASS but no --chip-evidence file was given"  # MUT:GB10_NEEDS_EVIDENCE
            [ "$ehash" = "$CHIP_SHA" ] || refuse "row '$item': GB10 PASS evidence hash does not match the chip evidence file"  # MUT:GB10_HASH_MATCH
            ev=$CHIP_SHA ;;
    esac
    printf '%s\t%s\t%s\t%s\n' "$label" "$hw" "$item" "$ev" >> "$ROWS"
done < "$WORK/table.txt"
[ -s "$ROWS" ] || refuse "no rows found in the qualification table"
if [ "$HAS_HOST" = 1 ]; then host_logs_pass || refuse "host PASS rows but the host logs do not show PASS"; fi  # MUT:HOST_LOG_PASS
[ "$COUNTED" -gt 0 ] || add_nq "no host PASS or GB10 PASS row"  # MUT:NO_ROWS_NQ
HW=host; [ "$HAS_GB10" = 0 ] || HW=GB10

# store_blob SRC EXT -> prints the digest (pattern of tests/run_reduce_chip.sh).
mkdir -p "$EVID_ABS/blobs" || refuse "cannot create $EVID_ABS/blobs"
store_blob() {
    local src=$1 ext=$2 d dst tmp have
    d=$(sha "$src"); [ -n "$d" ] || refuse "cannot hash $src"
    dst=$EVID_ABS/blobs/$d.$ext
    if [ -e "$dst" ]; then
        have=$(sha "$dst"); [ "$have" = "$d" ] || refuse "existing blob $dst does not match its digest"
    else
        tmp=$(mktemp "$EVID_ABS/blobs/.tmp.XXXXXX") || refuse "cannot create temp blob"
        cp "$src" "$tmp" || { rm -f "$tmp"; refuse "cannot store $ext blob"; }
        have=$(sha "$tmp"); [ "$have" = "$d" ] || { rm -f "$tmp"; refuse "stored $ext blob does not match its digest"; }
        chmod 0444 "$tmp" || { rm -f "$tmp"; refuse "cannot seal $ext blob"; }
        mv -n "$tmp" "$dst"; rm -f "$tmp"
        have=$(sha "$dst"); [ "$have" = "$d" ] || refuse "blob $dst does not match its digest after publish"
    fi
    echo "$d"
}
store_blob "$TLOG" log > /dev/null || exit 2
store_blob "$MLOG" log > /dev/null || exit 2
[ -z "$CHIP" ] || store_blob "$CHIP" json > /dev/null || exit 2
for b in ${BINS[@]+"${BINS[@]}"}; do
    d=$(store_blob "$b" bin) || exit 2
    printf '%s\t%s\n' "$(basename "$b")" "$d" >> "$BINTSV"
done
LC_ALL=C sort -o "$BINTSV" "$BINTSV"

# Test-only: lets tests/test_m20_receipt.sh change the tree mid-run. Setting
# it always forces NOT QUALIFIED.
if [ -n "${M20_RECEIPT_TEST_MIDRUN:-}" ]; then
    add_nq "test-only mid-run hook was set"
    sh -c "$M20_RECEIPT_TEST_MIDRUN"
fi
CLEAN_AFTER=false; [ -n "$(git -C "$TOP" status --porcelain)" ] || CLEAN_AFTER=true
UNCHANGED=false; [ "$(git -C "$TOP" rev-parse HEAD)" != "$COMMIT" ] || UNCHANGED=true
[ "$CLEAN_AFTER" = true ] || add_nq "omega tree dirty after the inputs were stored"  # MUT:CLEAN_AFTER
[ "$UNCHANGED" = true ] || add_nq "HEAD moved during the receipt run"  # MUT:COMMIT_UNCHANGED
VERDICT=QUALIFIED; [ "$NQ" = 0 ] || VERDICT="NOT QUALIFIED"

TMP=$WORK/receipt.json
jq -S -n --arg commit "$COMMIT" --argjson clean_after "$CLEAN_AFTER" --argjson unchanged "$UNCHANGED" \
    --arg table "$TABLE_REL" --arg table_sha "$TABLE_SHA" --rawfile rows "$ROWS" --arg hw "$HW" \
    --arg tlog "$TLOG_SHA" --arg mlog "$MLOG_SHA" --arg chip "$CHIP_SHA" --rawfile bins "$BINTSV" \
    --rawfile reasons "$REASONS" --arg verdict "$VERDICT" --arg started "$STARTED" --arg finished "$FINISHED" '
    def nlines($s): $s | split("\n") | map(select(length > 0));
    {gate: "M20_OMEGA_TENSOR", writer: "tools/m20_receipt.sh v1",
     omega_commit: $commit, omega_tree_clean_before: true, omega_tree_clean_after: $clean_after,
     omega_commit_unchanged_after: $unchanged,
     qualification_table: {path: $table, sha256: $table_sha},
     rows: [nlines($rows)[] | split("\t") |
            {row: .[2], label: .[0], hardware: .[1],
             evidence_sha256: (if .[3] == "" then [] else (.[3] | split(",")) end)}],
     hardware: $hw,
     logs: {test_tensor_sha256: $tlog, test_tensor_mutations_sha256: $mlog},
     chip_evidence_sha256: (if $chip == "" then null else $chip end),
     binaries: [nlines($bins)[] | split("\t") | {name: .[0], sha256: .[1]}],
     verdict: $verdict, verdict_reasons: nlines($reasons)}
    + (if $started == "" then {} else {started_utc: $started} end)
    + (if $finished == "" then {} else {finished_utc: $finished} end)' > "$TMP" \
    || refuse "receipt json (jq) failed, no receipt written"
[ -s "$TMP" ] || refuse "empty receipt, not written"
DIG=$(sha "$TMP")
(set -o noclobber; cat "$TMP" > "$EVID_ABS/$DIG.json") 2> /dev/null || refuse "receipt $DIG.json already exists, never overwritten"  # MUT:NO_OVERWRITE
chmod 0444 "$EVID_ABS/$DIG.json"
echo "receipt: $EVID_ABS/$DIG.json verdict=$VERDICT hardware=$HW"
[ "$VERDICT" = QUALIFIED ]
