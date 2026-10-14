#!/usr/bin/env bash
# tools/exp445/test_exp445.sh <build/omega_emit dir> <oscc> -- tests of the exp445 driver (omega#445)
# without a model: x445 helpers, the three arm documents' worked examples, and a dry run of run.sh +
# score.sh against fake445 (a local stand-in endpoint) on the public dev tasks. Every check has a
# negative control or a mutant that must fail it.
set -euo pipefail
B="${1:?usage: test_exp445.sh <build/omega_emit dir> <oscc>}"; OSCC="${2:?oscc}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
X="$B/x445" EMIT="$B/omega_emit" FAKE="$B/fake445"
T="$(mktemp -d "${TMPDIR:-/tmp}/exp445-test.XXXXXXXX")"
FPID=""
trap '[ -n "$FPID" ] && kill "$FPID" 2>/dev/null; rm -rf "$T"' EXIT
fails=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
not() { ! "$@"; }
DEV="$ROOT/tests/one_layer/exp445/dev/tasks/cases"
REQ="$DEV/dev01/requirement"

# ---- 1. x445 helpers --------------------------------------------------------------------------------
printf 'Sure.\n```osc\nfn a() {}\n```\ntrailing\n```\nsecond\n```\n' > "$T/f1"; printf 'no fence here\n' > "$T/f2"; printf 'x\n```\ncut off' > "$T/f3"
[ "$("$X" extract "$T/f1" "$T/o1")" = fenced ] && [ "$(cat "$T/o1")" = "fn a() {}" ] &&
[ "$("$X" extract "$T/f2" "$T/o2")" = unfenced ] && cmp -s "$T/f2" "$T/o2" &&
[ "$("$X" extract "$T/f3" "$T/o3")" = open ] && [ "$(cat "$T/o3")" = "cut off" ] &&
  ok "extract: first fenced block, whole text without a fence, an unclosed block to the end" || bad "extract"
"$OSCC" "$DEV/dev01/hidden/reference.osc" "$T/d1" > /dev/null
"$X" tohex "$T/d1.ir" > "$T/d1.hex"
"$X" hex "$T/d1.hex" "$T/d1b.oscir" > /dev/null && cmp -s "$T/d1.ir" "$T/d1b.oscir" && ok "tohex -> hex round trip byte-identical" || bad "hex round trip"
hexref() { printf "$1" > "$T/h"; ! "$X" hex "$T/h" "$T/ho" > "$T/hm" && [ ! -e "$T/ho" ] && grep -q "^REFUSED hex: $2" "$T/hm"; }
hexref '4f 5' 'odd number' && hexref '4f 5 3' 'byte at offset' && hexref '0x4f' 'byte at offset' && hexref '4g' 'byte at offset' &&
  hexref '' 'no bytes' && hexref '4f,53' 'character 0x2c' && ok "hex: odd digits, split byte, 0x prefix, non-hex, empty, comma all refused, nothing written" || bad "hex refusals"
chk() { "$X" check "$1" "$2" "$3" | head -1; }
printf 'fn clamp_add(a: u8, b: u8) -> u8 { return a + ; }\n' > "$T/syn.osc"
printf 'fn clamp_add(a: u8) -> u8 { return a; }\n' > "$T/sig.osc"
printf 'fn other(a: u8, b: u8) -> u8 { return a; }\n' > "$T/name.osc"
head -c 30 "$T/d1.ir" > "$T/trunc.oscir"
[ "$(chk osc "$DEV/dev01/hidden/reference.osc" "$REQ")" = OK ] && [ "$(chk oscir "$T/d1.ir" "$REQ")" = OK ] &&
[ "$(chk osc "$T/syn.osc" "$REQ")" = "REFUSED compiler: SYNTAX at line 1: expected an expression, found ';'" ] &&
[ "$(chk osc "$T/sig.osc" "$REQ")" = "REFUSED signature: clamp_add takes (u8) -> u8; the task needs (u8, u8) -> u8" ] &&
[ "$(chk osc "$T/name.osc" "$REQ")" = "REFUSED signature: no function named clamp_add" ] &&
[ "$(chk oscir "$T/trunc.oscir" "$REQ")" = "REFUSED decoder: TRUNCATED (the input ends inside a field)" ] &&
  ok "check: reference OK in both readers; compiler, signature, name and decoder refusals with reasons" || bad "check"
[ "$("$X" wilson 7 10)" = "0.7000 0.3968 0.8922" ] && [ "$("$X" wilson 0 60)" = "0.0000 0.0000 0.0602" ] && [ "$("$X" wilson 60 60)" = "1.0000 0.9398 1.0000" ] &&
  not "$X" wilson 3 2 2> /dev/null && ok "wilson: 7/10 -> [0.3968, 0.8922], 0/60 and 60/60 bounds; k > n refused" || bad "wilson"

# ---- 2. the three arm documents show the same unit ----------------------------------------------------
awk '/^Example/ { e = 1 } e && /^```$/ { n++; next } e && n == 1 { print }' "$HERE/arms/text.md" > "$T/ex.osc"
awk '/^Example/ { e = 1 } e && /^```$/ { n++; next } e && n == 1 { print }' "$HERE/arms/bytes.md" > "$T/ex.hex"
awk '/^```$/ { n++; next } n == 3 { print }' "$ROOT/tools/omega_emit/SCHEMA.md" > "$T/ex.json"
"$OSCC" "$T/ex.osc" "$T/ext" > /dev/null; "$X" hex "$T/ex.hex" "$T/exb.oscir" > /dev/null; "$EMIT" emit "$T/ex.json" "$T/exs.oscir" > /dev/null
cmp -s "$T/ext.ir" "$T/exb.oscir" && cmp -s "$T/ext.ir" "$T/exs.oscir" && ok "arm examples: text, bytes and structured clamp_sub are the same IR bytes" || bad "arm examples differ"
sed 's/0f 0400/0f 0300/' "$T/ex.hex" > "$T/ex2.hex"; "$X" hex "$T/ex2.hex" "$T/ex2.oscir" > /dev/null
not cmp -s "$T/ext.ir" "$T/ex2.oscir" && ok "  (negative control: one changed byte in the bytes example is caught)" || bad "example control"

# ---- 3. dry run against the fake endpoint ------------------------------------------------------------
"$ROOT/tests/one_layer/exp445/pack_dev.sh" "$T/pack" > /dev/null
mkdir -p "$T/pk"; cp "$ROOT/tests/one_layer/exp445/pack_dev.sh" "$ROOT/tests/one_layer/exp445/gen_expected.c" "$T/pk/"; cp -r "$ROOT/tests/one_layer/exp445/dev" "$T/pk/"
sed -i '0,/^255$/s//254/' "$T/pk/dev/tasks/cases/dev01/hidden/expected"
if bash "$T/pk/pack_dev.sh" "$T/pkout" > /dev/null 2> "$T/pk.err"; then bad "pack_dev accepted a wrong expected line"
else grep -q "dev01 hidden/expected differs" "$T/pk.err" && [ ! -e "$T/pkout/tasks.tar" ] && ok "pack_dev refuses a wrong expected line and packs nothing (negative control)" || bad "pack refusal reason"; fi
"$ROOT/tests/one_layer/acceptance/ola.sh" tasks-public "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/pub" > /dev/null 2>&1
A="$T/answers"; mkdir -p "$A/text" "$A/bytes" "$A/structured"
fence() { printf 'Here is the program.\n```\n'; cat "$1"; printf '```\n'; }
fence "$DEV/dev01/hidden/reference.osc" > "$A/text/clamp_add"
printf '```\nfn absdiff(a: i32, b: i32) -> i64 { return a - ; }\n```\n' > "$A/text/absdiff"
fence "$DEV/dev02/hidden/reference.osc" > "$A/text/absdiff.repair"
printf '@@500\n' > "$A/text/popcount"
fence "$T/d1.hex" > "$A/bytes/clamp_add"
"$OSCC" "$DEV/dev02/hidden/reference.osc" "$T/d2" > /dev/null; "$X" tohex "$T/d2.ir" | head -c 40 > "$T/d2t.hex"; echo >> "$T/d2t.hex"
fence "$T/d2t.hex" > "$A/bytes/absdiff"; fence "$T/d2t.hex" > "$A/bytes/absdiff.repair"
"$EMIT" lift "$T/d1.ir" > "$T/d1.json"; fence "$T/d1.json" > "$A/structured/clamp_add"
printf '```\n{"functions": [{"name": "absdiff", "params": ["i32", "i32"], "ret": "i64", "locals": [], "blocks": [[{"op": "ret", "a": 0}]]}]}\n```\n' > "$A/structured/absdiff"
"$EMIT" lift "$T/d2.ir" > "$T/d2.json"; fence "$T/d2.json" > "$A/structured/absdiff.repair"
{ printf '```\n'; head -c 20000 /dev/zero | tr '\0' ' '; printf '{}\n```\n'; } > "$A/structured/popcount"
"$FAKE" "$T/port" "$A" & FPID=$!
for i in $(seq 1 50); do [ -s "$T/port" ] && break; sleep 0.1; done
URL="http://127.0.0.1:$(cat "$T/port")/v1"
if "$HERE/run.sh" --base-url "$URL" --model fake --config A --tasks "$ROOT/tests/one_layer/exp445/dev" --out "$T/runbad" > /dev/null 2> "$T/hid.err"; then bad "driver accepted a tasks dir with hidden files"
else grep -q "hidden files" "$T/hid.err" && ok "driver refuses a tasks dir that holds hidden files" || bad "hidden refusal reason"; fi
R="$T/gens/run-a"
"$HERE/run.sh" --base-url "$URL" --model fake --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 --attempts 1 > /dev/null 2> "$T/run.err" ||
  { bad "run.sh failed"; tail -5 "$T/run.err"; }
lg() { awk -F'\t' -v s="$1" -v t="$2" -v c="$3" '$1 == s && $2 == t { print $c }' "$R/log.tsv"; }
[ "$(($(wc -l < "$R/log.tsv") - 1))" -eq 9 ] && grep -qx 'protocol=OVERRIDDEN: only attempts' "$R/meta.txt" &&
  ok "9 generations (3 tasks x 3 arms x 1 attempt) logged; the overrides are recorded in meta.txt" || bad "log/meta"
[ "$(lg a-text-1 dev01 3)" = 1 ] && [ "$(lg a-text-1 dev01 10)" = OK ] && [ "$(lg a-text-1 dev02 3)" = 2 ] &&
[ "$(lg a-text-1 dev02 10)" = "REFUSED compiler: SYNTAX at line 1: expected an expression, found ';'" ] && [ "$(lg a-text-1 dev02 11)" = OK ] &&
[ "$(lg a-text-1 dev03 10)" = "REQUEST FAILED" ] && [ "$(lg a-text-1 dev03 12)" = - ] && [ "$(wc -l < "$R/gen/a-text-1/dev03/r1.errors")" -eq 3 ] &&
  ok "text arm: accepted, repaired after a compiler refusal, HTTP 500 retried twice then no candidate" || bad "text arm log"
[ "$(lg a-bytes-1 dev02 10)" = "REFUSED decoder: TRUNCATED (the input ends inside a field)" ] && [ "$(lg a-bytes-1 dev02 3)" = 2 ] &&
[ -f "$R/cands/a-bytes-1/dev02/candidate.oscir" ] && [ "$(lg a-bytes-1 dev03 10)" = "REFUSED hex: character 0x49 at offset 0 is not a hex digit" ] && [ ! -e "$R/cands/a-bytes-1/dev03" ] &&
  ok "bytes arm: decoder refusal reported, still-truncated bytes kept as the candidate; prose instead of hex -> no candidate" || bad "bytes arm log"
[ "$(lg a-structured-1 dev02 10)" = "REFUSED step: function absdiff block 0 op 0 (ret): r0 is i32; this function returns i64" ] && [ "$(lg a-structured-1 dev02 11)" = OK ] &&
[ "$(lg a-structured-1 dev03 7)" = length ] && [ "$(lg a-structured-1 dev03 3)" = 2 ] && [ ! -e "$R/cands/a-structured-1/dev03" ] &&
  ok "structured arm: omega_emit step refusal then repaired; an over-budget answer cut at max_tokens (finish=length), no candidate" || bad "structured arm log: $(lg a-structured-1 dev02 10)"
same_budget() { jq -s -e --argjson m "$1" 'all(.[]; .max_tokens == $m and .temperature == 0.7 and .top_p == 0.8)' "$R"/gen/*/*/r*.req.json > /dev/null; }
seeds_equal() { for t in dev01 dev02; do [ "$(jq -s '[.[].seed] | unique | length' "$R"/gen/a-*-1/$t/r1.req.json)" = 1 ] || return 1; done; }
same_budget 3072 && seeds_equal && not same_budget 2048 && ok "every request: max_tokens 3072 (PARAMS), temperature 0.7, top_p 0.8; one seed per task and attempt across the arms" || bad "budget/sampling"
fixmsg() { jq -r '.messages[3].content' "$R/gen/$1/$2/r2.req.json" | tail -1; }
[ "$(fixmsg a-text-1 dev02)" = "Reply with the corrected complete program in one fenced code block." ] &&
[ "$(fixmsg a-bytes-1 dev02)" = "$(fixmsg a-structured-1 dev02)" ] && [ "$(fixmsg a-text-1 dev02)" = "$(fixmsg a-bytes-1 dev02)" ] &&
jq -r '.messages[3].content' "$R/gen/a-bytes-1/dev02/r2.req.json" | head -1 | grep -qx 'Your program was refused: decoder: TRUNCATED (the input ends inside a field)' &&
  ok "repair round: same message format in every arm, with the reader's refusal reason" || bad "repair message"
(cd "$R" && sha256sum -c --quiet SHA256SUMS) && [ "$(wc -l < "$R/SHA256SUMS")" -ge 21 ] && ok "every raw request/response hashed in SHA256SUMS (verifies)" || bad "SHA256SUMS"
if "$HERE/run.sh" --base-url "$URL" --model fake --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 --attempts 1 --top-p 0.9 > /dev/null 2> "$T/res.err"; then bad "resume with other settings accepted"
else grep -q "resuming with different settings" "$T/res.err" && ok "resume with different settings refused" || bad "resume refusal reason"; fi
n0="$(wc -l < "$A/requests.log")"
"$HERE/run.sh" --base-url "$URL" --model fake --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 --attempts 1 > /dev/null 2>&1
[ "$(wc -l < "$A/requests.log")" = "$n0" ] && ok "resume with the same settings sends no request again" || bad "resume repeated requests"
# driver mutants
mkdir -p "$T/mut"
mut_run() { sed "$2" "$HERE/run.sh" > "$T/mut/run.sh"; chmod +x "$T/mut/run.sh"; cp "$HERE/PARAMS" "$T/mut/"; mkdir -p "$T/mut/arms"; cp "$HERE"/arms/* "$T/mut/arms/"
  cmp -s "$HERE/run.sh" "$T/mut/run.sh" && { bad "mutant $1: sed changed nothing"; return 1; }
  rm -rf "$T/mr"; sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|" "$T/mut/run.sh"
  "$T/mut/run.sh" --base-url "$URL" --model fake --config A --tasks "$T/pub" --out "$T/mr" --only dev01,dev02,dev03 --attempts 1 > /dev/null 2>&1 || true; }
R0="$R"; R="$T/mr"
M="no repair round"; mut_run norepair 's/if \[ "\$k1" != OK \]; then/if false; then/' && { [ "$(lg a-text-1 dev02 11)" = OK ] && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
M="budget not sent"; mut_run nobudget 's/max_tokens: \$mt, //' && { same_budget 3072 && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
R="$R0"
# ---- 4. score.sh: the sealed harness scores the merged sets; results with Wilson intervals ------------
if OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$HERE/score.sh" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$R" > "$T/score.out" 2> "$T/score.err"; then
  S="$(ls -d "$T"/gens/score-* | head -1)"
  row() { awk -F'\t' -v c="$1" -v a="$2" -v k="$3" 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next } $1 == c && $2 == a { print $h[k] }' "$S/results.tsv"; }
  # dev01..dev10 are all tasks (10); the driver ran 3: the 7 others are no_candidate
  [ "$(row a text success_attempt1)" = 2/10 ] && [ "$(row a bytes success_attempt1)" = 1/10 ] && [ "$(row a structured success_attempt1)" = 2/10 ] &&
  [ "$(row a text p1_lo)" = "$("$X" wilson 2 10 | cut -d' ' -f2)" ] && [ "$(row a structured repaired_accepted)" = 1 ] &&
  [ "$(row a text completion_tokens_total)" = "$(awk -F'\t' '$1 == "a-text-1" { s += $5 + ($6 == "-" ? 0 : $6) } END { printf "%.1f", s }' "$R/log.tsv")" ] &&
  grep -q '^RUN INVALID' "$S/harness_score.txt" && grep -q 'token cost relative to text, config a' "$S/results.txt" &&
    ok "score.sh: harness verdicts -> per arm success 2/10, 1/10, 2/10 with Wilson intervals, repairs, tokens; RUN INVALID flagged (selftest tree)" ||
    { bad "score.sh results"; cat "$S/results.tsv"; }
  awk -F'\t' '$1 == "a-bytes-1" && $2 == "dev02" { print $3, $4 }' "$S/verdicts.tsv" | grep -qx 'REJECT decode_refused' &&
  awk -F'\t' '$1 == "a-text-1" && $2 == "dev03" { print $3, $4 }' "$S/verdicts.tsv" | grep -qx 'REJECT no_candidate' &&
    ok "score.sh verdicts: truncated bytes decode_refused, failed request no_candidate" || bad "score verdicts"
else bad "score.sh failed"; tail -5 "$T/score.err"; fi
mkdir -p "$T/gens/dup/cands/a-text-1"; touch "$T/gens/dup/SHA256SUMS"; printf 'h\n' > "$T/gens/dup/log.tsv"
if OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$HERE/score.sh" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$R" "$T/gens/dup" > /dev/null 2> "$T/dup.err"; then bad "colliding set ids accepted"
else grep -q "appears in two runs" "$T/dup.err" && ok "score.sh refuses two runs with the same set id" || bad "dup refusal reason"; fi
echo
if [ "$fails" -eq 0 ]; then echo "test-exp445: PASS"; else echo "test-exp445: FAIL ($fails)"; exit 1; fi
