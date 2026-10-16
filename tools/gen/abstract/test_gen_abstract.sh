#!/usr/bin/env bash
# tools/gen/abstract/test_gen_abstract.sh <build dir> -- tests of GEN-3 (aien-spark/omega#478):
#   1 recheck: the schema v1 fixtures pass; every format mutant is refused with its reason
#   2 corpus: files and prompt bytes, the C oracle reproduces every expected file, FAMSIG rule, the harness
#     (tests/one_layer/acceptance/ola.sh experiment) accepts every solution and every reference
#   3 extract: two runs are byte-identical and equal the committed record set; the expected rejections
#   4 verify: every committed record is VERIFIED
#   5 transfer probes (tools/gen/abstract/probe): every positive and negative probe passes
#   6 verify mutants: each must be BLOCKED with its reason
# <build dir> holds gen_abstract/{recheck,gabs,oracle,famsig}, omegese, omega_emit, checker2, one_layer.
# Every check has a negative control or a mutant that must fail it. CPU only; no model, no GPU.
set -euo pipefail
export LC_ALL=C
B="${1:?usage: test_gen_abstract.sh <build dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
RC="$B/gen_abstract/recheck"
FX="$HERE/fixtures"
T="$(mktemp -d "${TMPDIR:-/tmp}/gen-abstract-test.XXXXXXXX")"
trap 'rm -rf "$T"' EXIT
fails=0 oks=0
ok()  { echo "ok   $*"; oks=$((oks + 1)); }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# ---- 1. recheck ------------------------------------------------------------------------------------------
if "$RC" --root "$ROOT" --receipts "$FX/receipts" "$FX/abs-fixture.rec" "$FX/raw-fixture.rec" > "$T/ok.out"; then
  ok "recheck: both schema v1 fixtures (abstraction, raw) accepted with --root and --receipts"
else bad "recheck fixtures: $(cat "$T/ok.out")"; fi
# mutant <name> <fixture> <expected reason substring> <recheck options...> -- reads the mutated record from $T/m.rec
mut() {
  local name="$1" want="$2"; shift 2
  local out; out="$("$RC" "$@" "$T/m.rec" 2>&1 || true)"
  case "$out" in REFUSED*"$want"*) ok "  mutant refused: $name";; *) bad "mutant $name not refused as expected (want '$want'): $out";; esac
}
A="$FX/abs-fixture.rec" W="$FX/raw-fixture.rec"
sed '1s/.*/gen-record 2/' "$A" > "$T/m.rec"; mut "wrong header" "line 1 must be"
sed 's/^id abs-0123456789abcdef/id abs-0123456789abcdee/' "$A" > "$T/m.rec"; mut "id not from fingerprint" "first 16 hex of the fingerprint"
sed 's/^kind abstraction/kind helper/' "$A" > "$T/m.rec"; mut "unknown kind" "kind raw"
sed 's/^split corpus/split probe/' "$A" > "$T/m.rec"; mut "abstraction from a probe" "split corpus"
sed 's/^family \(gs1-[0-9a-f]*\) \(gs1-[0-9a-f]*\)$/family \2 \1/' "$A" > "$T/m.rec"; mut "family not ascending" "family"
sed 's/^params u32 u32 u32/params u32 u32 i32/' "$A" > "$T/m.rec"; mut "wrong param type" "params differ"
sed 's/^ret u32/ret u64/' "$A" > "$T/m.rec"; mut "wrong return type" "ret differs"
sed 's/^entry clampv/entry clamp/' "$A" > "$T/m.rec"; mut "entry not the signature name" "entry differs"
sed 's/^signature fn clampv x uint lo uint hi uint to uint/signature fn clampv x uint lo uint hi int to uint/' "$A" > "$T/m.rec"; mut "signature types differ from body" "params differ"
sed '0,/^fn clampv x uint lo uint hi uint to uint/{s/^fn clampv x uint lo uint hi uint to uint/fn clampv x uint lo uint hi uint to ulong/}' "$A" | sed 's/^signature .*/signature fn clampv x uint lo uint hi uint to uint/' > "$T/m.rec"
  sed -i '/^body$/,$ s/^fn clampv x uint lo uint hi uint to uint$/fn clampv x uint lo uint hi uint to ulong/' "$T/m.rec"; mut "body fn line differs from signature" "signature differs from the body"
sed 's/^pre STATED+TESTED/pre PROVED/' "$A" > "$T/m.rec"; mut "precondition claimed PROVED" "pre must be tagged STATED+TESTED"
sed 's/^post STATED+TESTED .*/post STATED+TESTED/' "$A" > "$T/m.rec"; mut "empty postcondition" "post"
sed 's/^example 12 3 9 -> 9/example 12 3 -> 9/' "$A" > "$T/m.rec"; mut "example arity" "wrong number of arguments"
sed 's/^counter 5 9 3 -> 9/counter 5 9 3 -> TRAP 15/' "$A" > "$T/m.rec"; mut "counterexample trap code" "TRAP <1..14>"
sed '/^limit /d' "$A" > "$T/m.rec"; mut "no limit" "at least one 'limit"
sed '/^ancestry fx_b /d' "$A" > "$T/m.rec"; mut "single-source helper" ">= 2 distinct corpus solutions"
sed "s/^ancestry fx_b .*/ancestry fx_b $(sed -n 's/^ancestry fx_a //p' "$A")/" "$A" > "$T/m.rec"; mut "two ancestors, one solution digest" ">= 2 distinct corpus solutions"
fp="$(sed -n 's/^fingerprint //p' "$A")"
sed "s/^differs fx_b .*/differs fx_b $fp/" "$A" > "$T/m.rec"; mut "helper identical to a whole corpus solution" "equals the whole corpus solution"
sed '/^differs fx_b /d' "$A" > "$T/m.rec"; mut "one differs line" "differs"
sed 's/^source \(.*\) [0-9a-f]*$/source \1 '"$(printf stale | sha256sum | cut -c1-64)"'/' "$A" > "$T/m.rec"; mut "stale source digest" "stale source digest" --root "$ROOT"
sed 's|^source tools/gen/abstract/fixtures/source.txt|source ../x/source.txt|' "$A" > "$T/m.rec"; mut "source path with .." "relative path"
fake="$(printf 'gen-receipt 1\nrecord abs-0123456789abcdef\nfixture forged\n' | sha256sum | cut -c1-64)"
sed "s/^receipt .*/receipt $fake/" "$A" > "$T/m.rec"; mut "receipt id with no receipt file" "receipt file" --receipts "$FX/receipts"
mkdir -p "$T/rc"; cp "$FX/receipts/"* "$T/rc/"; r="$(sed -n 's/^receipt //p' "$A")"; printf 'extra\n' >> "$T/rc/$r"; cp "$A" "$T/m.rec"; mut "edited receipt file" "does not hash to its id" --receipts "$T/rc"
rw="$(sed -n 's/^receipt //p' "$W")"; sed "s/^receipt .*/receipt $rw/" "$A" > "$T/m.rec"; mut "another record's receipt" "not this record's" --receipts "$FX/receipts"
sed 's/^validity unverified/validity trusted/' "$A" > "$T/m.rec"; mut "unknown validity" "validity"
printf '\n' | cat "$A" - > "$T/m.rec"; mut "trailing empty line" "empty line"
sed 's/^desc /desc  /' "$A" > "$T/m.rec"; mut "two spaces" "two spaces"
sed 's/^desc \(.*\)$/desc \1\r/' "$A" > "$T/m.rec"; mut "CR byte" "outside ASCII"
sed '$d' "$A" > "$T/m.rec"; mut "body not closed by end" "must end with 'end'"
sed '/^body$/,$ s/^ret x$/ret x\nend\nfn other a uint to uint\nret a/' "$A" > "$T/m.rec"; mut "two functions in an abstraction body" "exactly one function"
sed 's/^fn clampv x uint lo uint hi uint to uint$/fn clampv res uint lo uint hi uint to uint/; s/^signature fn clampv x /signature fn clampv res /' "$A" > "$T/m.rec"; mut "parameter named res" "res is reserved"
sed 's/^kind raw/kind abstraction/' "$W" > "$T/m.rec"; mut "raw record relabelled abstraction" "abstraction id"
sed 's/^ancestry fx_clamp /ancestry fx_other /' "$W" > "$T/m.rec"; mut "raw ancestry not its own task" "its own task"
sed 's/^signature fn fx_clamp a uint to uint/signature fn clampv x uint lo uint hi uint to uint/; s/^entry fx_clamp/entry clampv/; s/^params u32$/params u32 u32 u32/' "$W" > "$T/m.rec"; mut "raw signature not the entry (last fn)" "last fn line"
grep -v '^ancestry ' "$A" | sed 's/^validity unverified/validity unverified\nancestry fx_a '"$(printf a | sha256sum | cut -c1-64)"'/' > "$T/m.rec"; mut "single ancestry line" ">= 2 distinct"
"$RC" > /dev/null 2>&1 && bad "recheck with no record must be a usage error" || { [ $? = 2 ] && ok "  usage error (exit 2) without a record" || bad "recheck usage exit code"; }


# ---- 2. corpus ------------------------------------------------------------------------------------------
CO="$ROOT/tests/one_layer/generalization/corpus"
RS="$ROOT/tests/one_layer/generalization/records"
recof() { local f; for f in "$RS"/abstraction/*.rec; do if grep -qx "entry $1" "$f"; then echo "$f"; fi; done; }
ntask="$(ls "$CO/tasks" | wc -l)"
[ "$ntask" -ge 48 ] && ok "corpus: $ntask tasks" || bad "corpus has $ntask tasks (want >= 48)"
cbad=""
for d in "$CO"/tasks/*/; do
  t="$(basename "$d")"
  for f in requirement prompt desc inputs expected solution.omegese reference.osc; do [ -s "$d/$f" ] || cbad+=" $t/$f"; done
  LC_ALL=C grep -q '[^ -~]' "$d/prompt" && cbad+=" $t/prompt-bytes"
  [ "$(wc -c < "$d/prompt")" -le 4096 ] || cbad+=" $t/prompt-size"
  [ "$(wc -l < "$d/inputs")" -ge 8 ] || cbad+=" $t/inputs<8"
  grep -q '^kind task$' "$d/requirement" && grep -q "^entry $t\$" "$d/requirement" || cbad+=" $t/requirement"
  "$B/gen_abstract/oracle" "$t" < "$d/inputs" | cmp -s - "$d/expected" || cbad+=" $t/expected-not-oracle"
done
[ -z "$cbad" ] && ok "corpus: every task has its files, prompt bytes, >= 8 inputs, and expected == C oracle" || bad "corpus:$cbad"
sed '1s/^\(.*\)$/9999999/' "$CO/tasks/clamp_sum/inputs" | "$B/gen_abstract/oracle" clamp_sum | cmp -s - "$CO/tasks/clamp_sum/expected" \
  && bad "oracle negative control: a changed input still matched" || ok "  negative control: one changed input changes the oracle output"
if [ -f "$ROOT/tests/one_layer/generalization/dev/META" ]; then cp "$ROOT/tests/one_layer/generalization/dev/META" "$T/devMETA"
else git -C "$ROOT" show 8255a59031146e9f6de535425d591e0fdcee5b8d:tests/one_layer/generalization/dev/META > "$T/devMETA"; fi
FS="$B/gen_abstract/famsig"
"$FS" rule "$T/devMETA" "$CO/META" > "$T/rule.out" 2>&1 && tail -n 1 "$T/rule.out" | grep -qx 'RULE PASS' \
  && ok "famsig rule dev/META corpus/META: RULE PASS ($(grep -c . "$CO/META") META lines, $(sed -n 's/^split corpus .*families=//p' "$T/rule.out") corpus families)" \
  || bad "famsig rule: $(tail -n 2 "$T/rule.out")"
prims=""; for p in arith branch loop array call nested; do grep -v '^#' "$CO/META" | grep -q "$p" || prims+=" $p"; done
[ -z "$prims" ] && ok "corpus META covers all six primitives" || bad "corpus META misses:$prims"
{ cat "$CO/META"; echo "zz_reserved corpus 1 0 2 0 0 0 loop(loop(branch(arith)))"; } > "$T/badMETA"
"$FS" rule "$T/devMETA" "$T/badMETA" > /dev/null 2>&1 && bad "famsig rule accepted a reserved dev shape in the corpus" || ok "  negative control: a reserved dev shape in corpus META fails the rule"
cmp -s <(grep -v '^#' "$CO/META" | tail -n +2 | cut -d' ' -f1) <(ls "$CO/tasks") && ok "corpus META has exactly one line per task" || bad "corpus META lines differ from the task list"
# the harness, called (not copied): every solution IR as a candidate, plus a one-task mutant set
H="$T/ola"; mkdir -p "$H/tasks/cases" "$H/cands/solutions" "$H/cands/mutant/wrap8" "$H/ev"
for d in "$CO"/tasks/*/; do
  t="$(basename "$d")"; mkdir -p "$H/tasks/cases/$t/hidden" "$H/cands/solutions/$t"
  cp "$d/requirement" "$d/prompt" "$H/tasks/cases/$t/"; cp "$d/inputs" "$d/expected" "$d/reference.osc" "$H/tasks/cases/$t/hidden/"
  "$B/omegese/omegese" emit "$d/solution.omegese" "$T/s.json" > /dev/null && "$B/omega_emit/omega_emit" emit "$T/s.json" "$H/cands/solutions/$t/candidate.oscir" > /dev/null
done
sed '1,/^body$/d' "$(recof sat8)" > "$T/m.omegese"
cat "$CO/tasks/wrap8/misuse.sat8.omegese" >> "$T/m.omegese"
"$B/omegese/omegese" emit "$T/m.omegese" "$T/m.json" > /dev/null && "$B/omega_emit/omega_emit" emit "$T/m.json" "$H/cands/mutant/wrap8/candidate.oscir" > /dev/null
(cd "$H" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf tasks.tar tasks && sha256sum tasks.tar > TASKS.sha256)
OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$H/ev" bash "$ROOT/tests/one_layer/acceptance/ola.sh" experiment "$H/tasks.tar" "$H/TASKS.sha256" "$H/cands" > "$H/out" 2> "$H/err" || true
grep -q "^set=solutions accepted=$ntask/$ntask rejected=0 errors=0" "$H/out" && ok "harness: every Omegese solution accepted ($ntask/$ntask)" || bad "harness solutions: $(grep '^set=solutions' "$H/out")"
grep -q "^references_accepted=$ntask/$ntask$" "$H/out" && ok "harness: every OSC reference accepted ($ntask/$ntask)" || bad "harness references: $(grep '^references' "$H/out")"
mv="$(find "$H/ev" -path '*/sets/mutant/verdicts.txt' | head -n 1)"
grep -q "^set=mutant accepted=0/$ntask " "$H/out" && grep -q '^wrap8 REJECT wrong_result' "$mv" && ok "  negative control: the harness rejects the wrap8 candidate built from a misused record (wrong_result)" || bad "harness mutant: $(grep '^set=mutant' "$H/out") $(grep '^wrap8' "$mv")"

# ---- 3. extract -----------------------------------------------------------------------------------------
X="$HERE/extract"
bash "$X" "$B" "$T/r1" > "$T/x1.out" 2>&1 && bash "$X" "$B" "$T/r2" > /dev/null 2>&1 || bad "extract failed: $(tail -n 2 "$T/x1.out")"
diff -r "$T/r1" "$T/r2" > /dev/null && ok "extract: two runs are byte-identical ($(cat "$T/x1.out"))" || bad "extract is not deterministic"
diff -r "$T/r1" "$RS" > /dev/null && ok "extract: output equals the committed record set (reconstructible from pinned sources)" || bad "committed records differ from a fresh extract: $(diff -rq "$T/r1" "$RS" | head -n 3)"
for want in "^cube .* single-source: occurs in 1 extraction task$" "^dist .* single-source: occurs in 1 extraction task$" \
            "^ds2 - .* calls digsum: v1 extracts leaf helpers only$" "^gcd .* identical to the whole corpus solution of gcd_only$"; do
  grep -q "$want" "$RS/REJECTIONS" && ok "  rejection logged: $(grep "$want" "$RS/REJECTIONS" | cut -d' ' -f1,4-)" || bad "REJECTIONS lacks /$want/"
done
for n in clamp absdiff sat8 digsum digcount popcnt sum4 max4 avg2; do
  grep -qx "entry $n" "$RS"/abstraction/*.rec && ok "  abstraction record: $n" || bad "no abstraction record for $n"
done
"$RC" --root "$ROOT" --receipts "$RS/receipts" "$RS"/raw/*.rec "$RS"/abstraction/*.rec > "$T/rc.out" 2>&1 && ok "recheck: all $(grep -c '^OK' "$T/rc.out") committed records" || bad "recheck committed: $(grep -v '^OK' "$T/rc.out" | head -n 2)"
(cd "$RS" && sha256sum --quiet -c MANIFEST && sha256sum --quiet -c SET.sha256) && ok "MANIFEST and SET.sha256 match the record files" || bad "MANIFEST or SET.sha256 stale"

# ---- 4. verify ------------------------------------------------------------------------------------------
V="$HERE/verify"
bash "$V" "$B" "$RS"/raw/*.rec "$RS"/abstraction/*.rec > "$T/v.out" 2>&1 \
  && ok "verify: $(grep -c '^VERIFIED' "$T/v.out") records VERIFIED" || bad "verify: $(grep -v '^VERIFIED' "$T/v.out" | head -n 3)"

# ---- 5. probes ------------------------------------------------------------------------------------------
bash "$HERE/probe" "$B" "$RS" > "$T/p.out" 2>&1 && ok "probes: $(tail -n 1 "$T/p.out") ($(grep -c ' positive .* PASS' "$T/p.out") positive, $(grep -c ' negative .* PASS' "$T/p.out") negative)" \
  || bad "probes: $(grep -v ' PASS ' "$T/p.out" | head -n 3)"
for n in clamp absdiff sat8 digsum digcount popcnt sum4 max4 avg2; do
  grep -q " positive $n PASS" "$T/p.out" && grep -q " negative $n PASS" "$T/p.out" || bad "record $n lacks a passing positive and negative probe"
done

# ---- 6. verify mutants ----------------------------------------------------------------------------------
CL="$(recof clamp)" RW="$RS/raw/raw-clamp_sum.rec"
# vmut <name> <want> <record> [verify options] -- the record must be BLOCKED with <want> in the reason
vmut() {
  local name="$1" want="$2" rec="$3"; shift 3
  local out; out="$(bash "$V" "$@" "$B" "$rec" 2>&1 || true)"
  case "$out" in "BLOCKED "*"$want"*) ok "  mutant blocked: $name";; *) bad "mutant $name not blocked as expected (want '$want'): $out";; esac
}
cp "$CL" "$T/v.rec"; out="$(bash "$V" --receipts "$RS/receipts" "$B" "$T/v.rec" || true)"; [ "$out" = "VERIFIED $(sed -n 's/^id //p' "$CL")" ] && ok "  control: a copy of the clamp record verifies" || bad "control copy: $out"
sed 's/^pre STATED+TESTED .*/pre STATED+TESTED true/' "$CL" > "$T/v.rec"; vmut "broken precondition (record only)" "pre differs from the spec file" "$T/v.rec" --receipts "$RS/receipts"
sed 's/^ret x$/ret y/' "$CL" > "$T/v.rec"; vmut "invalid IR (unknown name in the body)" "invalid IR" "$T/v.rec" --receipts "$RS/receipts"
sed 's/^params u32 u32 u32$/params u64 u64 u64/; s/^ret u32$/ret u64/; s/ uint/ ulong/g' "$CL" > "$T/v.rec"; vmut "wrong types (signature, params, ret and body all u64)" "fingerprint does not match the body" "$T/v.rec" --receipts "$RS/receipts"
sed 's/^params u32 u32 u32$/params u32 u32 u64/' "$CL" > "$T/v.rec"; vmut "wrong type in params only" "params differ" "$T/v.rec" --receipts "$RS/receipts"
sed '0,/^source /s/^source \(.*\) [0-9a-f]*$/source \1 '"$(printf stale | sha256sum | cut -c1-64)"'/' "$CL" > "$T/v.rec"; vmut "stale source digest" "stale source digest" "$T/v.rec" --receipts "$RS/receipts"
sed 's/^receipt .*/receipt '"$(printf none | sha256sum | cut -c1-64)"'/' "$CL" > "$T/v.rec"; vmut "fake receipt id (no such receipt)" "receipt file" "$T/v.rec" --receipts "$RS/receipts"
mkdir -p "$T/frc"; cp "$RS/receipts/"* "$T/frc/"
{ sed -n '1,2p' "$RS/receipts/$(sed -n 's/^receipt //p' "$CL")"; echo "property inputs=forged expected=forged ACCEPT n=256"; } > "$T/forged"
fid="$(sha256sum "$T/forged" | cut -c1-64)"; cp "$T/forged" "$T/frc/$fid"
sed "s/^receipt .*/receipt $fid/" "$CL" > "$T/v.rec"; vmut "forged receipt (hashes to its name, not from the checks)" "receipt does not match the re-run checks" "$T/v.rec" --receipts "$T/frc"
sed "s/^receipt .*/receipt $(sed -n 's/^receipt //p' "$RW")/" "$CL" > "$T/v.rec"; vmut "another record's receipt" "not this record's" "$T/v.rec" --receipts "$RS/receipts"
sed "s/^ancestry clamp_triple .*/ancestry digsum_mod9 $(sha256sum "$CO/tasks/digsum_mod9/solution.omegese" | cut -c1-64)/" "$CL" > "$T/v.rec"
vmut "single-source helper (forged second ancestor)" "does not contain this helper" "$T/v.rec" --receipts "$RS/receipts"
sed "s/^ancestry clamp_triple .*/ancestry gap_half $(sha256sum "$CO/tasks/gap_half/solution.omegese" | cut -c1-64)/; /^ancestry sum_clamped /d" "$CL" > "$T/v.rec"
vmut "single-source helper (one real ancestor)" "does not contain this helper" "$T/v.rec" --receipts "$RS/receipts"
sed '/^family /s/ gs1-[0-9a-f]*$//' "$CL" > "$T/v.rec"; vmut "family list missing an ancestor family" "family" "$T/v.rec" --receipts "$RS/receipts"
sed '/^body$/,$ s/^ret clamp add a b 10 100$/ret clamp add a b 10 101/' "$RW" > "$T/v.rec"
vmut "raw record body edited" "body is not the task's solution" "$T/v.rec" --receipts "$RS/receipts"
sed 's/^split corpus$/split probe/' "$RW" > "$T/v.rec"; vmut "raw record split relabelled" "split is not the task's" "$T/v.rec" --receipts "$RS/receipts"
# consistent forgeries in a copy of the corpus: the spec file and the record agree, so only the checks can tell
mkroot() { rm -rf "$T/root"; mkdir -p "$T/root/tests/one_layer/generalization"; cp -R "$CO" "$T/root/tests/one_layer/generalization/"; }
respec() { # <spec name> <sed expr for spec and record> <record> -- edit both and re-pin the spec digest in the record
  local sp="$T/root/tests/one_layer/generalization/corpus/specs/$1.spec"
  sed -i "$2" "$sp"; sed "$2" "$3" | sed "s|^\(source tests/one_layer/generalization/corpus/specs/$1.spec\) .*|\1 $(sha256sum "$sp" | cut -c1-64)|" > "$T/v.rec"
}
mkroot; respec clamp 's/^\(pre\( STATED+TESTED\)\{0,1\}\) le lo hi$/\1 true/' "$CL"
vmut "broken precondition (spec and record agree, pre true)" "property check failed" "$T/v.rec" --root "$T/root" --receipts "$RS/receipts"
mkroot; respec clamp 's/^counter 5 9 3 -> 9$/counter 5 3 9 -> 5/' "$CL"
vmut "counterexample that does not violate pre" "counter1-pre-false check failed" "$T/v.rec" --root "$T/root" --receipts "$RS/receipts"
mkroot; respec clamp 's/^counter 5 9 3 -> 9$/counter 5 9 3 -> 7/' "$CL"
vmut "counterexample with a wrong stated result" "counter1-result check failed" "$T/v.rec" --root "$T/root" --receipts "$RS/receipts"
mkroot; respec clamp 's/^example 12 3 9 -> 9$/example 12 3 9 -> 12/' "$CL"
vmut "wrong example" "example check failed" "$T/v.rec" --root "$T/root" --receipts "$RS/receipts"
mkroot; mkdir -p "$T/root/tests/one_layer/generalization/corpus/tasks/clamp_only"
sed '1,/^body$/d; s/^fn clamp /fn clamp_only /' "$CL" > "$T/root/tests/one_layer/generalization/corpus/tasks/clamp_only/solution.omegese"
vmut "helper identical to a whole corpus solution" "identical to the whole corpus solution of clamp_only" "$CL" --root "$T/root" --receipts "$RS/receipts"
mkroot; sed -i 's/^ret clamp add a b 10 100$/ret clamp add a b 10 99/' "$T/root/tests/one_layer/generalization/corpus/tasks/clamp_sum/solution.omegese"
vmut "stale corpus (an ancestor solution changed)" "stale source digest" "$CL" --root "$T/root" --receipts "$RS/receipts"

echo "test_gen_abstract: $oks ok, $fails failed"
[ "$fails" = 0 ] || exit 1
echo "test-gen-abstract: PASS"
