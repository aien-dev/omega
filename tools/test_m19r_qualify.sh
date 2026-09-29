#!/bin/bash
# test_m19r_qualify.sh -- negative tests for candidate binding, immutable
# qualification emission and gate counting in tools/m19r_qualify.sh, plus
# golden tests for the canonical JSON helper tools/json_canon.c.
# Host only (no GPU). Run: make test-m19r-qualify   (or run this file).
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
# shellcheck source=tools/m19r_qualify.sh
. "$HERE/tools/m19r_qualify.sh"
m19r_build_canon || { echo "FAIL: cannot build tools/json_canon.c"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP" "$M19R_TMP"' EXIT
fails=0
ok() { echo "  [PASS] $1"; }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

echo "candidate mismatch and dirty tree"
repo=$TMP/repo
git init -q "$repo"
echo candidate > "$repo/source.txt"
git -C "$repo" add source.txt
git -C "$repo" -c user.name=Test -c user.email=test@example.invalid commit -qm candidate
sha=$(git -C "$repo" rev-parse HEAD)
check "all-zero SHA rejected" '! m19r_must_candidate "$repo" 0000000000000000000000000000000000000000'
check "  with the HEAD message" '[ "$M19R_ERR" = "repo HEAD differs from supplied candidate" ]'
check "short SHA rejected" '! m19r_must_candidate "$repo" "${sha:0:12}"'
check "  with the full-SHA message" '[ "$M19R_ERR" = "candidate must be an explicit full 40-hex SHA" ]'
check "exact clean candidate accepted" 'm19r_must_candidate "$repo" "$sha"'
check "upper-case candidate accepted" 'm19r_must_candidate "$repo" "${sha^^}"'
echo dirty > "$repo/source.txt"
check "dirty tree rejected" '! m19r_must_candidate "$repo" "$sha"'
check "  with the dirty message" '[ "$M19R_ERR" = "repo candidate tree is dirty" ]'
git -C "$repo" checkout -q source.txt
echo new > "$repo/untracked.txt"
check "untracked file counts as dirty" '! m19r_must_candidate "$repo" "$sha"'

echo "exclusive receipt and canonical digest"
body='{"count": 2, "candidate": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}'
h=$(printf '%s' "$body" | "$JSON_CANON" --sha256)
check "digest of the canonical body" '[ "$h" = "$(printf "%s" "{\"candidate\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"count\":2}" | sha256sum | cut -d" " -f1)" ]'
receipt="{\"receipt_digest\":\"$h\",${body#\{}"
path=$TMP/M19R/$h.json
check "first write succeeds" 'printf "%s" "$receipt" | m19r_write_immutable_receipt "$path"'
original=$(sha256sum < "$path")
check "file is read-only (0444)" '[ "$(stat -c %a "$path")" = 444 ]'
check "second write refused" '! printf "%s" "$receipt" | m19r_write_immutable_receipt "$path"'
check "  as an existing file" '[ "$(cat "$M19R_TMP/error")" = "[Errno 17] File exists: '"'"'$path'"'"'" ]'
check "original bytes unchanged" '[ "$(sha256sum < "$path")" = "$original" ]'

echo "failed gate and derived final count"
printf '  [PASS] GATE_ONE\n  [FAIL] GATE_TWO\n' > "$TMP/gates.log"
m19r_events "$TMP/gates.log" suite > "$TMP/seen.tsv"
check "counts derived from observed lines" '[ "$(m19r_observed_counts < "$TMP/seen.tsv")" = "{\"completed\":2,\"failed\":1,\"passed\":1}" ]'
check "a failed gate fails the run" '! m19r_require_passed < "$TMP/seen.tsv"'
check "no observed gates fails the run" '! m19r_require_passed < /dev/null'
head -n 1 "$TMP/seen.tsv" > "$TMP/one.tsv"
check "first gate alone counts 1" '[ "$(m19r_observed_counts < "$TMP/one.tsv")" = "{\"completed\":1,\"failed\":0,\"passed\":1}" ]'
check "first gate alone passes" 'm19r_require_passed < "$TMP/one.tsv"'
printf 'SILICON_PASS: FAIL\nOTHER_PASS: measured true\n' > "$TMP/named.log"
m19r_events "$TMP/named.log" named > "$TMP/named.tsv"
check "named lines: FAIL word wins" '[ "$(cut -f3 "$TMP/named.tsv" | paste -sd, -)" = "FAIL,PASS" ]'
check "named FAIL fails the run" '! m19r_require_passed < "$TMP/named.tsv"'
printf 'XY_PASS: y\n  [PASS]\n\n   LATE_ID trailing\n[PASS]FOO\n[PASS] a:b-c(d)\n A__PASS: FAILED\nA_PASS: x\n' > "$TMP/order.log"
check "gate lines first, id after blank lines, then named lines" '[ "$(m19r_events "$TMP/order.log" s | m19r_events_json)" = "[{\"suite\":\"s\",\"id\":\"LATE_ID\",\"status\":\"PASS\"},{\"suite\":\"s\",\"id\":\"a:b-c\",\"status\":\"PASS\"},{\"suite\":\"s\",\"id\":\"XY_PASS\",\"status\":\"PASS\"}]" ]'

echo "tagged observations"
printf 'M19_OBSERVED_JSON:{"a":1}\nnoise\nM19_OBSERVED_JSON:{"a":2}\n' > "$TMP/tag.log"
check "every tagged line returned" '[ "$(m19r_tagged_json "$TMP/tag.log" M19_OBSERVED_JSON | tail -n 1)" = "{\"a\":2}" ]'
check "missing tag fails" '! m19r_tagged_json "$TMP/tag.log" M19R_SOAK_JSON > /dev/null'
printf 'M19R_SOAK_JSON:{"a":\n' > "$TMP/badtag.log"
check "malformed tagged JSON fails" '! m19r_tagged_json "$TMP/badtag.log" M19R_SOAK_JSON > /dev/null'

echo "canonical JSON matches Python json.dumps"
trap_doc='{"z":[84.830,85.000,1e3,0,-0,-0.0,18446744073709551615,1e16,1e-05,0.0001,1.5e-7],"b":{"e":{},"f":[]},"s":"q\"b\\s\u0001\n\u00e9\u007f\ud83d\ude00","a":1,"a":"dup-last"}'
compact=$(printf '%s' "$trap_doc" | "$JSON_CANON")
want=$(printf '{"a":"dup-last","b":{"e":{},"f":[]},"s":"q\\"b\\\\s\\u0001\\n\xc3\xa9\x7f\xf0\x9f\x98\x80","z":[84.83,85.0,1000.0,0,0,-0.0,18446744073709551615,1e+16,1e-05,0.0001,1.5e-07]}')
check "compact form (numbers, escapes, raw UTF-8, dup key)" '[ "$compact" = "$want" ]'
pretty=$(printf '%s' "$trap_doc" | "$JSON_CANON" --pretty)
want_pretty='{
  "a": "dup-last",
  "b": {
    "e": {},
    "f": []
  },
  "s": "q\"b\\s\u0001\n\u00e9\u007f\ud83d\ude00",
  "z": [
    84.83,
    85.0,
    1000.0,
    0,
    0,
    -0.0,
    18446744073709551615,
    1e+16,
    1e-05,
    0.0001,
    1.5e-07
  ]
}'
check "pretty form (indent 2, ASCII escapes)" '[ "$pretty" = "$want_pretty" ]'
check "invalid JSON rejected" '! printf "{\"a\":1,}" | "$JSON_CANON" > /dev/null 2>&1'
rec=$(ls "$HERE"/evidence/M19R/*.json | head -n 1)
name=$(basename "$rec" .json)
check "recorded M19R receipt digest reproduced" '[ "$(jq "del(.receipt_digest)" "$rec" | "$JSON_CANON" --sha256)" = "$name" ]'
check "recorded M19R receipt pretty form reproduced" '"$JSON_CANON" --pretty < "$rec" | cmp -s - "$rec"'

if [ "$fails" -ne 0 ]; then
    echo "M19R qualifier tests: $fails FAILED"
    exit 1
fi
echo "M19R qualifier tests: all passed"
