#!/usr/bin/env bash
# tools/gen/abstract/test_gen_abstract.sh <build dir> -- tests of GEN-3 (aien-spark/omega#478):
#   1 recheck: the schema v1 fixtures pass; every mutant is refused with its reason
#   (later sections are added by the extractor work: corpus, extract, verify, probes, mutants)
# <build dir> holds gen_abstract/recheck. Every check has a negative control or a mutant that must fail it.
set -euo pipefail
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

echo "test_gen_abstract: $oks ok, $fails failed"
[ "$fails" = 0 ] || exit 1
echo "test-gen-abstract: PASS"
