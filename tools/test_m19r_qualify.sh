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

echo "evidence retention across runs (stubbed qualification, no hardware)"
check "make clean removes only the build directory" '[ "$(make --no-print-directory -C "$HERE" -n clean | tr -d "\n")" = "rm -rf build" ]'
fake=$TMP/fakeomega
mkdir -p "$fake/tools" "$fake/build"
ln -s "$HERE/src" "$fake/src"; cp "$HERE/tools/json_canon.c" "$fake/tools/"
printf "/qual-runs/\nsrc\n" > "$fake/.gitignore"
printf 'clean:\n\trm -rf build\n' > "$fake/Makefile"
git init -q "$fake"
fake_run() { # fake_run EXTRA_ARGS... ; runs m19r_main with a stubbed qualify
    (
        M19R_OMEGA=$fake
        m19r_qualify() { # like the real flow: make clean, then write evidence into the run dir
            m19r_cmd "$M19R_OMEGA" - make clean || return 1
            mkdir -p "$M19R_OMEGA/build"; echo product > "$M19R_OMEGA/build/omegatool"
            printf '  [PASS] GATE_A\n' > "$M19R_RUN_DIR/m19.log"
            echo "stub stdout line"; echo "stub stderr line" >&2
            R_STATUS=$STUB_STATUS R_EVENTS='[]' R_M19='{}'
            [ "$STUB_STATUS" = PASS ] || { m19r_fail "stub failure"; return 1; }
        }
        m19r_main --omega-candidate "$(printf 'a%.0s' {1..40})" --physics-candidate "$(printf 'b%.0s' {1..40})" \
            --physics-dir "$TMP/nophysics" "$@"
    ) > "$TMP/fake.out" 2> "$TMP/fake.err"
}
STUB_STATUS=PASS fake_run --run-id run1; rc1=$?
r1=$fake/qual-runs/run1
check "run 1 exits 0 and writes under <omega>/qual-runs (default root)" '[ "$rc1" = 0 ] && [ -f "$r1/run.json" ]'
check "run 1 has command, environment, logs, verdict, hashes" 'for f in command.txt environment.json stdout.log stderr.log verdict.json hashes.sha256 m19.log; do [ -f "$r1/$f" ] || exit 1; done'
check "stdout.log/stderr.log captured the run" 'grep -q "stub stdout line" "$r1/stdout.log" && grep -q "stub stderr line" "$r1/stderr.log"'
check "environment.json has the six fields" '[ "$(jq -r "[.hostname,.date_utc,.omega_sha,.physics_sha,.uname_r,.user] | map(length > 0) | all" "$r1/environment.json")" = true ]'
check "verdict.json says PASS, exit 0" '[ "$(jq -r "[.status,.exit_code,.run_id] | @tsv" "$r1/verdict.json")" = "$(printf "PASS\t0\trun1")" ]'
check "hashes.sha256 verifies and omits itself" '(cd "$r1" && sha256sum -c --quiet hashes.sha256) && ! grep -q hashes.sha256 "$r1/hashes.sha256"'
count1=$(find "$r1" -type f | wc -l)
sums1=$(cd "$r1" && find . -type f -print0 | sort -z | xargs -0 sha256sum)
STUB_STATUS=PASS fake_run --run-id run2; rc2=$?
check "run 2 (which runs make clean) exits 0" '[ "$rc2" = 0 ] && [ -f "$fake/qual-runs/run2/run.json" ]'
check "run 2 cleanup removed build products but not run 1" '[ -d "$r1" ] && [ -f "$fake/build/omegatool" ]'
check "run 1 hashes still verify after run 2" '(cd "$r1" && sha256sum -c --quiet hashes.sha256)'
check "run 1 file count unchanged" '[ "$(find "$r1" -type f | wc -l)" = "$count1" ]'
check "run 1 bytes identical (every file)" '[ "$(cd "$r1" && find . -type f -print0 | sort -z | xargs -0 sha256sum)" = "$sums1" ]'
STUB_STATUS=PASS fake_run --run-id run1; rcx=$?
check "reusing an existing run id exits 2" '[ "$rcx" = 2 ] && grep -q "never reused" "$TMP/fake.err"'
check "  and leaves run 1 untouched" '(cd "$r1" && sha256sum -c --quiet hashes.sha256) && [ "$(find "$r1" -type f | wc -l)" = "$count1" ]'
STUB_STATUS=PASS fake_run --run-id inbuild --evidence-root "$fake/build/qual"; rcb=$?
check "evidence root inside build/ refused (exit 2)" '[ "$rcb" = 2 ]'
STUB_STATUS=FAILED fake_run --run-id run3; rc3=$?
check "failed run exits 1 with FAILED verdict and exit code" '[ "$rc3" = 1 ] && [ "$(jq -r "[.status,.exit_code,.failing_step,.failing_gate_or_exit] | @tsv" "$fake/qual-runs/run3/verdict.json")" = "$(printf "FAILED\t1\tstub failure\texit:1")" ]'
check "command.txt records the exact argv" 'grep -q -- "--run-id run3" "$fake/qual-runs/run3/command.txt"'

echo "campaign verdicts (tools/m19r_campaign.sh)"
CAMP=$HERE/tools/m19r_campaign.sh
mkrun() { # mkrun DIR ID STATUS -- a sealed run directory
    local d=$1/$2
    mkdir -p "$d"
    printf '{"status":"%s"}' "$3" > "$d/run.json"
    printf '{"run_id":"%s","status":"%s","exit_code":0}' "$2" "$3" > "$d/verdict.json"
    echo "log $2" > "$d/m19.log"
    m19r_seal "$d"
}
verdict() { "$CAMP" --campaign-dir "$1" --runs "$2" > /dev/null 2>&1; jq -r .verdict "$1/final-verdict.json"; }
c=$TMP/c_pass; mkdir "$c"; mkrun "$c" r1 PASS; mkrun "$c" r2 PASS
check "PASS: 2 of 2 runs PASS" '[ "$(verdict "$c" 2)" = PASS ]'
check "  campaign.json written with sha, rule, created_at" '[ "$(jq -r "[.sha,.rule,.created_at] | map(length > 0) | all" "$c/campaign.json")" = true ]'
check "  exits 0" '"$CAMP" --campaign-dir "$c" --runs 2 > /dev/null'
check "INCOMPLETE: 2 usable runs but 3 required" '[ "$(verdict "$c" 3)" = INCOMPLETE ]'
check "  exits 1" '! "$CAMP" --campaign-dir "$c" --runs 3 > /dev/null'
c=$TMP/c_fail; mkdir "$c"; mkrun "$c" r1 PASS; mkrun "$c" r2 FAILED
check "FAIL: one run FAILED" '[ "$(verdict "$c" 2)" = FAIL ]'
c=$TMP/c_inv; mkdir "$c"; mkrun "$c" r1 PASS; mkrun "$c" r2 PASS
chmod u+w "$c/r2/m19.log"; printf 'log r3' > "$c/r2/m19.log"
check "INVALID: one altered byte in a run file" '[ "$(verdict "$c" 2)" = INVALID ]'
c=$TMP/c_inv2; mkdir "$c"; mkrun "$c" r1 PASS; mkrun "$c" r2 PASS; rm "$c/r2/verdict.json"
check "INVALID: run lacks verdict.json" '[ "$(verdict "$c" 2)" = INVALID ]'
c=$TMP/c_inv3; mkdir "$c"; mkrun "$c" r1 PASS; mkrun "$c" r2 FAILED; chmod u+w "$c/r1/m19.log"; echo x >> "$c/r1/m19.log"
check "INVALID takes precedence over FAIL" '[ "$(verdict "$c" 2)" = INVALID ]'
check "bad arguments exit 2" '"$CAMP" --campaign-dir "$TMP/nonexistent" --runs 2 > /dev/null 2>&1; [ $? = 2 ]'

if [ "$fails" -ne 0 ]; then
    echo "M19R qualifier tests: $fails FAILED"
    exit 1
fi
echo "M19R qualifier tests: all passed"
