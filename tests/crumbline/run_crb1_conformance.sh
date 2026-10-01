#!/bin/bash
# CRB1 shared conformance (migration seam 1): Omega's C reader
# (src/crumbline/cl_crumb.c via crumbline-learner --decode) against the
# language-neutral corpus of aien-protocols specs/crumb-visible
# (CRUMB_READER_CONTRACT.md 1.0.0). The corpus is vendored byte for byte in
# tests/crumbline/crb1; PIN names the source commit and the manifest sha256.
#
# Every outcome line must equal the contract line, except differences listed
# in crb1_findings.txt. Those are recorded Rust-vs-C FINDINGS: neither reader
# was changed to hide them. A difference not listed fails, and a listed
# finding that no longer occurs also fails (keep the list honest).
#
# Usage: run_crb1_conformance.sh [LEARNER] [CORPUS_DIR] [FINDINGS]
set -u
LEARNER=${1:-build/crumbline-learner}
DIR=${2:-tests/crumbline/crb1}
FINDINGS=${3:-tests/crumbline/crb1_findings.txt}
PIN=$DIR/PIN

want_sha=$(sed -n 's/^manifest_sha256 //p' "$PIN")
got_sha=$(sha256sum "$DIR/expected.txt" | cut -d' ' -f1)
if [ "$want_sha" != "$got_sha" ]; then
    echo "CRB1 corpus manifest does not match PIN ($got_sha != $want_sha)"
    exit 1
fi

pass=0 fail=0 known=0 agree=0 total=0
seen=$(mktemp)
trap 'rm -f "$seen"' EXIT
while read -r name want; do
    case "$name" in '' | '#'*) continue ;; esac
    total=$((total + 1))
    got=$("$LEARNER" --decode "$DIR/$name" | sed 's/^decode-error /refuse /')
    # accept/refuse agreement is tracked separately from exact codes
    [ "${got%% *}" == "${want%% *}" ] && agree=$((agree + 1))
    if [ "$got" == "$want" ]; then
        pass=$((pass + 1))
    elif grep -qxF "$name $got" <(sed -n 's/^\([^# ][^ ]*\) \(.*\) F[0-9][0-9]*$/\1 \2/p' "$FINDINGS"); then
        known=$((known + 1))
        echo "$name" >>"$seen"
    else
        fail=$((fail + 1))
        echo "MISMATCH $name"
        echo "  want: $want"
        echo "  got:  $got"
    fi
done <"$DIR/expected.txt"

# Listed findings that did not occur are stale.
while read -r name _; do
    case "$name" in '' | '#'*) continue ;; esac
    if ! grep -qxF "$name" "$seen"; then
        fail=$((fail + 1))
        echo "STALE FINDING $name (now matches or changed; update crb1_findings.txt and the report)"
    fi
done <"$FINDINGS"

echo "CRB1 accept/refuse agreement: $agree/$total"
echo "CRUMB_VISIBLE_V1_CONFORMANCE impl=c pass=$pass fail=$fail known_findings=$known"
[ "$fail" -eq 0 ]
