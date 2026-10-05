#!/bin/bash
# test_chipwait_campaign.sh -- host-only tests of tools/chipwait_campaign.sh
# with a STUB qualify script (CHIPWAIT_QUALIFY). No GPU, no real qualifier.
# Run: make test-chipwait-campaign   (or run this file).
set -u
HERE=$(cd -P "$(dirname "$0")/.." && pwd)
CW=$HERE/tools/chipwait_campaign.sh
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fails=0
ok() { echo "  [PASS] $1"; }
bad() { echo "  [FAIL] $1"; fails=$((fails + 1)); }
check() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# Stub qualify. Per-run behaviour from env STUB_STATUS_<n> (default PASS):
# PASS/FAIL write a valid verified run dir; NODIR writes nothing; both exit
# nonzero for FAIL/NODIR. STUB_NEED_CAMPAIGN_JSON=1 records whether
# campaign.json existed at invocation time.
STUB=$TMP/stub_qualify.sh
cat > "$STUB" <<'STUBEOF'
#!/bin/bash
root= id=
while [ $# -gt 0 ]; do
    case $1 in --evidence-root) root=$2;; --run-id) id=$2;; esac
    shift
done
n=${id#run-}; n=$((10#$n))
[ -e "$root/campaign.json" ] && echo "$id saw campaign.json" >> "$root/../stub-order.log" ||
    echo "$id NO campaign.json" >> "$root/../stub-order.log"
echo "stub stdout $id"; echo "stub stderr $id" >&2
v=STUB_STATUS_$n; s=${!v:-PASS}
[ "$s" = NODIR ] && exit 3
mkdir "$root/$id" || exit 2
echo '{"stub":true}' > "$root/$id/run.json"
printf '{"status":"%s"}\n' "$s" > "$root/$id/verdict.json"
(cd "$root/$id" && sha256sum run.json verdict.json > hashes.sha256)
[ "$s" = PASS ]
STUBEOF
chmod +x "$STUB"
export CHIPWAIT_QUALIFY=$STUB
S40=$(printf 'a%.0s' $(seq 40)); P40=$(printf 'b%.0s' $(seq 40))
run() { # run <dir> [extra args]; sets rc
    local d=$1; shift
    "$CW" --omega-candidate "$S40" --physics-candidate "$P40" --physics-dir "$TMP/phys" --campaign-dir "$d" "$@" \
        > "$TMP/out.log" 2> "$TMP/err.log"
    rc=$?
}
verdict() { jq -r .verdict "$1/final-verdict.json"; }

echo "PASS: three PASS runs"
c=$TMP/pass; run "$c"
check "exit 0" '[ $rc = 0 ]'
check "final-verdict PASS" '[ "$(verdict "$c")" = PASS ]'
check "three run dirs" '[ "$(ls -d "$c"/run-00?/ | wc -l)" = 3 ]'
check "campaign.json written before the first run" '[ "$(head -n1 "$TMP/stub-order.log")" = "run-001 saw campaign.json" ] && ! grep -q NO "$TMP/stub-order.log"'
check "campaign.json fields" 'jq -e --arg o "$S40" --arg p "$P40" ".omega_candidate_sha==\$o and .physics_candidate_sha==\$p and .rule==\"3/3 PASS, predeclared\" and .runs_required==3 and (.created_at|test(\"Z\$\")) and (.host|length>0) and (.uname|length>0) and has(\"runner_sha\")" "$c/campaign.json" > /dev/null'
check "stdout log captured" 'grep -q "stub stdout run-002" "$c/runner-run-002.stdout.log"'
check "stderr log captured" 'grep -q "stub stderr run-002" "$c/runner-run-002.stderr.log"'
check "output also shown on terminal" 'grep -q "stub stdout run-003" "$TMP/out.log" && grep -q "stub stderr run-003" "$TMP/err.log"'
check "exit codes recorded" '[ "$(cat "$c/runner-run-001.exit")" = 0 ]'

echo "FAIL: run 2 fails, campaign continues"
c=$TMP/fail; STUB_STATUS_2=FAIL run "$c"
check "exit 1" '[ $rc = 1 ]'
check "final-verdict FAIL" '[ "$(verdict "$c")" = FAIL ]'
check "run 3 still ran" '[ -d "$c/run-003" ] && [ "$(cat "$c/runner-run-003.exit")" = 0 ]'
check "failing exit code recorded" '[ "$(cat "$c/runner-run-002.exit")" = 1 ]'

echo "INCOMPLETE: run 3 writes no run dir (m19r_campaign.sh mechanically gives INCOMPLETE)"
c=$TMP/inc; STUB_STATUS_3=NODIR run "$c"
check "exit nonzero (1)" '[ $rc = 1 ]'
check "final-verdict INCOMPLETE" '[ "$(verdict "$c")" = INCOMPLETE ]'
check "exit code 3 recorded" '[ "$(cat "$c/runner-run-003.exit")" = 3 ]'

echo "runs count"
c=$TMP/two; run "$c" --runs 2
check "--runs 2 passes with 2 runs, rule text derived" '[ $rc = 0 ] && [ "$(jq -r .rule "$c/campaign.json")" = "2/2 PASS, predeclared" ] && [ ! -d "$c/run-003" ]'

echo "refusal to reuse a campaign dir"
find "$TMP/pass" -type f | sort | xargs sha256sum > "$TMP/before.sums"
run "$TMP/pass"
check "exit 2" '[ $rc = 2 ]'
check "existing files byte-identical" 'find "$TMP/pass" -type f | sort | xargs sha256sum | cmp -s - "$TMP/before.sums"'
mkdir -p "$TMP/partial/run-001"; echo keep > "$TMP/partial/run-001/x"
run "$TMP/partial"
check "lone run-001 dir refused (exit 2) and untouched" '[ $rc = 2 ] && [ "$(cat "$TMP/partial/run-001/x")" = keep ] && [ ! -e "$TMP/partial/campaign.json" ]'

echo "bad arguments"
check "no arguments" '"$CW" > /dev/null 2>&1; [ $? = 2 ]'
check "runs 0" 'run "$TMP/b1" --runs 0; [ $rc = 2 ]'
check "runs abc" 'run "$TMP/b2" --runs abc; [ $rc = 2 ]'
check "runs -1" 'run "$TMP/b3" --runs -1; [ $rc = 2 ]'
check "missing candidate" '"$CW" --physics-candidate x --physics-dir y --campaign-dir "$TMP/b4" > /dev/null 2>&1; [ $? = 2 ]'
check "unknown flag" '"$CW" --bogus > /dev/null 2>&1; [ $? = 2 ]'
check "error text on stderr" '"$CW" --runs 0 2>&1 >/dev/null | grep -q usage'
check "bad usage created no campaign dir" '[ ! -e "$TMP/b1" ] && [ ! -e "$TMP/b2" ]'

echo "nothing deleted"
find "$TMP" -type f | sort > "$TMP/files.before"
run "$TMP/after-all" ; run "$TMP/fail" ; run "$TMP/inc"
find "$TMP" -type f | sort > "$TMP/files.after"
check "file list before is a subset of after" '[ -z "$(comm -23 "$TMP/files.before" "$TMP/files.after")" ]'

if [ "$fails" -ne 0 ]; then
    echo "chipwait campaign tests: $fails FAILED"
    exit 1
fi
echo "chipwait campaign tests: all passed"
