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
mkdir -p "$T/pk"; cp "$ROOT/tests/one_layer/exp445/pack_dev.sh" "$ROOT/tests/one_layer/exp445/gen_expected.c" "$T/pk/"; cp -r "$ROOT/tests/one_layer/exp445/dev" "$T/pk/"
sed -i '0,/^255$/s//254/' "$T/pk/dev/tasks/cases/dev01/hidden/expected"
if bash "$T/pk/pack_dev.sh" "$T/pkout" > /dev/null 2> "$T/pk.err"; then bad "pack_dev accepted a wrong expected line"
else grep -q "dev01 hidden/expected differs" "$T/pk.err" && [ ! -e "$T/pkout/tasks.tar" ] && ok "pack_dev refuses a wrong expected line and packs nothing (negative control)" || bad "pack refusal reason"; fi
"$ROOT/tests/one_layer/exp445/pack_dev.sh" "$T/pack" > /dev/null
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
# test PARAMS: the frozen PARAMS with both configurations pointed at the fake endpoint and 1 attempt
TP="$T/PARAMS"
sed -e "s|^config\.A\.base_url=.*|config.A.base_url=$URL|" -e "s|^config\.B\.base_url=.*|config.B.base_url=$URL|" -e 's/^attempts=.*/attempts=1/' "$HERE/PARAMS" > "$TP"
grep -qx 'config.B.extra={"chat_template_kwargs":{"enable_thinking":false}}' "$TP" && grep -qx 'config.A.extra={}' "$TP" || bad "PARAMS: config extras not as frozen"
export EXP445_PARAMS="$TP"
if "$HERE/run.sh" --config A --tasks "$ROOT/tests/one_layer/exp445/dev" --out "$T/runbad" > /dev/null 2> "$T/hid.err"; then bad "driver accepted a tasks dir with hidden files"
else grep -q "hidden files" "$T/hid.err" && ok "driver refuses a tasks dir that holds hidden files" || bad "hidden refusal reason"; fi
if "$HERE/run.sh" --config C --tasks "$T/pub" --out "$T/runc" > /dev/null 2> "$T/cfg.err"; then bad "driver ran a config not named in PARAMS"
else grep -q "config C is not named in" "$T/cfg.err" && [ ! -e "$T/runc/log.tsv" ] && ok "driver refuses a config not named in PARAMS (protocol section 9)" || bad "config refusal reason"; fi
R="$T/gens/run-a"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 > /dev/null 2> "$T/run.err" ||
  { bad "run.sh failed"; tail -5 "$T/run.err"; }
lg() { awk -F'\t' -v s="$1" -v t="$2" -v c="$3" '$1 == s && $2 == t { print $c }' "$R/log.tsv"; }
[ "$(($(wc -l < "$R/log.tsv") - 1))" -eq 9 ] && grep -qx 'protocol=OVERRIDDEN: only' "$R/meta.txt" && grep -qx "base_url=$URL" "$R/meta.txt" && grep -qx 'request_extra={}' "$R/meta.txt" &&
  ok "9 generations (3 tasks x 3 arms x 1 attempt) logged; config A's frozen endpoint and extra, the override recorded in meta.txt" || bad "log/meta"
[ "$(lg a-text-1 dev01 3)" = 1 ] && [ "$(lg a-text-1 dev01 10)" = OK ] && [ "$(lg a-text-1 dev02 3)" = 2 ] &&
[ "$(lg a-text-1 dev02 10)" = "REFUSED compiler: SYNTAX at line 1: expected an expression, found ';'" ] && [ "$(lg a-text-1 dev02 11)" = OK ] &&
[ "$(lg a-text-1 dev03 10)" = "REQUEST FAILED" ] && [ "$(lg a-text-1 dev03 12)" = - ] && [ "$(wc -l < "$R/gen/a-text-1/dev03/r1.errors")" -eq 3 ] &&
  ok "text arm: accepted, repaired after a compiler refusal, HTTP 500 retried twice then no candidate" || bad "text arm log"
[ "$(lg a-bytes-1 dev02 10)" = "REFUSED decoder: TRUNCATED (the input ends inside a field)" ] && [ "$(lg a-bytes-1 dev02 3)" = 2 ] &&
[ -f "$R/cands/a-bytes-1/dev02/candidate.oscir" ] && [ "$(lg a-bytes-1 dev03 10)" = "REFUSED hex: character 0x49 at offset 0 is not a hex digit" ] && [ ! -e "$R/cands/a-bytes-1/dev03" ] &&
  ok "bytes arm: decoder refusal reported, still-truncated bytes kept as the candidate; prose instead of hex -> no candidate" || bad "bytes arm log"
[ "$(lg a-structured-1 dev02 10)" = "REFUSED step: function absdiff block 0 op 0 (ret): r0 is i32; this function returns i64" ] && [ "$(lg a-structured-1 dev02 11)" = OK ] &&
[ "$(lg a-structured-1 dev03 7)" = length ] && [ "$(lg a-structured-1 dev03 3)" = 2 ] && [ ! -e "$R/cands/a-structured-1/dev03" ] &&
  ok "structured arm: omega_emit step refusal then repaired; an over-budget answer cut at max_tokens (finish=length), no candidate" || bad "structured arm log"
same_budget() { jq -s -e --argjson m "$1" 'all(.[]; .max_tokens == $m and .temperature == 0.7 and .top_p == 0.8)' "$R"/gen/*/*/r*.req.json > /dev/null; }
seeds_equal() { for t in dev01 dev02; do [ "$(jq -s '[.[].seed] | unique | length' "$R"/gen/a-*-1/$t/r1.req.json)" = 1 ] || return 1; done; }
same_budget 3072 && seeds_equal && not same_budget 2048 && ok "every request: max_tokens 3072 (PARAMS), temperature 0.7, top_p 0.8; one seed per task and attempt across the arms" || bad "budget/sampling"
fixmsg() { jq -r '.messages[3].content' "$R/gen/$1/$2/r2.req.json" | tail -1; }
[ "$(fixmsg a-text-1 dev02)" = "Reply with the corrected complete program in one fenced code block." ] &&
[ "$(fixmsg a-bytes-1 dev02)" = "$(fixmsg a-structured-1 dev02)" ] && [ "$(fixmsg a-text-1 dev02)" = "$(fixmsg a-bytes-1 dev02)" ] &&
jq -r '.messages[3].content' "$R/gen/a-bytes-1/dev02/r2.req.json" | head -1 | grep -qx 'Your program was refused: decoder: TRUNCATED (the input ends inside a field)' &&
  ok "repair round: same message format in every arm, with the reader's refusal reason" || bad "repair message"
(cd "$R" && sha256sum -c --quiet SHA256SUMS) && [ "$(wc -l < "$R/SHA256SUMS")" -ge 21 ] && ok "every raw request/response hashed in SHA256SUMS (verifies)" || bad "SHA256SUMS"
if "$HERE/run.sh" --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 --top-p 0.9 > /dev/null 2> "$T/res.err"; then bad "resume with other settings accepted"
else grep -q "resuming with different settings" "$T/res.err" && ok "resume with different settings refused" || bad "resume refusal reason"; fi
n0="$(wc -l < "$A/requests.log")"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 > /dev/null 2>&1
[ "$(wc -l < "$A/requests.log")" = "$n0" ] && ok "resume with the same settings sends no request again" || bad "resume repeated requests"
# the full dev matrix in both configurations (10 tasks x 3 arms x 1 attempt each)
FA="$T/gens/full-a" FB="$T/gens/full-b"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$FA" > /dev/null 2>&1 && "$HERE/run.sh" --config B --tasks "$T/pub" --out "$FB" > /dev/null 2>&1 || bad "full matrix runs failed"
has_kw() { jq -s -e 'all(.[]; .chat_template_kwargs == {"enable_thinking": false})' "$1"/gen/*/*/r*.req.json > /dev/null; }
no_kw() { jq -s -e 'all(.[]; has("chat_template_kwargs") | not)' "$1"/gen/*/*/r*.req.json > /dev/null; }
has_kw "$FB" && no_kw "$FA" && not has_kw "$FA" && grep -qx 'request_extra={"chat_template_kwargs":{"enable_thinking":false}}' "$FB/meta.txt" &&
grep -qx 'model=qwen3-8b' "$FB/meta.txt" && grep -qx 'protocol=PARAMS' "$FB/meta.txt" &&
  ok "config B: every request carries chat_template_kwargs.enable_thinking=false (in meta.txt and the hashed requests); config A requests do not" || bad "config extras in requests"
# driver mutants
mkdir -p "$T/mut"
mut_run() { sed "$2" "$HERE/run.sh" > "$T/mut/run.sh"; chmod +x "$T/mut/run.sh"; cp "$HERE/PARAMS" "$HERE/reader.sh" "$T/mut/"; mkdir -p "$T/mut/arms"; cp "$HERE"/arms/* "$T/mut/arms/"
  cmp -s "$HERE/run.sh" "$T/mut/run.sh" && { bad "mutant $1: sed changed nothing"; return 1; }
  rm -rf "$T/mr"; sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|" "$T/mut/run.sh"
  "$T/mut/run.sh" --config "${3:-A}" --tasks "$T/pub" --out "$T/mr" --only dev01,dev02,dev03 > /dev/null 2>&1 || true; }
R0="$R"; R="$T/mr"
M="no repair round"; mut_run norepair 's/if \[ "\$k1" != OK \]; then/if false; then/' && { [ "$(lg a-text-1 dev02 11)" = OK ] && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
M="budget not sent"; mut_run nobudget 's/max_tokens: \$mt, //' && { same_budget 3072 && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
M="config extra not sent"; mut_run noextra 's/stream: false} + \$x/stream: false}/' B && { has_kw "$T/mr" && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
R="$R0"

# ---- 4. score.sh: provenance first, then the sealed harness, then the results ------------------------
SC() { EXP445_TEST=1 OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$HERE/score.sh" "$@"; }
if SC "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$FA" "$FB" > "$T/score.out" 2> "$T/score.err"; then
  S="$(sed -n 's/^score.sh: results in //p' "$T/score.err")"
  row() { awk -F'\t' -v c="$1" -v a="$2" -v k="$3" 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next } $1 == c && $2 == a { print $h[k] }' "$S/results.tsv"; }
  [ "$(row a text success_attempt1)" = 2/10 ] && [ "$(row a bytes success_attempt1)" = 1/10 ] && [ "$(row a structured success_attempt1)" = 2/10 ] &&
  [ "$(row b text success_attempt1)" = 2/10 ] && [ "$(row a text p1_lo)" = "$("$X" wilson 2 10 | cut -d' ' -f2)" ] && [ "$(row a structured repaired_accepted)" = 1 ] &&
  [ "$(row a text completion_tokens_total)" = "$(awk -F'\t' '$1 == "a-text-1" { s += $5 + ($6 == "-" ? 0 : $6) } END { printf "%.1f", s }' "$FA/log.tsv")" ] &&
  head -1 "$S/results.txt" | grep -q '^TEST MODE, NOT EVIDENCE' && grep -q 'token cost relative to text, config a' "$S/results.txt" &&
  grep -qx 'gate config a: structured 2/10, text 2/10: structured FAIL (needs >= 80% and at most 10 points below text)' "$S/results.txt" &&
  grep -q '^bytes rule config b: bytes 1/10 vs structured 2/10, .*: bytes does not win$' "$S/results.txt" && grep -q '^GATE FAIL ' "$S/results.txt" &&
  grep -q '^provenance OK: 2 runs, 60 generations, configs A,B$' "$S/validity.txt" &&
    ok "score.sh: provenance OK, per arm success 2/10, 1/10, 2/10 with Wilson intervals, tokens, gate and bytes rule per config; marked TEST MODE" ||
    { bad "score.sh results"; cat "$S/results.txt"; }
  awk -F'\t' '$1 == "a-bytes-1" && $2 == "dev02" { print $3, $4 }' "$S/verdicts.tsv" | grep -qx 'REJECT decode_refused' &&
  awk -F'\t' '$1 == "a-text-1" && $2 == "dev03" { print $3, $4 }' "$S/verdicts.tsv" | grep -qx 'REJECT no_candidate' &&
    ok "score.sh verdicts: truncated bytes decode_refused, failed request no_candidate" || bad "score verdicts"
else bad "score.sh failed"; tail -5 "$T/score.err"; fi
if SC --configs A "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$FA" > "$T/s1.out" 2> /dev/null; then
  grep -q '^GATE NOT EVALUATED: configs scored a, the protocol needs a,b' "$T/s1.out" && ok "score.sh --configs A: config A alone is scored, the gate is not evaluated" || bad "subset gate line"
else bad "score.sh --configs A failed"; fi
# negative controls: each provenance failure gives RUN INVALID, exit 1 and no results table
invalid() { # invalid <name> <reason grep> <score.sh args...>
  local n="$1" why="$2"; shift 2
  if SC "$@" > "$T/inv.out" 2> "$T/inv.err"; then bad "$n: scored"; return; fi
  local s; s="$(sed -n 's/^score.sh: output in //p' "$T/inv.err")"
  grep -q '^RUN INVALID' "$T/inv.err" && grep -q -- "$why" "$s/validity.txt" && [ ! -e "$s/results.txt" ] && [ ! -s "$T/inv.out" ] &&
    ok "RUN INVALID, no table: $n" || { bad "$n: $(head -3 "$T/inv.err")"; }
}
cpr() { rm -rf "$T/neg"; mkdir -p "$T/neg"; cp -R "$FA" "$T/neg/a"; cp -R "$FB" "$T/neg/b"; }
cpr; printf ' ' >> "$T/neg/a/cands/a-text-1/dev01/candidate.osc"
invalid "tampered candidate" "a-text-1 dev01 candidate in cands/ does not match" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/neg/a" "$T/neg/b"
cpr; sed -i 's/"stop"/"stop" /' "$T/neg/b/gen/b-text-1/dev01/r1.resp.json"
invalid "tampered response" "SHA256SUMS does not verify" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/neg/a" "$T/neg/b"
cpr; awk -F'\t' '!($1 == "b-bytes-1" && $2 == "dev07")' "$FB/log.tsv" > "$T/neg/b/log.tsv"
invalid "missing generation" "1 of 60 generations missing (first: b-bytes-1 dev07)" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/neg/a" "$T/neg/b"
cpr; awk -F'\t' -v OFS='\t' '$1 == "a-structured-1" && $2 == "dev01" { $12 = "0000" } { print }' "$FA/log.tsv" > "$T/neg/a/log.tsv"
invalid "log hash not the re-derived candidate" "a-structured-1 dev01 log candidate_sha256" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/neg/a" "$T/neg/b"
sed 's/^temperature=.*/temperature=0.6/' "$TP" > "$T/PARAMS2"
EXP445_PARAMS="$T/PARAMS2" invalid "changed PARAMS" "PARAMS changed since generation" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$FA" "$FB"
invalid "override flag used" "settings overridden on the command line (OVERRIDDEN: only)" --configs A "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$R"
invalid "same run twice" "generations appear more than once" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$FA" "$FA" "$FB"
invalid "config outside --configs" "config 'B' is not one of the configs scored (A)" --configs A "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$FA" "$FB"
# score.sh mutant: the candidate re-derivation removed must let the tampered candidate through
sed 's/\[ "\$n" -le 1 \] && \[ "\$got" = "\$want" \] ||/true ||/' "$HERE/score.sh" > "$T/mut/score.sh"; chmod +x "$T/mut/score.sh"; sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|; s|^HERE=.*|HERE=\"$HERE\"|" "$T/mut/score.sh"
cpr; printf ' ' >> "$T/neg/a/cands/a-text-1/dev01/candidate.osc"
if cmp -s "$HERE/score.sh" "$T/mut/score.sh"; then bad "score mutant: sed changed nothing"
elif EXP445_TEST=1 OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$T/mut/score.sh" "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/neg/a" "$T/neg/b" > /dev/null 2>&1; then ok "  mutant caught: score.sh without the cands/ check scores a tampered candidate (the check is what stops it)"
else bad "  score mutant: still refused, the cands/ check is not the only guard tested"; fi
unset EXP445_PARAMS
echo
if [ "$fails" -eq 0 ]; then echo "test-exp445: PASS"; else echo "test-exp445: FAIL ($fails)"; exit 1; fi
