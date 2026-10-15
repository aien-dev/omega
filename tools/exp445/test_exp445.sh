#!/usr/bin/env bash
# tools/exp445/test_exp445.sh <build/omega_emit dir> <oscc> <omegese> -- tests of the exp445 r3 tooling
# (omega#445, docs/one_layer/EXP445_PROTOCOL.md section 13) without a model:
#   1 x445 helpers (extract, hex, check, wilson, mcnemar)
#   2 the worked examples (13.2): format, expected results computed independently, both forms through the
#     arm's reader and x445 check, both forms through the sealed harness (ola.sh experiment) on their own
#     inputs, the same task text in both arms' prompts, the word card, the one-token gate, held-out hygiene
#   3 G0 (g0.sh) and a dry run of run.sh against fake445 (a local stand-in endpoint) on the public dev tasks
#   4 score.sh: provenance, results, McNemar, cost, interim score and the Stage 1 stop rule
# Every check has a negative control or a mutant that must fail it.
# OMEGESE_TOKCOUNT + OMEGESE_TOKENIZER: run the one-token gate on the examples' Omegese words (default: the Spark paths when both exist; skipped otherwise).
# EXP445_HELDOUT_TAR + EXP445_HELDOUT_SUMS (optional; default: the sealed pack on the Spark): the held-out
# entry-name collision check; skipped when the pack is absent (CI). It prints only a count.
set -euo pipefail
B="${1:?usage: test_exp445.sh <build/omega_emit dir> <oscc> <omegese>}"; OSCC="${2:?oscc}"; G="${3:?omegese}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
X="$B/x445" EMIT="$B/omega_emit" FAKE="$B/fake445" EXX="$B/examples_expected"
T="$(mktemp -d "${TMPDIR:-/tmp}/exp445-test.XXXXXXXX")"
FPIDS=""
trap 'for p in $FPIDS; do kill "$p" 2>/dev/null; done; rm -rf "$T"' EXIT
fails=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
not() { ! "$@"; }
DEV="$ROOT/tests/one_layer/exp445/dev/tasks/cases"
OMD="$ROOT/tests/one_layer/omegese/dev"
REQ="$DEV/dev01/requirement"
EXD="$HERE/arms/examples"
. "$HERE/reader.sh"
fence() { printf 'Here is the program.\n```\n'; cat "$1"; printf '```\n'; }
# pack <src dir holding tasks/> <out dir>: a sealed task tarball the way pack_dev.sh makes one
pack() { mkdir -p "$2"; (cd "$1" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf "$2/tasks.tar" tasks); (cd "$2" && sha256sum tasks.tar > TASKS.sha256); }
OLA() { OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$ROOT/tests/one_layer/acceptance/ola.sh" "$@"; }

# ---- 1. x445 helpers --------------------------------------------------------------------------------
printf 'Sure.\n```osc\nfn a() {}\n```\ntrailing\n```\nsecond\n```\n' > "$T/f1"; printf 'no fence here\n' > "$T/f2"; printf 'x\n```\ncut off' > "$T/f3"
[ "$("$X" extract "$T/f1" "$T/o1")" = fenced ] && [ "$(cat "$T/o1")" = "fn a() {}" ] &&
[ "$("$X" extract "$T/f2" "$T/o2")" = unfenced ] && cmp -s "$T/f2" "$T/o2" &&
[ "$("$X" extract "$T/f3" "$T/o3")" = open ] && [ "$(cat "$T/o3")" = "cut off" ] &&
  ok "extract: first fenced block, whole text without a fence, an unclosed block to the end" || bad "extract"
"$OSCC" "$DEV/dev01/hidden/reference.osc" "$T/d1" > /dev/null
"$X" tohex "$T/d1.ir" > "$T/d1.hex"
"$X" hex "$T/d1.hex" "$T/d1b.oscir" > /dev/null && cmp -s "$T/d1.ir" "$T/d1b.oscir" && ok "tohex -> hex round trip byte-identical" || bad "hex round trip"
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
# exact two-sided McNemar: p = min(1, 2 P(X <= min(b, c))), X ~ Bin(b + c, 1/2); 1 vs 9: 2 * 11/1024
[ "$("$X" mcnemar 1 9)" = 0.021484 ] && [ "$("$X" mcnemar 9 1)" = 0.021484 ] && [ "$("$X" mcnemar 3 12)" = 0.035156 ] &&
[ "$("$X" mcnemar 0 0)" = 1.000000 ] && [ "$("$X" mcnemar 5 5)" = 1.000000 ] && [ "$("$X" mcnemar 0 1)" = 1.000000 ] &&
[ "$("$X" mcnemar 0 10)" = 0.001953 ] && not "$X" mcnemar -1 2 2> /dev/null && not "$X" mcnemar 1 2> /dev/null &&
  ok "mcnemar: 1/9 -> 0.021484 (symmetric), 3/12 -> 0.035156, 0/10 -> 0.001953, ties and 0/0 -> 1; bad arguments refused" || bad "mcnemar"

# ---- 2. worked examples (13.2) -----------------------------------------------------------------------
mapfile -t MANY < <(grep . "$EXD/many.list"); mapfile -t ONE < <(grep . "$EXD/one.list")
EXS=("${MANY[@]}" "${ONE[@]}")
[ "${#MANY[@]}" = 14 ] && [ "${#ONE[@]}" = 1 ] && [ "$(printf '%s\n' "${EXS[@]}" | LC_ALL=C sort -u | wc -l)" = 15 ] &&
[ "$(printf '%s\n' "${EXS[@]}" | LC_ALL=C sort)" = "$(cd "$EXD" && find . -mindepth 1 -maxdepth 1 -type d -printf '%P\n' | LC_ALL=C sort)" ] &&
  ok "examples: 14 in the many-shot list, 1 (the r2 example) in the one-shot list, every example dir listed exactly once" || bad "example lists"
# format, inputs, expected computed from the prompt by examples_expected.c (not from the programs)
devents="$(cat "$DEV"/*/requirement | awk '$1 == "entry" { print $2 }' | LC_ALL=C sort)"
exform() { # exform <example dir>: 0 when the example is well formed and its expected file is the independent one
  local d="$1" e; e="$(basename "$1")"
  [ "$(sed -n 1p "$d/requirement")" = "# exp445 worked example (protocol r3 section 13.2), not a held-out task" ] &&
  [ "$(sed -n 2p "$d/requirement")" = "kind task" ] && [ "$(sed -n 3p "$d/requirement")" = "entry $e" ] &&
  grep -qE '^params( (bool|[ui](8|16|32|64)))+$' "$d/requirement" && grep -qE '^ret (bool|[ui](8|16|32|64))$' "$d/requirement" &&
  [ "$(grep -c . "$d/inputs")" -ge 3 ] && [ "$(grep -c . "$d/inputs")" = "$(grep -c . "$d/expected")" ] &&
  [ -s "$d/prompt" ] && [ -s "$d/program.osc" ] && [ -s "$d/program.omegese" ] &&
  "$EXX" "$e" < "$d/inputs" | cmp -s - "$d/expected" && not grep -qx "$e" <<< "$devents"
}
nb=0 nin=0; for e in "${EXS[@]}"; do if exform "$EXD/$e"; then nin=$((nin + $(grep -c . "$EXD/$e/inputs"))); else nb=$((nb + 1)); echo "     example $e malformed"; fi; done
[ "$nb" = 0 ] && ok "every example: requirement format, entry = its name (not a dev entry), >= 3 inputs ($nin in all), expected = examples_expected" || bad "$nb examples malformed"
cp -R "$EXD/hash_step" "$T/exbad"; sed -i '1s/.*/TRAP 9/' "$T/exbad/expected"
mkdir -p "$T/exbad2"; cp -R "$EXD/hash_step" "$T/exbad2/dev_entry"; sed -i 's/^entry hash_step$/entry clamp_add/' "$T/exbad2/dev_entry/requirement"
not exform "$T/exbad" && not exform "$T/exbad2/dev_entry" && ok "  (negative controls: a changed expected line, an entry named like a dev task)" || bad "example format controls"
# both forms through the arm's own reader: omegese -> omega_emit -> x445 check; text -> x445 check (oscc)
mkdir -p "$T/rd" "$T/exc/omegese" "$T/exc/text"
nr=0; for e in "${EXS[@]}"; do for arm in omegese text; do
  ext=osc; [ "$arm" = omegese ] && ext=omegese
  fence "$EXD/$e/program.$ext" > "$T/rd/$e.$arm.content"
  reader "$X" "$EMIT" "$arm" "$T/rd/$e.$arm.content" "$T/rd/$e.$arm" "$EXD/$e/requirement"
  if [ "$(cat "$T/rd/$e.$arm.check")" = OK ]; then nr=$((nr + 1)); mkdir -p "$T/exc/$arm/$e"; for f in "$T/rd/$e.$arm".candidate.*; do cp "$f" "$T/exc/$arm/$e/candidate.${f##*.}"; done
  else echo "     $e $arm: $(cat "$T/rd/$e.$arm.check")"; fi
done; done
[ "$nr" = 30 ] && ok "reader + x445 check: all 15 examples OK in both forms (30 of 30)" || bad "reader: $nr of 30 example forms OK"
printf '```\nfn hash_step h uint to uint\nret h\nend\n```\n' > "$T/rd/sig.content"
reader "$X" "$EMIT" omegese "$T/rd/sig.content" "$T/rd/sig" "$EXD/hash_step/requirement"
grep -q '^REFUSED signature' "$T/rd/sig.check" && ok "  (negative control: an Omegese hash_step with one parameter is refused: $(cat "$T/rd/sig.check"))" || bad "reader control: $(cat "$T/rd/sig.check")"
# both forms through the sealed harness on the examples' own inputs, the way score.sh scores candidates;
# a mutant set (one Omegese and one OSC program with one changed constant) must be rejected
for e in "${EXS[@]}"; do
  c="$T/expk/src/tasks/cases/$e"; mkdir -p "$c/hidden"
  cp "$EXD/$e/requirement" "$EXD/$e/prompt" "$c/"; cp "$EXD/$e/inputs" "$EXD/$e/expected" "$c/hidden/"; cp "$EXD/$e/program.osc" "$c/hidden/reference.osc"
done
pack "$T/expk/src" "$T/expk"
mkdir -p "$T/exc/mutant/hash_step" "$T/exc/mutant/count_digits"
grep -q '31' "$EXD/hash_step/program.omegese" && sed 's/31/37/' "$EXD/hash_step/program.omegese" > "$T/rd/mut.omegese"
fence "$T/rd/mut.omegese" > "$T/rd/mut.content"; reader "$X" "$EMIT" omegese "$T/rd/mut.content" "$T/rd/mut" "$EXD/hash_step/requirement"
cp "$T/rd/mut.candidate.oscir" "$T/exc/mutant/hash_step/candidate.oscir"
grep -q '10' "$EXD/count_digits/program.osc" && sed '0,/10/s//16/' "$EXD/count_digits/program.osc" > "$T/exc/mutant/count_digits/candidate.osc"
not cmp -s "$T/exc/mutant/count_digits/candidate.osc" "$EXD/count_digits/program.osc" || bad "mutant: sed changed nothing in count_digits"
if OLA experiment "$T/expk/tasks.tar" "$T/expk/TASKS.sha256" "$T/exc" > /dev/null 2> "$T/exh.err"; then
  ev="$(sed -n 's/^ola.sh: evidence in //p' "$T/exh.err" | tail -1)"; case "$ev" in /*) ;; *) ev="$ROOT/$ev" ;; esac
  grep -q '^set=omegese accepted=15/15 ' "$ev/score.txt" && grep -q '^set=text accepted=15/15 ' "$ev/score.txt" && grep -q '^references_accepted=15/15' "$ev/score.txt" &&
    ok "harness: both forms of all 15 examples ACCEPT on their $nin inputs, so both forms give the expected (the same) outputs" || { bad "harness on the examples"; grep '^set=\|^references' "$ev/score.txt"; }
  [ "$(awk '$1 == "hash_step" || $1 == "count_digits" { print $2 }' "$ev/sets/mutant/verdicts.txt" | sort -u)" = REJECT ] &&
  ! awk '$1 == "hash_step" || $1 == "count_digits"' "$ev/sets/mutant/verdicts.txt" | grep -q no_candidate &&
    ok "  (negative control: one changed constant in an Omegese and in an OSC example is REJECTED by the harness)" || bad "harness mutant set: $(cat "$ev/sets/mutant/verdicts.txt" | head -3)"
else bad "ola.sh experiment on the examples failed: $(tail -3 "$T/exh.err")"; fi
# both arms' prompts carry the same tasks; only the program blocks differ
tasktext() { awk '/^Worked example/ { w = 1 } w' | awk '/^```$/ { f = !f; print; next } !f'; }
for mode in many one; do for arm in omegese text; do system_prompt "$HERE/arms" "$arm" "$mode" > "$T/p.$arm.$mode"; done; done
cmp -s <(tasktext < "$T/p.omegese.many") <(tasktext < "$T/p.text.many") && cmp -s <(tasktext < "$T/p.omegese.one") <(tasktext < "$T/p.text.one") &&
[ "$(grep -cE '^Example [0-9]+\.$' "$T/p.omegese.many")" = 14 ] && [ "$(grep -cE '^Example [0-9]+\.$' "$T/p.text.one")" = 1 ] &&
grep -qx 'entry clamp_sub' "$T/p.text.one" && not grep -qx 'entry clamp_sub' "$T/p.text.many" &&
[ "$(grep -c '^```$' "$T/p.omegese.many")" = 28 ] && grep -qF "$(cat "$EXD/dot3/program.omegese")" "$T/p.omegese.many" && grep -qF "$(cat "$EXD/dot3/program.osc")" "$T/p.text.many" &&
  ok "prompts: both arms carry the same task text (14 many-shot, 1 one-shot = clamp_sub), each with its own program form" || bad "prompt task text differs between arms"
not cmp -s <(tasktext < "$T/p.omegese.many") <(tasktext < "$T/p.text.many" | sed '0,/signed/s//unsigned/') &&
  not system_prompt "$HERE/arms" bytes many 2> /dev/null > /dev/null && not system_prompt "$HERE/arms" omegese few 2> /dev/null > /dev/null &&
  ok "  (negative controls: one changed word is seen; the bytes arm and an unknown mode have no prompt)" || bad "prompt controls"
# word card: one line per fixed word of the language, no more, no less
card() { awk '/^Word card/ { w = 1; next } w && /^    [a-z]/ { print $1 }' "$1" | LC_ALL=C sort; }
[ "$(card "$HERE/arms/omegese.md")" = "$("$G" words | LC_ALL=C sort)" ] && [ "$(card "$HERE/arms/omegese.md" | uniq -d | wc -l)" = 0 ] &&
  ok "word card: exactly the $("$G" words | wc -l) fixed words of omegese, each once" || bad "word card differs from omegese words"
grep -v '^    bound ' "$HERE/arms/omegese.md" > "$T/card.md"
[ "$(card "$T/card.md")" != "$("$G" words | LC_ALL=C sort)" ] && ok "  (negative control: a card missing 'bound' is caught)" || bad "word card control"
# one-token gate on every word of the Omegese examples (numbers, = and the task's own entry name excluded)
# the Spark default paths when the variables are unset and both exist (the gate then runs in a plain make call)
if [ -z "${OMEGESE_TOKCOUNT:-}" ] && [ -z "${OMEGESE_TOKENIZER:-}" ] && [ -x "$HOME/workspace/glyph-tok/target/release/tokcount" ] &&
   [ -f "$HOME/models/qwen3-30b-a3b-instruct-2507-fp8/tokenizer.json" ]; then
  OMEGESE_TOKCOUNT="$HOME/workspace/glyph-tok/target/release/tokcount" OMEGESE_TOKENIZER="$HOME/models/qwen3-30b-a3b-instruct-2507-fp8/tokenizer.json"; fi
if [ -n "${OMEGESE_TOKCOUNT:-}" ] && [ -n "${OMEGESE_TOKENIZER:-}" ]; then
  for e in "${EXS[@]}"; do tr -s ' \t' '\n\n' < "$EXD/$e/program.omegese" | grep -v -E '^-?[0-9]+$|^=$|^$' | grep -vx "$e" || true; done | LC_ALL=C sort -u > "$T/words"
  multi="$("$OMEGESE_TOKCOUNT" "$OMEGESE_TOKENIZER" words $(cat "$T/words") | awk '$1 != 1 { print $3 }')"
  [ -z "$multi" ] && ok "one-token gate: all $(wc -l < "$T/words") distinct words of the Omegese examples are one token each" || bad "multi-token example words: $multi"
  [ "$("$OMEGESE_TOKCOUNT" "$OMEGESE_TOKENIZER" words a1 mulmod | awk '$1 != 1' | wc -l)" = 2 ] &&
    ok "  (negative control: a1 and mulmod are two tokens each and would fail the gate)" || bad "token gate control"
else echo "one-token gate on the examples: SKIPPED (set OMEGESE_TOKCOUNT and OMEGESE_TOKENIZER)"; fi
# held-out hygiene: no example entry name equals a held-out task entry name. Reads only the entry lines of
# the public view's requirement files and prints only the count.
printf '%s\n' "${EXS[@]}" | LC_ALL=C sort -u > "$T/exnames"
collisions() { cat "$1"/tasks/cases/*/requirement | awk '$1 == "entry" { print $2 }' | LC_ALL=C sort -u | LC_ALL=C comm -12 - "$2" | wc -l; }
"$ROOT/tests/one_layer/exp445/pack_dev.sh" "$T/pack" > /dev/null
"$ROOT/tests/one_layer/acceptance/ola.sh" tasks-public "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/pub" > /dev/null 2>&1
{ cat "$T/exnames"; echo clamp_add; } | LC_ALL=C sort > "$T/exnames.bad"
[ "$(collisions "$T/pub" "$T/exnames")" = 0 ] && [ "$(collisions "$T/pub" "$T/exnames.bad")" = 1 ] &&
  ok "no example entry name is a dev entry name (negative control: an extra clamp_add counts 1)" || bad "dev collision check"
HT="${EXP445_HELDOUT_TAR:-$HOME/workspace/one-layer-sealed-445/tasks.tar}" HS="${EXP445_HELDOUT_SUMS:-$HOME/workspace/omega-vision/stage2/TASKS.sha256}"
if [ -f "$HT" ] && [ -f "$HS" ]; then
  if "$ROOT/tests/one_layer/acceptance/ola.sh" tasks-public "$HT" "$HS" "$T/hpub-sealed" > /dev/null 2>&1; then
    n="$(collisions "$T/hpub-sealed" "$T/exnames")"; rm -rf "$T/hpub-sealed"
    [ "$n" = 0 ] && ok "held-out hygiene: example entry names colliding with held-out entry names: 0" || bad "held-out hygiene: example entry names colliding with held-out entry names: $n"
  else bad "held-out hygiene: tasks-public refused the sealed pack"; rm -rf "$T/hpub-sealed"; fi
else echo "held-out hygiene: SKIPPED (no sealed pack here)"; fi
# the r2 one-shot example is still the same unit in every r2 form (bytes.md, the SCHEMA example)
awk '/^Example/ { e = 1 } e && /^```$/ { n++; next } e && n == 1 { print }' "$HERE/arms/bytes.md" > "$T/ex.hex"
awk '/^```$/ { n++; next } n == 3 { print }' "$ROOT/tools/omega_emit/SCHEMA.md" > "$T/ex.json"
"$OSCC" "$EXD/clamp_sub/program.osc" "$T/ext" > /dev/null; "$X" hex "$T/ex.hex" "$T/exb.oscir" > /dev/null; "$EMIT" emit "$T/ex.json" "$T/exs.oscir" > /dev/null
cmp -s "$T/ext.ir" "$T/exb.oscir" && cmp -s "$T/ext.ir" "$T/exs.oscir" &&
  ok "clamp_sub (the r2 one-shot example): the OSC, r2 bytes and SCHEMA forms are the same IR bytes" || bad "clamp_sub forms differ"

# ---- 3. G0 and the dry run against the fake endpoint -------------------------------------------------
mkdir -p "$T/pk"; cp "$ROOT/tests/one_layer/exp445/pack_dev.sh" "$ROOT/tests/one_layer/exp445/gen_expected.c" "$T/pk/"; cp -r "$ROOT/tests/one_layer/exp445/dev" "$T/pk/"
sed -i '0,/^255$/s//254/' "$T/pk/dev/tasks/cases/dev01/hidden/expected"
if bash "$T/pk/pack_dev.sh" "$T/pkout" > /dev/null 2> "$T/pk.err"; then bad "pack_dev accepted a wrong expected line"
else grep -q "dev01 hidden/expected differs" "$T/pk.err" && [ ! -e "$T/pkout/tasks.tar" ] && ok "pack_dev refuses a wrong expected line and packs nothing (negative control)" || bad "pack refusal reason"; fi
# a held-out-like pack: dev01..dev04 renamed h01..h04 (ids that are not dev*)
for i in 1 2 3 4; do mkdir -p "$T/hp/src/tasks/cases"; cp -R "$DEV/dev0$i" "$T/hp/src/tasks/cases/h0$i"; done
pack "$T/hp/src" "$T/hp"; "$ROOT/tests/one_layer/acceptance/ola.sh" tasks-public "$T/hp/tasks.tar" "$T/hp/TASKS.sha256" "$T/hpub" > /dev/null 2>&1
# fake answers. text: dev01 OK, dev02 compiler refusal then repaired, dev03 HTTP 500 (all tries).
# omegese: dev01 OK (a different wording per G0 seed), dev02 refused then repaired, dev03 over budget, dev04 OK.
A="$T/answers"; mkdir -p "$A/text" "$A/omegese"
fence "$DEV/dev01/hidden/reference.osc" > "$A/text/clamp_add"
printf '```\nfn absdiff(a: i32, b: i32) -> i64 { return a - ; }\n```\n' > "$A/text/absdiff"
fence "$DEV/dev02/hidden/reference.osc" > "$A/text/absdiff.repair"
printf '@@500\n' > "$A/text/popcount"
fence "$OMD/dev01.omegese" > "$A/omegese/clamp_add"
printf '```\nfn absdiff a int b int to long\nret sub a b\nend\n```\n' > "$A/omegese/absdiff"
fence "$OMD/dev02.omegese" > "$A/omegese/absdiff.repair"
{ printf '```\n'; head -c 20000 /dev/zero | tr '\0' ' '; printf 'fn popcount\n```\n'; } > "$A/omegese/popcount"
fence "$OMD/dev04.omegese" > "$A/omegese/gcd"
g0seeds() { for k in 1 2 3 4 5 6 7 8; do seed_of 445000 "$k" 1; done; }
for s in $(g0seeds); do { echo "Answer for seed $s."; fence "$OMD/dev01.omegese"; } > "$A/omegese/clamp_add.seed$s"; done
# text, attempt 2 of the first task (seed 447001): the same program with another variable name (a distinct candidate)
sed 's/\bs\b/t/g' "$DEV/dev01/hidden/reference.osc" > "$T/clamp_add.alt.osc"; fence "$T/clamp_add.alt.osc" > "$A/text/clamp_add.seed447001"
printf '{"object":"list","data":[{"id":"qwen3-30b-a3b-instruct-2507","object":"model"},{"id":"qwen3-8b","object":"model"}]}\n' > "$A/models.json"
A2="$T/answers2"; mkdir -p "$A2/omegese"; cp "$A/models.json" "$A2/"; fence "$OMD/dev01.omegese" > "$A2/omegese/clamp_add"
startfake() { "$FAKE" "$1" "$2" & FPIDS="$FPIDS $!"; for i in $(seq 1 50); do [ -s "$1" ] && break; sleep 0.1; done; printf -v "$3" "http://127.0.0.1:%s/v1" "$(cat "$1")"; }
startfake "$T/port" "$A" URL; startfake "$T/port2" "$A2" URL2
# test PARAMS: the frozen PARAMS with config A pointed at the fake endpoint and given a stand-in server,
# config B made available on the fake endpoint, 1 attempt, and a small stand-in model directory
mkdir -p "$T/models/m"; printf 'w1\n' > "$T/models/m/a.bin"; printf 'w2\n' > "$T/models/m/b.bin"
(cd "$T/models" && sha256sum m/a.bin m/b.bin) > "$T/wl.sha256"
TP="$T/PARAMS"
sed -e "s|^config\.A\.base_url=.*|config.A.base_url=$URL|" -e 's/^attempts=.*/attempts=1/' -e 's|^config\.A\.server=.*|config.A.server=fake445 test server A|' \
    -e '/^config\.B\.unavailable=/d' -e "s|^models_root=.*|models_root=$T/models|" -e "s|^config\.\([AB]\)\.weights=.*|config.\1.weights=$T/wl.sha256|" "$HERE/PARAMS" > "$TP"
printf 'config.B.base_url=%s\nconfig.B.extra={"chat_template_kwargs":{"enable_thinking":false}}\nconfig.B.server=fake445 test server B\n' "$URL" >> "$TP"
sed "s|^config\.A\.base_url=.*|config.A.base_url=$URL2|" "$TP" > "$T/PARAMS2"
sed -e 's/^attempts=.*/attempts=2/' -e 's/^heldout_tasks=.*/heldout_tasks=4/' -e 's/^futility_max=.*/futility_max=3/' "$TP" > "$T/PARAMSH"
sed 's/^futility_max=.*/futility_max=2/' "$T/PARAMSH" > "$T/PARAMSC"
export EXP445_PARAMS="$TP" EXP445_TEST_BACKOFF_S=0
export EXP445_G0_LEDGER="$T/g0-ledger"   # the tests never write the real ledger
lcat() { find "$1" -maxdepth 1 -type f -name "*.tsv" -print0 2> /dev/null | LC_ALL=C sort -z | xargs -0 -r cat; }   # a ledger directory, oldest entry first
cw() { "$HERE/check_weights.sh" "$1" "$2" > /dev/null; }
g0() { "$HERE/g0.sh" --config "$1" --out "$2" > /dev/null 2>&1; }
prep() { cw "$1" "$2" && g0 "$1" "$2"; }
rehash() { local d; for d in "$1"/gen/*/*/; do gen_sums "${d%/}"; done; run_sums "$1"; }
g0hash() { (cd "$1/g0" && g0_files . | tr '\n' '\0' | xargs -0 sha256sum) > "$1/g0/SHA256SUMS"; }
refuse() { # refuse <name> <reason grep> <command...>: the command exits non-zero with that reason
  local n="$1" why="$2"; shift 2
  if "$@" > "$T/rf.out" 2> "$T/rf.err"; then bad "$n: not refused"; return; fi
  grep -q -- "$why" "$T/rf.err" && ok "refused: $n" || bad "$n: $(tail -2 "$T/rf.err")"
}
# the real PARAMS: config B unavailable with its reason, config A's server frozen (13.3)
refuse "real PARAMS: run.sh config B (OWN-8B not requalified)" "config B is unavailable: config B runs on our own dense 8B engine (OWN-8B), which has not requalified" \
  env -u EXP445_PARAMS "$HERE/run.sh" --config B --tasks "$T/pub" --out "$T/realb"
refuse "real PARAMS: g0.sh config B" "config B is unavailable: config B runs on our own dense 8B engine" env -u EXP445_PARAMS "$HERE/g0.sh" --config B --out "$T/realb"
refuse "real PARAMS: score.sh --configs B" "config B is unavailable" env -u EXP445_PARAMS "$HERE/score.sh" --configs B "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/pub"
sed 's|^config\.A\.server=.*|config.A.server=PLACEHOLDER: not frozen|' "$HERE/PARAMS" > "$T/PARAMSPH"
refuse "PARAMS with config A's server a placeholder: a held-out run" "config A server is still a placeholder in PARAMS: held-out generation waits" \
  env EXP445_PARAMS="$T/PARAMSPH" "$HERE/run.sh" --config A --tasks "$T/hpub" --out "$T/realh"
[ ! -e "$T/realb" ] && [ ! -e "$T/realh" ] && ok "  nothing written by the refused runs" || bad "refused runs wrote files"
grep -qx 'arms=omegese,text' "$HERE/PARAMS" && grep -qx 'prompt_modes=many,one' "$HERE/PARAMS" && grep -qx 'heldout_prompt_mode=many' "$HERE/PARAMS" &&
grep -qx 'futility_max=30' "$HERE/PARAMS" && grep -qx 'heldout_tasks=60' "$HERE/PARAMS" &&
  ok "PARAMS r3: arms omegese,text; prompt modes many,one (held-out: many); futility 30 of 60" || bad "PARAMS r3 values"
# config A's server: a binary path and its sha256, the same pair as the protocol table 13.3; the binary, when present here, has it
SV="$(awk -F= '$1 == "config.A.server" { print substr($0, 17); exit }' "$HERE/PARAMS")"
SVP="$(sed -n 's|^\(/[^ ]*/aien-serve-http\) sha256=[0-9a-f]\{64\} .*|\1|p' <<< "$SV")" SVH="$(sed -n 's|^/[^ ]* sha256=\([0-9a-f]\{64\}\) .*|\1|p' <<< "$SV")"
if [ -n "$SVP" ] && [ -n "$SVH" ] && grep -F '| server |' "$ROOT/docs/one_layer/EXP445_PROTOCOL.md" | grep -qF "\`$SVP\`, sha256 \`$SVH\`"; then
  ok "config A server frozen: $SVP, sha256 ${SVH:0:16}..., the same as protocol 13.3"
  if [ -f "$SVP" ]; then [ "$(sha256sum < "$SVP" | cut -d' ' -f1)" = "$SVH" ] && ok "  and the binary here has that sha256" || bad "config A binary $SVP does not have the frozen sha256"
  else echo "skip: $SVP not present here; the frozen pair is checked against the protocol only"; fi
else bad "config A server is not a frozen path + sha256 matching protocol 13.3: $SV"; fi
# frozen weight lists and the operator check
pblock() { awk '/^Weight files, sha256/ { w = 1 } w && /^```$/ { n++; next } w && n == 1 { print }' "$1"; }
[ "$(pblock "$ROOT/docs/one_layer/EXP445_PROTOCOL.md")" = "$(cat "$HERE/weights.A.sha256" "$HERE/weights.B.sha256")" ] &&
  [ "$(pblock "$ROOT/docs/one_layer/EXP445_PROTOCOL.md" | sed '3s/^./0/')" != "$(cat "$HERE/weights.A.sha256" "$HERE/weights.B.sha256")" ] &&
  ok "weights.A/B.sha256 equal the protocol's frozen hash block (one changed digit is caught)" || bad "weight lists vs protocol"
cw B "$T/wck" && [ "$(cat "$T/wck/weights.check")" = "$(printf 'm/a.bin: OK\nm/b.bin: OK')" ] && jq -e '.data[1].id == "qwen3-8b"' "$T/wck/server.json" > /dev/null &&
  ok "check_weights.sh: every listed file OK and the server's model list recorded" || bad "check_weights.sh"
printf 'w3\n' > "$T/models/m/b.bin"
if cw B "$T/wck2" 2> /dev/null; then bad "check_weights.sh accepted a changed weight file"
else grep -qx 'm/b.bin: FAILED' "$T/wck2/weights.check" && ok "  (negative control: a changed weight file FAILS the check)" || bad "check_weights failure record"; fi
refuse "driver with a FAILED weight check" 'weights.check + server.json missing or not all OK' "$HERE/run.sh" --config B --tasks "$T/pub" --out "$T/wck2"
printf 'w2\n' > "$T/models/m/b.bin"
refuse "driver with no weight check" 'run tools/exp445/check_weights.sh A' "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/nowck"
refuse "driver on a tasks dir with hidden files" "hidden files" "$HERE/run.sh" --config A --tasks "$ROOT/tests/one_layer/exp445/dev" --out "$T/runbad"
# a 60-task dir with many hidden files (more find output than a pipe holds: a guard that pipes find into grep -q
# fails open under pipefail when find is killed by SIGPIPE)
for i in $(seq -w 1 60); do mkdir -p "$T/hid/tasks/cases/t$i/hidden"; for j in $(seq -w 1 40); do : > "$T/hid/tasks/cases/t$i/hidden/a_long_hidden_file_name_for_the_pipe_test_$j"; done; done
refuse "driver on a 60-task dir with 2400 hidden files" "hidden files" "$HERE/run.sh" --config A --tasks "$T/hid" --out "$T/runhid"
refuse "driver on a config not named in PARAMS" "config C is not named in" "$HERE/run.sh" --config C --tasks "$T/pub" --out "$T/runc"
refuse "the bytes arm (dropped by r3)" "arm bytes is dropped by protocol r3" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/runc" --arms omegese,bytes
refuse "an unknown prompt mode" "prompt mode 'few' is not one of PARAMS prompt_modes" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/runc" --prompt-mode few
refuse "a held-out run in the one-shot mode" "a held-out run uses the many prompt mode" env EXP445_PARAMS="$T/PARAMSH" "$HERE/run.sh" --config A --tasks "$T/hpub" --out "$T/runc" --prompt-mode one
# G0 (13.4): the stage S sampling gate (vendored, frozen by sha256) plus dev01 at 8 seeds and a repeat of the first.
GS="$(sha256sum < "$HERE/sampling-gate.sh" | cut -d' ' -f1)"
grep -qx "g0_gate_sha256=$GS" "$HERE/PARAMS" && ok "the vendored sampling gate's sha256 equals PARAMS g0_gate_sha256" || bad "vendored sampling gate sha256 vs PARAMS"
SC="$HOME/workspace/aien-sovereign-core"
if git -C "$SC" cat-file -e f881710a:scripts/sampling-gate.sh 2> /dev/null; then
  [ "$(git -C "$SC" show f881710a:scripts/sampling-gate.sh | sha256sum | cut -d' ' -f1)" = "$GS" ] &&
    ok "  and is byte for byte aien-sovereign-core f881710a scripts/sampling-gate.sh" || bad "vendored sampling gate differs from sovereign-core f881710a"
else echo "skip: aien-sovereign-core f881710a not present here; the vendored gate is checked against PARAMS only"; fi
# PASS on the fake endpoint: 8 distinct answers, the repeat identical, every reply sampled with its own seed
G0P="$T/g0pass"; cw A "$G0P"
if "$HERE/g0.sh" --config A --out "$G0P" > "$T/g0p.out" 2>&1; then
  v="$G0P/g0/verdict.txt"
  [ "$(tail -1 "$v")" = "G0 PASS" ] && grep -qx 'derived=gate=PASS distinct=8/8 repeat_identical=yes sampled_own_seed=9/9' "$v" &&
  [ "$(tail -1 "$G0P/g0/gate/gate.txt")" = "VERDICT PASS" ] && grep -q '^gate d PASS distinct=8/8' "$G0P/g0/gate/gate.txt" &&
  [ "$(jq -s '[.[].seed] | join(" ")' "$G0P"/g0/r[1-9].req.json)" = "\"$(g0seeds | tr '\n' ' ')446001\"" ] &&
  jq -s -e 'all(.[]; .temperature == 0.7 and .top_p == 0.8 and .max_tokens == 3072)' "$G0P"/g0/r*.req.json > /dev/null &&
  jq -e '.messages[1].content | test("\nentry clamp_add\n")' "$G0P/g0/r1.req.json" > /dev/null && cmp -s "$G0P/g0/prompt.system.txt" "$T/p.omegese.many" &&
  cmp -s "$G0P/g0/sampling-gate.sh" "$HERE/sampling-gate.sh" && grep -qx "gate_sha256=$GS" "$v" &&
  (cd "$G0P/g0" && sha256sum -c --quiet --strict SHA256SUMS) && grep -q ' gate/d-r8.json$' "$G0P/g0/SHA256SUMS" &&
    ok "G0 PASS: the sampling gate passes (its gate d on dev01's 8 attempt seeds), the repeat of seed 1 identical, 9/9 sampled with their own seeds; T 0.7, top_p 0.8; all hashed" ||
    { bad "G0 PASS record"; cat "$v"; }
else bad "g0.sh did not PASS: $(tail -2 "$T/g0p.out")"; fi
refuse "g0.sh over an existing G0 record" "exists: a G0 record is never overwritten" "$HERE/g0.sh" --config A --out "$G0P"
# the same answer for every seed: PASS only when the reported p_repeat makes it plausible (p_repeat^7 >= 0.01)
g0case() { # g0case <name> <sampling.json or -> <expect PASS|FAIL> <derived> [gate.txt line grep]
  local d="$T/g0c$((++G0N))"; if [ "$2" = - ]; then rm -f "$A2/sampling.json"; else printf '%s\n' "$2" > "$A2/sampling.json"; fi
  EXP445_PARAMS="$T/PARAMS2" cw A "$d"; local e=0; EXP445_G0_LEDGER="$d.ledger" EXP445_PARAMS="$T/PARAMS2" "$HERE/g0.sh" --config A --out "$d" > /dev/null 2>&1 || e=$?
  [ "$e" = "$([ "$3" = PASS ] && echo 0 || echo 1)" ] && [ "$(tail -1 "$d/g0/verdict.txt")" = "G0 $3" ] && grep -qx "derived=$4" "$d/g0/verdict.txt" &&
    { [ -z "${5:-}" ] || grep -q -- "$5" "$d/g0/gate/gate.txt"; } && ok "G0 $3 (exit $e): $1" || { bad "G0 case: $1"; grep derived "$d/g0/verdict.txt"; tail -3 "$d/g0/gate/gate.txt"; }
  G0LAST="$d"
}
G0N=0
g0case "identical answers, p_repeat 0.5 (0.5^7 < 0.01): the seed looks ignored" - FAIL 'gate=FAIL distinct=1/8 repeat_identical=yes sampled_own_seed=9/9' 'gate d FAIL identical replies with p_repeat^7=0.0078125 < 0.01'
g0case "identical answers, p_repeat 0.51 (0.51^7 = 0.00897 < 0.01), boundary" '{"p_repeat": 0.51}' FAIL 'gate=FAIL distinct=1/8 repeat_identical=yes sampled_own_seed=9/9' 'gate d FAIL identical'
g0case "identical answers, p_repeat 0.52 (0.52^7 = 0.0103 >= 0.01): a confident model; distinct 1/8 recorded, not gated" '{"p_repeat": 0.52}' PASS 'gate=PASS distinct=1/8 repeat_identical=yes sampled_own_seed=9/9' 'gate d PASS identical replies explained by confidence'
# the G0 ledger: a FAIL bars the configuration's identity (server and settings) in every run dir (13.4)
ID2="$(g0_ident "$T/PARAMS2" A)"
[ "$(lcat "$T/g0c1.ledger" | cut -f2-5)" = "$(printf 'A\tfake445 test server A\t%s\tFAIL' "$ID2")" ] && [ "$(lcat "$T/g0c1.ledger" | cut -f6)" = "$T/g0c1/g0" ] &&
[ "$(lcat "$T/g0c1.ledger" | cut -f7)" = "$(sha256sum < "$T/g0c1/g0/SHA256SUMS" | cut -d' ' -f1)" ] && [ "$(lcat "$T/g0c1.ledger" | cut -f8)" = "$(sha256sum < "$T/PARAMS2" | cut -d' ' -f1)" ] &&
  ok "G0 ledger: the FAIL appended (config, server, identity, verdict, record dir, SHA256SUMS sha, PARAMS sha)" || bad "G0 ledger line: $(lcat "$T/g0c1.ledger")"
[ "$(lcat "$T/g0c3.ledger" | cut -f2,4,5)" = "$(printf 'A\t%s\tPASS' "$ID2")" ] && [ "$(lcat "$T/g0c3.ledger" | cut -f7)" = "$(sha256sum < "$T/g0c3/g0/SHA256SUMS" | cut -d' ' -f1)" ] &&
  ok "G0 ledger: a PASS appended too, with its record's SHA256SUMS sha" || bad "G0 ledger PASS line: $(lcat "$T/g0c3.ledger")"
[ "$(find "$T/g0c1.ledger" -type f | wc -l)" = 1 ] && [ "$(find "$T/g0c3.ledger" -type f | wc -l)" = 1 ] && h1="$(lcat "$T/g0c1.ledger" | sha256sum)" &&
  not env EXP445_G0_LEDGER="$T/g0c1.ledger" bash -c ". '$HERE/reader.sh'; g0_append \"\$1\"" _ "$(lcat "$T/g0c1.ledger" | sed 's/FAIL/PASS/')" 2> "$T/ga.err" &&
  grep -q "exists already" "$T/ga.err" && [ "$(lcat "$T/g0c1.ledger" | sha256sum)" = "$h1" ] && [ "$(find "$T/g0c1.ledger" -type f | wc -l)" = 1 ] &&
  ok "G0 ledger: one new file per verdict (evidence stays append-only); an entry with the same name is never overwritten (a FAIL cannot become a PASS)" || bad "G0 ledger files"
g0refuse() { # g0refuse <name> <ledger> <PARAMS>: g0.sh refused with exit 3 and the ledger reason, nothing written
  local e=0; rm -rf "$T/g0again"; env EXP445_G0_LEDGER="$2" EXP445_PARAMS="$3" "$HERE/g0.sh" --config A --out "$T/g0again" > /dev/null 2> "$T/g0again.err" || e=$?
  [ "$e" = 3 ] && grep -q "a G0 FAIL for config A on this server and settings is in" "$T/g0again.err" && [ ! -e "$T/g0again" ] &&
    ok "refused (exit 3, nothing written): $1" || bad "$1: exit $e $(tail -1 "$T/g0again.err")"
}
g0refuse "g0.sh in a new run dir after a G0 FAIL for the same config, server and settings" "$T/g0c1.ledger" "$T/PARAMS2"
# an edit elsewhere in PARAMS (a comment, the stop rule) does not lift the bar (the identity is not the PARAMS sha)
{ cat "$T/PARAMS2"; echo "# a comment added later"; } | sed 's/^futility_max=.*/futility_max=29/' > "$T/PARAMS2e"
if [ "$(g0_ident "$T/PARAMS2e" A)" = "$ID2" ]; then g0refuse "g0.sh after a FAIL, PARAMS edited outside the configuration's identity" "$T/g0c1.ledger" "$T/PARAMS2e"; else bad "an edit outside the identity changed g0_ident"; fi
refuse "run.sh on a run dir with a G0 PASS while the ledger holds a FAIL for that config" "a G0 FAIL for config A on this server and settings is in" \
  env EXP445_G0_LEDGER="$T/g0c1.ledger" EXP445_PARAMS="$T/PARAMS2e" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0c3"
# negative controls: a FAIL for another server, another setting or another config bars nothing
sed 's|^config\.A\.server=.*|config.A.server=fake445 test server A2|' "$T/PARAMS2" > "$T/PARAMS2s"
sed 's|^temperature=.*|temperature=0.6|' "$T/PARAMS2" > "$T/PARAMS2t"
[ "$(EXP445_G0_LEDGER="$T/g0c1.ledger" g0_failed A "$ID2" | cut -f5)" = FAIL ] &&
[ -z "$(EXP445_G0_LEDGER="$T/g0c1.ledger" g0_failed A "$(g0_ident "$T/PARAMS2s" A)")" ] &&
[ -z "$(EXP445_G0_LEDGER="$T/g0c1.ledger" g0_failed A "$(g0_ident "$T/PARAMS2t" A)")" ] &&
[ -z "$(EXP445_G0_LEDGER="$T/g0c1.ledger" g0_failed B "$ID2")" ] &&
  ok "  negative controls: that FAIL does not bar another server, another temperature or config B" || bad "g0_failed matches too much"
EXP445_G0_LEDGER="$T/ledger-elsewhere" g0_ledger | grep -qx "$T/ledger-elsewhere" && [ "$(env -u EXP445_PARAMS EXP445_G0_LEDGER="$T/x" bash -c "ROOT=$ROOT; . $HERE/reader.sh; g0_ledger")" = "$ROOT/evidence/ONE-LAYER-2/g0-ledger" ] &&
  ok "  EXP445_G0_LEDGER is honoured only with EXP445_PARAMS (tests); otherwise the real ledger" || bad "g0_ledger override"
# run.sh needs the record's PASS line on the ledger: a G0 PASS record that the ledger does not know is refused
mkdir -p "$T/empty.ledger"
refuse "run.sh on a G0 PASS record with no PASS line on the ledger" "has no PASS line in" \
  env EXP445_G0_LEDGER="$T/empty.ledger" EXP445_PARAMS="$T/PARAMS2" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0c3"
mkdir -p "$T/other.ledger"; lcat "$T/g0c3.ledger" | awk -F"\t" -v OFS="\t" -v z="$(printf "%064d" 0)" '$5 == "PASS" { $7 = z } 1' > "$T/other.ledger/x.tsv"
refuse "run.sh when the ledger's PASS line names another record" "has no PASS line in" \
  env EXP445_G0_LEDGER="$T/other.ledger" EXP445_PARAMS="$T/PARAMS2" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0c3"
printf 'config.A.server=fake445\twith a tab\n' > "$T/tab.params"; grep -v '^config\.A\.server=' "$T/PARAMS2" >> "$T/tab.params"
refuse "g0.sh with a tab in the server name (a ledger column)" "must not contain a tab" env EXP445_PARAMS="$T/tab.params" "$HERE/g0.sh" --config A --out "$T/g0tab"
g0case "a server that ignores the seed (reports seed 0, the stage S mutant)" '{"ignore_seed": true, "p_repeat": 0.9}' FAIL 'gate=FAIL distinct=1/8 repeat_identical=yes sampled_own_seed=0/9' 'gate d FAIL a reply was not sampled with its own seed'
g0case "a server that forces greedy decoding (reports mode greedy, the stage S mutant)" '{"force_greedy": true, "p_repeat": 0.9}' FAIL 'gate=FAIL distinct=1/8 repeat_identical=yes sampled_own_seed=0/9' 'gate b 0/4 FAIL'
g0case "the gate's own replays differ (gates a and c fail) while dev01's identical answers are confident (gate d passes)" '{"vary_untasked": true, "p_repeat": 0.9}' FAIL 'gate=FAIL distinct=1/8 repeat_identical=yes sampled_own_seed=9/9' '^gate a 0/4 FAIL'
grep -q '^b p1 distinct=3/3 PASS ' "$G0LAST/g0/gate/gate.txt" && grep -qx 'gate c 0/4 FAIL' "$G0LAST/g0/gate/gate.txt" && grep -q '^gate d PASS identical replies explained by confidence' "$G0LAST/g0/gate/gate.txt" &&
  ok "  (there gate b passes on distinct replies, gate c fails, gate d passes: G0 needs the whole gate)" || bad "gate a/c failure record"
# a forged record: the p_repeat 0.5 FAIL with gate.txt's verdict, verdict.txt's derived and G0 lines edited to PASS and every hash redone
G0X="$T/g0c1"; sed -i '$s/.*/VERDICT PASS/' "$G0X/g0/gate/gate.txt"; (cd "$G0X/g0/gate" && find . -type f ! -name SHA256SUMS | sort | sed 's|^\./||' | xargs sha256sum > SHA256SUMS)
sed -i -e 's/^derived=gate=FAIL/derived=gate=PASS/' -e 's/^gate_exit=.*/gate_exit=0/' -e '$s/.*/G0 PASS/' "$G0X/g0/verdict.txt"; g0hash "$G0X"
refuse "run.sh with a forged G0 (gate verdict, gate exit, derived and G0 lines edited, all hashes redone)" "G0 verdict (gate=PASS distinct=1/8 repeat_identical=yes sampled_own_seed=9/9) differs from its raw files (gate=FAIL" \
  env EXP445_PARAMS="$T/PARAMS2" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$G0X"
# a transport failure writes no record (exit 2): an endpoint that refuses connections
sed "s|^config\.A\.base_url=.*|config.A.base_url=http://127.0.0.1:9/v1|" "$TP" > "$T/PARAMSD"
refuse "g0.sh against an endpoint that does not answer" "the sampling gate could not complete (exit 2" env EXP445_PARAMS="$T/PARAMSD" "$HERE/g0.sh" --config A --out "$T/g0dead"
[ ! -e "$T/g0dead/g0" ] && ok "  no G0 record written for a transport failure (the run dir can still be gated)" || bad "transport failure wrote a G0 record"
for s in $(g0seeds); do { echo "Answer for seed $s."; fence "$OMD/dev01.omegese"; } > "$A2/omegese/clamp_add.seed$s"; done
{ echo "Another answer."; fence "$OMD/dev01.omegese"; } > "$A2/omegese/clamp_add.seed446001.again"; rm -f "$A2"/seen.*
g0case "8 distinct answers, the sampling gate passes, but the repeat of seed 1 differs" - FAIL 'gate=PASS distinct=8/8 repeat_identical=no sampled_own_seed=9/9' '^VERDICT PASS$'
G0R="$G0LAST"
# run.sh refuses without a G0 PASS for this config, server and PARAMS
cw A "$T/nog0"
refuse "run.sh with no G0 record" "no G0 PASS for config A and this server in .*no G0 record" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/nog0"
refuse "run.sh with a G0 FAIL (repeat differs)" "no G0 PASS for config A .*G0 FAILED (gate=PASS distinct=8/8 repeat_identical=no" env EXP445_PARAMS="$T/PARAMS2" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$G0R"
sed -i '$s/.*/G0 PASS/' "$G0R/g0/verdict.txt"; g0hash "$G0R"
refuse "run.sh with a G0 FAIL whose verdict line was edited to PASS (re-hashed)" "G0 FAILED (gate=PASS distinct=8/8 repeat_identical=no" env EXP445_PARAMS="$T/PARAMS2" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$G0R"
cw B "$T/g0b"; cp -R "$G0P/g0" "$T/g0b/g0"
refuse "run.sh with config A's G0 in a config B run" "G0 was made for another config (A)" "$HERE/run.sh" --config B --tasks "$T/pub" --out "$T/g0b"
cw A "$T/g0s"; cp -R "$G0P/g0" "$T/g0s/g0"; sed -i 's/^server=.*/server=another server/' "$T/g0s/g0/verdict.txt"; g0hash "$T/g0s"
refuse "run.sh with a G0 made on another server (re-hashed)" "G0 was made for another server (another server)" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0s"
cw A "$T/g0t"; cp -R "$G0P/g0" "$T/g0t/g0"; sed -i 's/Answer/answer/' "$T/g0t/g0/gate/d-r3.json"
refuse "run.sh with a G0 response edited (not re-hashed)" "G0 SHA256SUMS does not verify" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0t"
cw A "$T/g0q"; cp -R "$G0P/g0" "$T/g0q/g0"; jqi() { jq -c "$1" "$2" > "$2.tmp"; mv "$2.tmp" "$2"; }; jqi '.temperature = 0' "$T/g0q/g0/r2.req.json"; g0hash "$T/g0q"
refuse "run.sh with a G0 request at another temperature (re-hashed)" "G0 request 2 is not the frozen G0 request" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0q"
cw A "$T/g0g"; cp -R "$G0P/g0" "$T/g0g/g0"; sed -i 's/>= 0.01/>= 0.001/' "$T/g0g/g0/sampling-gate.sh"; g0hash "$T/g0g"
refuse "run.sh with a G0 made by an edited sampling gate (re-hashed)" "G0 was made by another sampling gate script" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/g0g"
mut_make0() { rm -rf "$T/mg"; mkdir -p "$T/mg"; cp "$HERE/g0.sh" "$HERE/PARAMS" "$HERE/reader.sh" "$HERE/sampling-gate.sh" "$T/mg/"; cp -R "$HERE/arms" "$T/mg/arms"
  sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|" "$T/mg/g0.sh"; }
mut_make0; sed -i "s/at_least \"\$chance\" 0.01/at_least \"\$chance\" 0.001/" "$T/mg/sampling-gate.sh"; ! cmp -s "$T/mg/sampling-gate.sh" "$HERE/sampling-gate.sh" || bad "gate mutant: sed changed nothing"
refuse "g0.sh with an edited sampling gate script" "is not the frozen sampling gate" "$T/mg/g0.sh" --config A --out "$T/g0mg"
[ ! -e "$T/g0mg/g0" ] && ok "  nothing recorded by the refused G0" || bad "refused G0 wrote a record"
prep A "$T/locked"
refuse "driver on a run dir another driver holds (flock)" "another run.sh holds" flock "$T/locked/.lock" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/locked" --only dev01
[ ! -e "$T/locked/log.tsv" ] && ok "  the refused driver wrote no log.tsv" || bad "locked run dir got a log.tsv"
# a dev run of 3 tasks
R="$T/gens/run-a"; prep A "$R"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 > /dev/null 2> "$T/run.err" || { bad "run.sh failed"; tail -5 "$T/run.err"; }
lg() { awk -F'\t' -v s="$1" -v t="$2" -v c="$3" '$1 == s && $2 == t { print $c }' "$R/log.tsv"; }
[ "$(($(wc -l < "$R/log.tsv") - 1))" -eq 6 ] && grep -qx 'protocol=OVERRIDDEN: only' "$R/meta.txt" && grep -qx "base_url=$URL" "$R/meta.txt" && grep -qx 'request_extra={}' "$R/meta.txt" &&
grep -qx 'revision=r3' "$R/meta.txt" && grep -qx 'prompt_mode=many' "$R/meta.txt" && grep -qx 'order=attempt,task,arm' "$R/meta.txt" && grep -qx 'server=fake445 test server A' "$R/meta.txt" &&
  ok "6 generations (3 tasks x 2 arms) logged; meta.txt: revision r3, prompt mode many, order, server, the override recorded" || bad "log/meta"
[ "$(tail -n +2 "$R/log.tsv" | cut -f1,2 | tr '\t' ' ' | paste -sd,)" = "a-omegese-1 dev01,a-text-1 dev01,a-omegese-1 dev02,a-text-1 dev02,a-omegese-1 dev03,a-text-1 dev03" ] &&
  ok "order: task, then arm within attempt 1" || bad "generation order: $(tail -n +2 "$R/log.tsv" | cut -f1,2 | paste -sd,)"
[ "$(lg a-text-1 dev01 3)" = 1 ] && [ "$(lg a-text-1 dev01 10)" = OK ] && [ "$(lg a-text-1 dev02 3)" = 2 ] &&
[ "$(lg a-text-1 dev02 10)" = "REFUSED compiler: SYNTAX at line 1: expected an expression, found ';'" ] && [ "$(lg a-text-1 dev02 11)" = OK ] &&
[ "$(lg a-text-1 dev03 10)" = "REQUEST FAILED" ] && [ "$(lg a-text-1 dev03 12)" = - ] && [ "$(grep -c "^try=[012] http=500 curl_exit=0 time_s=[0-9.]* body=r1.err.[012].json msg=" "$R/gen/a-text-1/dev03/r1.transport")" = 3 ] &&
  ok "text arm: accepted, repaired after a compiler refusal, HTTP 500 retried twice then no candidate" || bad "text arm log"
[ "$(lg a-omegese-1 dev01 10)" = OK ] && [ -f "$R/cands/a-omegese-1/dev01/candidate.oscir" ] && [ "$(lg a-omegese-1 dev02 3)" = 2 ] &&
[[ "$(lg a-omegese-1 dev02 10)" == REFUSED* ]] && [ "$(lg a-omegese-1 dev02 11)" = OK ] &&
[ "$(lg a-omegese-1 dev03 7)" = length ] && [ ! -e "$R/cands/a-omegese-1/dev03" ] &&
  ok "omegese arm: accepted, repaired after '$(lg a-omegese-1 dev02 10)', an over-budget answer cut at max_tokens (no candidate)" || bad "omegese arm log"
cmp -s "$R/prompts/omegese.system.txt" "$T/p.omegese.many" && cmp -s "$R/prompts/text.system.txt" "$T/p.text.many" &&
  ok "run prompts: the many-shot prompts of both arms" || bad "run prompts"
same_budget() { jq -s -e --argjson m "$1" 'all(.[]; .max_tokens == $m and .temperature == 0.7 and .top_p == 0.8)' "$R"/gen/*/*/r*.req.json > /dev/null; }
seeds_equal() { for t in dev01 dev02; do [ "$(jq -s '[.[].seed] | unique | length' "$R"/gen/a-*-1/$t/r1.req.json)" = 1 ] || return 1; done; }
same_budget 3072 && seeds_equal && not same_budget 2048 && ok "every request: max_tokens 3072 (PARAMS), temperature 0.7, top_p 0.8; one seed per task and attempt across the arms" || bad "budget/sampling"
fixmsg() { jq -r '.messages[3].content' "$R/gen/$1/$2/r2.req.json" | tail -1; }
[ "$(fixmsg a-text-1 dev02)" = "Reply with the corrected complete program in one fenced code block." ] && [ "$(fixmsg a-omegese-1 dev02)" = "$(fixmsg a-text-1 dev02)" ] &&
  ok "repair round: same message format in both arms" || bad "repair message"
(cd "$R" && sha256sum -c --quiet SHA256SUMS) && grep -q ' g0/verdict.txt$' "$R/SHA256SUMS" && ok "every raw request/response and the G0 record hashed in SHA256SUMS (verifies)" || bad "SHA256SUMS"
refuse "resume with other settings" "resuming with different settings" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 --top-p 0.9
n0="$(grep -c . "$A/requests.log")"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$R" --only dev01,dev02,dev03 > /dev/null 2>&1
[ "$(grep -c . "$A/requests.log")" = "$n0" ] && ok "resume with the same settings sends no request again" || bad "resume repeated requests"
# the full dev matrix in both configurations, and config B in the one-shot mode
FA="$T/gens/full-a" FB="$T/gens/full-b" FBO="$T/gens/one-b"
prep A "$FA"; prep B "$FB"; prep B "$FBO"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$FA" > /dev/null 2>&1 && "$HERE/run.sh" --config B --tasks "$T/pub" --out "$FB" > /dev/null 2>&1 &&
  "$HERE/run.sh" --config B --tasks "$T/pub" --out "$FBO" --prompt-mode one > /dev/null 2>&1 || bad "full matrix runs failed"
has_kw() { jq -s -e 'all(.[]; .chat_template_kwargs == {"enable_thinking": false})' "$1"/gen/*/*/r*.req.json > /dev/null; }
no_kw() { jq -s -e 'all(.[]; has("chat_template_kwargs") | not)' "$1"/gen/*/*/r*.req.json > /dev/null; }
has_kw "$FB" && no_kw "$FA" && not has_kw "$FA" && grep -qx 'model=qwen3-8b' "$FB/meta.txt" && grep -qx 'protocol=PARAMS' "$FB/meta.txt" &&
  ok "config extras: B's in every B request, none in A's" || bad "config extras in requests"
grep -qx 'prompt_mode=one' "$FBO/meta.txt" && grep -qx 'protocol=PARAMS' "$FBO/meta.txt" && cmp -s "$FBO/prompts/omegese.system.txt" "$T/p.omegese.one" &&
  ok "--prompt-mode one: the one-shot prompts, recorded as prompt_mode=one (not an override)" || bad "one-shot run"
# driver mutants
mut_make() { rm -rf "$T/mut"; mkdir -p "$T/mut"; cp "$HERE/run.sh" "$HERE/PARAMS" "$HERE/reader.sh" "$HERE/g0.sh" "$HERE/sampling-gate.sh" "$T/mut/"; cp -R "$HERE/arms" "$T/mut/arms"
  sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|" "$T/mut/run.sh" "$T/mut/g0.sh"; cp "$T/mut/$1" "$T/mut/$1.orig"; sed -i "$2" "$T/mut/$1"
  if cmp -s "$T/mut/$1.orig" "$T/mut/$1"; then bad "mutant: sed changed nothing in $1 ($2)"; return 1; fi; }
mut_run() { mut_make "$2" "$3" || return 1; rm -rf "$T/mr"; rm -f "$T/mut.ledger"; cw "${4:-A}" "$T/mr"   # one fresh ledger per mutant, for both scripts
  EXP445_G0_LEDGER="$T/mut.ledger" "$T/mut/g0.sh" --config "${4:-A}" --out "$T/mr" > /dev/null 2>&1 || true
  EXP445_G0_LEDGER="$T/mut.ledger" "$T/mut/run.sh" --config "${4:-A}" --tasks "$T/pub" --out "$T/mr" --only dev01,dev02,dev03 > /dev/null 2>&1 || true; }
R0="$R"; R="$T/mr"
M="no repair round"; mut_run norepair run.sh 's/if \[ "\$k1" != OK \] \&\& ask/if false \&\& ask/' && { [ "$(lg a-text-1 dev02 11)" = OK ] && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
M="budget not sent"; mut_run nobudget reader.sh 's/max_tokens: \$mt, //' && { same_budget 3072 && bad "mutant survived: $M" || ok "  mutant caught: $M"; }
M="G0 check skipped"; mut_make run.sh 's/^  die "no G0 PASS/  true "no G0 PASS/' && { rm -rf "$T/mr"; cw A "$T/mr"
  "$T/mut/run.sh" --config A --tasks "$T/pub" --out "$T/mr" --only dev01 > /dev/null 2>&1 && [ -f "$T/mr/log.tsv" ] && ok "  mutant caught: $M (that run.sh generates without G0; the real one refused above)" || bad "mutant survived: $M"; }
R="$R0"
# transport failure: curl's elapsed time is kept (not 0), every try recorded with HTTP status and curl exit
tpos() { awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^time_s=/) { split($i, q, "="); if (!(q[2] > 0)) bad = 1 } } END { exit bad }' "$1"; }
prep A "$T/dead"
"$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/dead" --only dev01 --arms text --base-url http://127.0.0.1:1/v1 > /dev/null 2>&1 || true
DT="$T/dead/gen/a-text-1/dev01/r1.transport"
[ "$(grep -c '^try=[012] http=000 curl_exit=7 time_s=[0-9.]* body=none msg=curl: (7)' "$DT" 2>/dev/null)" = 3 ] && tpos "$DT" &&
[ "$(awk -F'\t' 'NR == 2 { print $10 }' "$T/dead/log.tsv")" = "REQUEST FAILED" ] && [ "$(awk -F'\t' 'NR == 2 { print ($9 > 0) }' "$T/dead/log.tsv")" = 1 ] &&
  { printf 'try=0 http=000 curl_exit=7 time_s=0 body=none msg=\n' > "$T/zero.tr"; not tpos "$T/zero.tr"; } &&
  ok "refused connection: 3 tries recorded (HTTP 000, curl exit 7, curl's own time > 0), row REQUEST FAILED with wall > 0 (a time of 0 is caught)" || bad "transport failure record"
# interrupted runs: killed after a round-1 response (before round 2) and after the log append (before
# done); resuming sends no request for a received round and leaves no duplicate row
tries() { cat "$1"/gen/*/*/r*.transport | wc -l; }
nreq() { grep -c . "$A/requests.log"; }
kill_seq() { # kill_seq <run.sh> <out>: prints the number of requests the three invocations sent
  local n0; rm -rf "$2"; cw A "$2"; cp -R "$FA/g0" "$2/g0"; n0="$(nreq)"
  EXP445_TEST_KILL=after-r1:a-text-1:dev02 "$1" --config A --tasks "$T/pub" --out "$2" > /dev/null 2>&1 && return 1
  [ -f "$2/gen/a-text-1/dev02/r1.transport" ] && [ ! -e "$2/gen/a-text-1/dev02/r2.req.json" ] && [ ! -e "$2/gen/a-text-1/dev02/done" ] || return 1
  EXP445_TEST_KILL=after-log:a-omegese-1:dev04 "$1" --config A --tasks "$T/pub" --out "$2" > /dev/null 2>&1 && return 1
  [ "$(awk -F'\t' '$1 == "a-omegese-1" && $2 == "dev04"' "$2/log.tsv" | wc -l)" = 1 ] && [ ! -e "$2/gen/a-omegese-1/dev04/done" ] || return 1
  "$1" --config A --tasks "$T/pub" --out "$2" > /dev/null 2>&1 || true
  echo $(($(nreq) - n0))
}
K="$T/gens/killed-a"
nk="$(kill_seq "$HERE/run.sh" "$K")" || nk=x
nocol9() { tail -n +2 "$1/log.tsv" | cut -f1-8,10-12 | LC_ALL=C sort; }
[ "$nk" = "$(tries "$K")" ] && [ "$nk" = "$(tries "$FA")" ] && [ "$(tail -n +2 "$K/log.tsv" | cut -f1,2 | sort | uniq -d | wc -l)" = 0 ] &&
[ "$(nocol9 "$K")" = "$(nocol9 "$FA")" ] && (cd "$K" && sha256sum -c --quiet SHA256SUMS) &&
  ok "killed twice and resumed: $nk requests in all, the same as one uninterrupted run; 20 rows, no duplicate, same rows" || bad "kill/resume (requests $nk, tries $(tries "$K") vs $(tries "$FA"))"
M="resume requests a committed round again"
mut_make reader.sh 's/if \[ ! -f "\$p.transport" \]; then/if true; then/' &&
  { n="$(kill_seq "$T/mut/run.sh" "$T/mk")" || n=x; [ "$n" = "$(tries "$FA")" ] && bad "mutant survived: $M" || ok "  mutant caught: $M ($n requests)"; }
M="resume appends the row again"
mut_make run.sh 's/if \[ -z "\$have" \]; then/if true; then/' &&
  { kill_seq "$T/mut/run.sh" "$T/mk" > /dev/null || true; [ "$(tail -n +2 "$T/mk/log.tsv" | wc -l)" = 20 ] && bad "mutant survived: $M" || ok "  mutant caught: $M"; }

# ---- 4. score.sh: provenance first, then the sealed harness, then the results ------------------------
# the gate of 13.6 at its boundaries (omegese attempt-1, text attempt-1, tasks -> expected)
gres=""; for gc in "8 9 10 PASS" "8 10 10 FAIL" "7 7 10 FAIL" "48 54 60 PASS" "48 55 60 FAIL" "47 47 60 FAIL" "50 40 60 PASS" "60 60 60 PASS" "0 0 0 FAIL"; do
  set -- $gc; if gate_config "$1" "$2" "$3"; then r=PASS; else r=FAIL; fi; [ "$r" = "$4" ] || gres="$gres $1/$3 vs $2/$3 gave $r;"; done
[ -z "$gres" ] && ok "gate 13.6 boundaries: 8 vs 9 of 10 PASS, 8 vs 10 FAIL, 7 FAIL; 48 vs 54 of 60 PASS (exactly 10 points), 48 vs 55 FAIL, 47 FAIL" || bad "gate 13.6:$gres"
SC() { EXP445_TEST=1 OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$HERE/score.sh" "$@"; }
PK=("$T/pack/tasks.tar" "$T/pack/TASKS.sha256"); HPK=("$T/hp/tasks.tar" "$T/hp/TASKS.sha256")
if SC "${PK[@]}" "$FA" "$FB" > "$T/score.out" 2> "$T/score.err"; then
  S="$(sed -n 's/^score.sh: results in //p' "$T/score.err")"
  row() { awk -F'\t' -v c="$1" -v a="$2" -v k="$3" 'NR == 1 { for (i = 1; i <= NF; i++) h[$i] = i; next } $1 == c && $2 == a { print $h[k] }' "$S/results.tsv"; }
  [ "$(row a omegese success_attempt1)" = 3/10 ] && [ "$(row a text success_attempt1)" = 2/10 ] && [ "$(row b omegese success_attempt1)" = 3/10 ] &&
  [ "$(row a omegese success_attempt1_round1)" = 2/10 ] && [ "$(row a text success_attempt1_round1)" = 1/10 ] &&
  [ "$(row a text p1_lo)" = "$("$X" wilson 2 10 | cut -d' ' -f2)" ] && [ "$(row a omegese repaired_accepted)" = 1 ] &&
  [ "$(row a text completion_tokens_total)" = "$(awk -F'\t' '$1 == "a-text-1" { s += $5 + ($6 == "-" ? 0 : $6) } END { printf "%.1f", s }' "$FA/log.tsv")" ] &&
  head -1 "$S/results.txt" | grep -q '^TEST MODE, NOT EVIDENCE.*exp445 r3 results.*prompt mode many' &&
  grep -q '^cost config a: omegese / text mean completion tokens per generation x[0-9.]* ' "$S/results.txt" &&
  grep -qx 'paired attempt 1 config a: both 2, only omegese 1, only text 0, neither 7; exact two-sided McNemar p = 1.000000' "$S/results.txt" &&
  grep -q 'round 1 only 2/10' "$S/results.txt" &&
  grep -qx 'gate config a: omegese 3/10, text 2/10: FAIL (needs omegese >= 80% and at most 10 points below text)' "$S/results.txt" &&
  grep -q '^GATE FAIL ' "$S/results.txt" && grep -q '^provenance OK: 2 runs, 40 generations, configs A,B, prompt mode many$' "$S/validity.txt" &&
    ok "score.sh: success 3/10 omegese vs 2/10 text (round 1 alone 2/10 vs 1/10), Wilson, cost ratio, paired McNemar, gate 13.6 per config; TEST MODE" ||
    { bad "score.sh results"; cat "$S/results.txt"; }
  awk -F'\t' '$1 == "a-omegese-1" && $2 == "dev03" { print $3, $4 }' "$S/verdicts.tsv" | grep -qx 'REJECT no_candidate' &&
  awk -F'\t' '$1 == "a-omegese-1" && $2 == "dev04" { print $3 }' "$S/verdicts.tsv" | grep -qx 'ACCEPT' &&
    ok "score.sh verdicts: the over-budget answer no_candidate, the Omegese gcd ACCEPT" || bad "score verdicts"
else bad "score.sh failed"; tail -5 "$T/score.err"; fi
if SC --configs A "${PK[@]}" "$FA" > "$T/s1.out" 2> /dev/null; then
  grep -q '^GATE NOT EVALUATED: configs scored a, the protocol needs a,b' "$T/s1.out" && ok "score.sh --configs A: config A alone is scored, the gate is not evaluated" || bad "subset gate line"
else bad "score.sh --configs A failed"; fi
if SC --configs B "${PK[@]}" "$FBO" > "$T/s2.out" 2> "$T/s2.err"; then
  grep -q 'prompt mode one$' "$(sed -n 's/^score.sh: output in //p' "$T/s2.err")/validity.txt" && ok "score.sh: a one-shot run scores against the one-shot prompts" || bad "one-shot score validity line"
else bad "score.sh refused the one-shot run: $(grep INVALID "$T/s2.err" | head -2)"; fi
# negative controls: each provenance failure gives RUN INVALID, exit 1 and no results table
invalid() { # invalid <name> <reason grep> <score.sh args...>
  local n="$1" why="$2"; shift 2
  if SC "$@" > "$T/inv.out" 2> "$T/inv.err"; then bad "$n: scored"; return; fi
  local s; s="$(sed -n 's/^score.sh: output in //p' "$T/inv.err")"
  grep -q '^RUN INVALID' "$T/inv.err" && grep -q -- "$why" "$s/validity.txt" && [ ! -e "$s/results.txt" ] && [ ! -s "$T/inv.out" ] &&
    ok "RUN INVALID, no table: $n" || { bad "$n: $(head -3 "$T/inv.err")"; }
}
# smut <sed> <check name> <score.sh args...>: score.sh with that check removed must SCORE the case the real
# score.sh refuses (the check is what stops it)
smut() {
  local s="$1" n="$2"; shift 2
  mkdir -p "$T/smut"; sed "$s" "$HERE/score.sh" > "$T/smut/score.sh"; chmod +x "$T/smut/score.sh"
  sed -i "s|^ROOT=.*|ROOT=\"$ROOT\"|; s|^HERE=.*|HERE=\"$HERE\"|" "$T/smut/score.sh"
  if cmp -s "$HERE/score.sh" "$T/smut/score.sh"; then bad "score mutant ($n): sed changed nothing"
  elif EXP445_TEST=1 OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid" "$T/smut/score.sh" "$@" > /dev/null 2>&1; then ok "  mutant caught: score.sh without $n scores it"
  else bad "  score mutant ($n): still refused, that check is not the only guard tested"; fi
}
cpr() { rm -rf "$T/neg"; mkdir -p "$T/neg"; cp -R "$FA" "$T/neg/a"; cp -R "$FB" "$T/neg/b"; }
NA=(--configs A "${PK[@]}" "$T/neg/a"); NB=(--configs B "${PK[@]}" "$T/neg/b")
cpr; printf ' ' >> "$T/neg/a/cands/a-text-1/dev01/candidate.osc"
invalid "tampered candidate" "a-text-1 dev01 candidate in cands/ does not match" "${NA[@]}"
cpr; sed -i 's/"stop"/"stop" /' "$T/neg/b/gen/b-text-1/dev01/r1.resp.json"
invalid "tampered response" "SHA256SUMS does not verify" "${NB[@]}"
cpr; rm -rf "$T/neg/b/gen/b-omegese-1/dev07" "$T/neg/b/cands/b-omegese-1/dev07"; awk -F'\t' '!($1 == "b-omegese-1" && $2 == "dev07")' "$FB/log.tsv" > "$T/neg/b/log.tsv"; rehash "$T/neg/b"
invalid "missing generation (re-hashed)" "1 of 20 generations missing (first: b-omegese-1 dev07)" "${NB[@]}"
cpr; awk -F'\t' -v OFS='\t' '$1 == "b-omegese-1" { $5 = 1 } { print }' "$FB/log.tsv" > "$T/neg/b/log.tsv"
invalid "edited token column (not re-hashed)" "b: SHA256SUMS does not verify" "${NB[@]}"
rehash "$T/neg/b"
invalid "edited token column (re-hashed: only the cross-check sees it)" "log.tsv differs from the rows re-derived from the raw files (first: b-omegese-1" "${NB[@]}"
smut 's/^  if ! cmp -s "\$W\/derived_s" "\$W\/log_s"; then$/  if false; then/' "the log.tsv cross-check" "${NB[@]}"
cpr; jqi '.usage.prompt_tokens = "a[$(touch '"$T"'/pwned)]"' "$T/neg/b/gen/b-text-1/dev01/r1.resp.json"
awk -F'\t' -v OFS='\t' '$1 == "b-text-1" && $2 == "dev01" { $4 = "BAD" } { print }' "$FB/log.tsv" > "$T/neg/b/log.tsv"; rehash "$T/neg/b"
invalid "non-integer token count in a response (re-hashed)" "b-text-1 dev01 token counts in the response are not non-negative integers (BAD" "${NB[@]}"
[ ! -e "$T/pwned" ] && ok "  the token payload was never evaluated" || bad "token payload evaluated by the shell"
cpr; awk -F'\t' -v OFS='\t' '$1 == "a-omegese-1" && $2 == "dev01" { $12 = "0000" } { print }' "$FA/log.tsv" > "$T/neg/a/log.tsv"; rehash "$T/neg/a"
invalid "log candidate hash edited (re-hashed)" "log.tsv differs from the rows re-derived from the raw files (first: a-omegese-1 dev01)" "${NA[@]}"
rm -rf "$T/hov"; prep A "$T/hov"; "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/hov" --temperature 0.6 > /dev/null 2>&1
sed -i 's/^protocol=.*/protocol=PARAMS/; s/^temperature=.*/temperature=0.7/' "$T/hov/meta.txt"; rehash "$T/hov"
invalid "override hidden by editing meta.txt (re-hashed)" "a-omegese-1 dev01 r1 request differs from the frozen request in: temperature" --configs A "${PK[@]}" "$T/hov"
cpr; jqi '.seed += 1' "$T/neg/a/gen/a-text-1/dev01/r1.req.json"; rehash "$T/neg/a"
invalid "wrong seed (re-hashed)" "a-text-1 dev01 r1 request differs from the frozen request in: seed" "${NA[@]}"
cpr; jqi 'del(.chat_template_kwargs)' "$T/neg/b/gen/b-text-1/dev01/r1.req.json"; rehash "$T/neg/b"
invalid "B request without its extra field (re-hashed)" "b-text-1 dev01 r1 request differs from the frozen request in: chat_template_kwargs" "${NB[@]}"
cpr; cp "$FA"/gen/a-text-1/dev02/r2.{req.json,transport,resp.json} "$T/neg/a/gen/a-text-1/dev01/"; rehash "$T/neg/a"
invalid "round 2 without a round-1 refusal (re-hashed)" "a-text-1 dev01 has a round 2 without a round-1 refusal" "${NA[@]}"
cpr; rm "$T/neg/a/gen/a-omegese-1/dev02"/r2.*; rehash "$T/neg/a"
invalid "refused round 1 without its repair round (re-hashed)" "a-omegese-1 dev02 round 1 was refused (REFUSED" "${NA[@]}"
cpr; sed -i '$d' "$T/neg/a/gen/a-text-1/dev03/r1.transport"; rehash "$T/neg/a"
invalid "failed round with a try missing (re-hashed)" "a-text-1 dev03 failed round r1 has 2 tries, the protocol makes 3" "${NA[@]}"
cpr; sed -i '1s/: OK$/: FAILED/' "$T/neg/a/weights.check"; rehash "$T/neg/a"
invalid "a FAILED weight line (re-hashed)" "a: weights.check / server.json missing or not all OK for config A" "${NA[@]}"
sed 's/^temperature=.*/temperature=0.6/' "$TP" > "$T/PARAMS3"
EXP445_PARAMS="$T/PARAMS3" invalid "changed PARAMS" "PARAMS changed since generation" "${PK[@]}" "$FA" "$FB"
invalid "override flag used" "settings overridden on the command line (OVERRIDDEN: only)" --configs A "${PK[@]}" "$R"
invalid "same run twice" "generations appear more than once" "${PK[@]}" "$FA" "$FA" "$FB"
invalid "config outside --configs" "config 'B' is not one of the configs scored (A)" --configs A "${PK[@]}" "$FA" "$FB"
cpr; printf ' ' >> "$T/neg/a/cands/a-text-1/dev01/candidate.osc"
smut 's/\[ "\$n" -le 1 \] && \[ "\$got" = "\$want" \] ||/true ||/' "the cands/ check" "${NA[@]}"
# r3-specific provenance: revision, order, prompt mode, G0
cpr; sed -i '/^revision=r3$/d' "$T/neg/a/meta.txt"; rehash "$T/neg/a"
invalid "an r2 run (meta.txt without revision=r3, re-hashed)" "not an r3 run (meta.txt has no revision=r3): r1/r2 runs are not scored" "${NA[@]}"
cpr; sed -i 's/^order=.*/order=arm,attempt,task/' "$T/neg/a/meta.txt"; rehash "$T/neg/a"
invalid "another generation order (re-hashed)" "generation order is not attempt,task,arm" "${NA[@]}"
invalid "a many-shot and a one-shot run scored together" "prompt mode one differs from the other runs' (many)" "${PK[@]}" "$FA" "$FBO"
rm -rf "$T/neg"; mkdir -p "$T/neg"; cp -R "$FBO" "$T/neg/b"; sed -i 's/^prompt_mode=one$/prompt_mode=many/' "$T/neg/b/meta.txt"; rehash "$T/neg/b"
invalid "a one-shot run relabelled many (re-hashed)" "arm omegese prompt differs from the current arm documents and many examples" "${NB[@]}"
cpr; rm -rf "$T/neg/a/g0"; rehash "$T/neg/a"
invalid "no G0 record (re-hashed)" "no G0 PASS for config A and its server: no G0 record" "${NA[@]}"
smut 's/|| inval "\$r: no G0 PASS/|| true "/; s/|| inval "\$r: the G0 record is not the one/|| true "/' "the G0 checks (PASS and run binding)" "${NA[@]}"
cpr; sed -i 's/Answer/answer/' "$T/neg/a/g0/gate/d-r4.json"; rehash "$T/neg/a"
invalid "a G0 response edited (run re-hashed, G0 not)" "G0 SHA256SUMS does not verify" "${NA[@]}"
cpr; rm -rf "$T/neg/a/g0"; cp -R "$G0R/g0" "$T/neg/a/g0"; rehash "$T/neg/a"
invalid "a G0 made with another PARAMS" "G0 was made for another base_url" "${NA[@]}"
cpr; rm -rf "$T/neg/a/g0"; cp -R "$G0P/g0" "$T/neg/a/g0"; rehash "$T/neg/a"
invalid "another G0 PASS swapped in after generation (re-hashed)" "the G0 record is not the one this run was generated under" "${NA[@]}"
smut 's/|| inval "\$r: the G0 record is not the one/|| true "/' "the run's G0 record check" "${NA[@]}"
# the G0 ledger at scoring: a FAIL for the config's identity, or no PASS line for the run's G0 record
cpr; mkdir -p "$T/fail.ledger"; { lcat "$T/g0-ledger"; printf '20261015T000000Z\tA\tfake445 test server A\t%s\tFAIL\telsewhere/g0\t%064d\t-\n' "$(g0_ident "$TP" A)" 0; } > "$T/fail.ledger/all.tsv"
EXP445_G0_LEDGER="$T/fail.ledger" invalid "a G0 FAIL on the ledger for config A's server and settings" "a G0 FAIL for config A on this server and settings is in" "${NA[@]}"
EXP445_G0_LEDGER="$T/fail.ledger" smut 's/\[ -z "\$fl" \] || inval "\$r: a G0 FAIL/true "/' "the ledger FAIL check" "${NA[@]}"
EXP445_G0_LEDGER="$T/empty.ledger" invalid "the run's G0 PASS record has no PASS line on the ledger" "has no PASS line in" "${NA[@]}"
rm -rf "$T/rsw"; cp -R "$FA" "$T/rsw"; rm -rf "$T/rsw/g0"; cp -R "$G0P/g0" "$T/rsw/g0"
refuse "run.sh resuming a run whose G0 record was swapped" "resuming with different settings" "$HERE/run.sh" --config A --tasks "$T/pub" --out "$T/rsw"
cpr; cp "$T/neg/a/gen/a-omegese-1/dev02/r1.req.json" "$T/neg/a/gen/a-omegese-1/dev02/r10.req.json"; rehash "$T/neg/a"
invalid "a round 10 request in a generation (re-hashed)" "has a round other than 1 and 2" "${NA[@]}"
if SC "${PK[@]}" "$K" "$FB" > /dev/null 2> "$T/ks.err"; then
  grep -q '^provenance OK: 2 runs, 40 generations' "$(sed -n 's/^score.sh: output in //p' "$T/ks.err")/validity.txt" &&
    ok "score.sh: the killed-and-resumed run is valid (provenance OK, no duplicate)" || bad "killed run validity line"
else bad "score.sh refused the killed-and-resumed run: $(grep INVALID "$T/ks.err" | head -2)"; fi

# ---- 5. Stage 1 stop rule on a held-out-like pack (h01..h04, heldout_tasks=4, attempts 2) -------------
# omegese attempt 1 on h01..h04 = 3/4: futility_max 3 -> STOP, futility_max 2 -> CONTINUE
export EXP445_PARAMS="$T/PARAMSH"
H1="$T/gens/h-stop"; prep A "$H1"
"$HERE/run.sh" --config A --tasks "$T/hpub" --out "$H1" > /dev/null 2> "$T/h1.err" || bad "held-out run attempt 1 failed: $(tail -2 "$T/h1.err")"
[ "$(tail -n +2 "$H1/log.tsv" | cut -f1 | sort -u | paste -sd,)" = "a-omegese-1,a-text-1" ] && grep -q 'score attempt 1 with tools/exp445/score.sh --interim' "$T/h1.err" &&
  (cd "$H1" && sha256sum -c --quiet SHA256SUMS) && ok "held-out run: the driver stops after attempt 1 and asks for the interim score (8 generations, hashed)" || bad "held-out pause after attempt 1"
if SC --interim --configs A "${HPK[@]}" "$H1" > "$T/hi.out" 2> "$T/hi.err"; then
  SI="$(sed -n 's/^score.sh: results in //p' "$T/hi.err")"; FUT="$SI/futility.txt"
  grep -q "^run=$H1 config=A omegese_attempt1=3/4 futility_max=3 a1_rows_sha256=[0-9a-f]\{64\} decision=STOP$" "$FUT" &&
  head -1 "$SI/results.txt" | grep -q 'INTERIM: attempt 1 only' && grep -q '^GATE NOT EVALUATED: interim' "$SI/results.txt" &&
    ok "score.sh --interim: attempt 1 scored, futility.txt: omegese 3/4 <= 3 -> decision=STOP, gate not evaluated" || { bad "interim score"; cat "$FUT" "$SI/results.txt"; }
else bad "score.sh --interim failed: $(grep INVALID "$T/hi.err" | head -2)"; FUT=/nonexistent; fi
sed 's/a1_rows_sha256=[0-9a-f]*/a1_rows_sha256=0000/' "$FUT" > "$T/fut.bad"
refuse "run.sh --futility made from other attempt-1 rows" "was made from other attempt-1 rows than this run" "$HERE/run.sh" --config A --tasks "$T/hpub" --out "$H1" --futility "$T/fut.bad"
rm -rf "$T/h-cont-mut"; cp -R "$H1" "$T/h-cont-mut"; sed "s|^run=$H1 |run=$T/h-cont-mut |; s/decision=STOP/decision=CONTINUE/" "$FUT" > "$T/fut.cont"
"$HERE/run.sh" --config A --tasks "$T/hpub" --out "$H1" --futility "$FUT" > /dev/null 2> "$T/h1b.err" || bad "run.sh --futility STOP failed"
[ "$(tail -n +2 "$H1/log.tsv" | wc -l)" = 8 ] && grep -q 'stop rule fired, attempts 2 and later are not run' "$T/h1b.err" &&
  ok "run.sh --futility (decision STOP): attempts 2 and later not run" || bad "STOP decision not followed by run.sh"
if SC --configs A "${HPK[@]}" "$H1" > "$T/hf.out" 2> "$T/hf.err"; then
  grep -qx 'stop rule config a (omegese attempt 1 <= 3): STOP, attempts 2 and later not run' "$T/hf.out" &&
    ok "final score of the stopped run: valid, the stop rule recorded in results.txt" || { bad "stopped run results"; cat "$T/hf.out"; }
else bad "score.sh refused the stopped held-out run: $(grep INVALID "$T/hf.err" | head -2)"; fi
# a driver that ignores the decision (fed CONTINUE although the rule fired) runs attempt 2: score.sh refuses
"$HERE/run.sh" --config A --tasks "$T/hpub" --out "$T/h-cont-mut" --futility "$T/fut.cont" > /dev/null 2>&1 || true
[ "$(tail -n +2 "$T/h-cont-mut/log.tsv" | wc -l)" = 16 ] || bad "CONTINUE mutant did not run attempt 2"
invalid "attempts run although the stop rule fired" "the stop rule fired (omegese attempt 1 3/4 <= 3) but attempts 2..2 were run" --configs A "${HPK[@]}" "$T/h-cont-mut"
# CONTINUE: futility_max 2
export EXP445_PARAMS="$T/PARAMSC"
H2="$T/gens/h-cont"; prep A "$H2"
"$HERE/run.sh" --config A --tasks "$T/hpub" --out "$H2" > /dev/null 2>&1 || bad "held-out run (CONTINUE) attempt 1 failed"
invalid "attempt 2 missing although the stop rule did not fire" "attempts 2..2 are missing but the stop rule did not fire (omegese attempt 1 3/4 > 2)" --configs A "${HPK[@]}" "$H2"
if SC --interim --configs A "${HPK[@]}" "$H2" > /dev/null 2> "$T/hi2.err"; then
  FUT2="$(sed -n 's/^score.sh: results in //p' "$T/hi2.err")/futility.txt"
  # the resume after attempt 1 happens at a later commit (the G0 ledger and the interim scores are committed
  # between): the tools must be what decides, not the commit id. Negative control first: a run whose
  # commit has other tools (the repository's first commit) is refused.
  cp "$H2/meta.txt" "$T/h2meta"; MC="$(sed -n 's/^commit=//p' "$H2/meta.txt")"
  sed -i "s/^commit=.*/commit=$(git -C "$ROOT" rev-list --max-parents=0 HEAD | tail -n 1)${MC#"${MC%-dirty}"}/" "$H2/meta.txt"
  refuse "resume when the tools changed since the run's commit" "tool files differ" "$HERE/run.sh" --config A --tasks "$T/hpub" --out "$H2" --futility "$FUT2"
  cp "$T/h2meta" "$H2/meta.txt"
  if [ "$MC" = "${MC%-dirty}" ]; then
    # a later commit with the very same tree, like the ledger commit (an object only; no ref moves)
    LC="$(git -C "$ROOT" -c user.name=exp445-test -c user.email=exp445-test@invalid commit-tree "HEAD^{tree}" -p HEAD -m "exp445 test: a later commit, same tools")"
    sed -i "s/^commit=.*/commit=$LC/" "$H2/meta.txt"
  else echo "skip resume across commits (tree dirty; meta_resume unit tests cover it, CI runs it clean)"; fi
  grep -q "decision=CONTINUE$" "$FUT2" && "$HERE/run.sh" --config A --tasks "$T/hpub" --out "$H2" --futility "$FUT2" > /dev/null 2> "$T/h2b.err" &&
  [ "$(tail -n +2 "$H2/log.tsv" | wc -l)" = 16 ] &&
  [ "$(tail -n +2 "$H2/log.tsv" | cut -f1,2 | tr '\t' ' ' | paste -sd,)" = "$(for at in 1 2; do for t in h01 h02 h03 h04; do for a in omegese text; do printf 'a-%s-%s %s,' "$a" "$at" "$t"; done; done; done | sed 's/,$//')" ] &&
    ok "decision CONTINUE: attempt 2 run; 16 generations in the order attempt, then task, then arm" || bad "CONTINUE run: $(tail -n +2 "$H2/log.tsv" | cut -f1,2 | paste -sd, | head -c 300) / $(tail -n 3 "$T/h2b.err" 2> /dev/null | tr "\n" " ")"
  if SC --configs A "${HPK[@]}" "$H2" > "$T/hc.out" 2> "$T/hc.err"; then
    grep -qx 'stop rule config a (omegese attempt 1 <= 2): CONTINUE' "$T/hc.out" && ok "final score of the continued run: valid, stop rule CONTINUE" || { bad "continued run results"; cat "$T/hc.out"; }
    grep -qx 'distinct candidates config a arm omegese: 1.00 per task over 2.00 attempts; every attempt identical on 4 of 4 tasks with 2+ attempts' "$T/hc.out" &&
    grep -qx 'distinct candidates config a arm text: 1.25 per task over 2.00 attempts; every attempt identical on 3 of 4 tasks with 2+ attempts' "$T/hc.out" &&
      ok "  distinct candidates per task reported (13.5): text 1.25 (h01 attempt 2 is another program), omegese 1.00 (the same program every attempt)" || { bad "distinct candidates line"; grep distinct "$T/hc.out"; }
  else bad "score.sh refused the continued held-out run: $(grep INVALID "$T/hc.err" | head -2)"; fi
else bad "score.sh --interim (CONTINUE) failed"; fi
unset EXP445_PARAMS
# meta_resume on a scratch repository: the resume rule by itself, whatever state this checkout is in
MR="$T/mr"; mkdir -p "$MR/tools" "$MR/evidence"; git -C "$MR" init -q
gc_() { git -C "$MR" add -A && git -C "$MR" -c user.name=t -c user.email=t@t commit -q -m "$1" && git -C "$MR" rev-parse HEAD; }
echo a > "$MR/tools/x"; C1="$(gc_ one)"; echo l > "$MR/evidence/ledger"; C2="$(gc_ ledger)"; echo b > "$MR/tools/x"; C3="$(gc_ tools)"
mm() { printf 'config=A\nmodel=m\ncommit=%s\ntasks=4\n' "$1" > "$T/mr.$2"; }
mrok() { mm "$2" a; mm "$3" b; [ -n "${4:-}" ] && sed -i 's/^tasks=4$/tasks=5/' "$T/mr.b"; meta_resume "$MR" "$T/mr.a" "$T/mr.b" > "$T/mr.why"; }
mrok x "$C1" "$C1" && ok "meta_resume: identical meta.txt resumes" || bad "meta_resume identical"
mrok x "$C1" "$C2" && ok "meta_resume: a later commit that touches only evidence resumes (the G0 ledger commit)" || bad "meta_resume ledger commit: $(cat "$T/mr.why")"
mrok x "$C1" "$C3" && bad "meta_resume accepted changed tools" || { grep -q "tool files differ" "$T/mr.why" && ok "meta_resume: changed tool files refused (negative control)" || bad "meta_resume tools reason: $(cat "$T/mr.why")"; }
mrok x "$C1" "$C2-dirty" && bad "meta_resume accepted a dirty tree across commits" || { grep -q "dirty" "$T/mr.why" && ok "meta_resume: a dirty tree across commits refused" || bad "meta_resume dirty reason"; }
mrok x "$C1" "$C2" y && bad "meta_resume accepted another setting" || { grep -q "settings differ" "$T/mr.why" && ok "meta_resume: another setting refused even with the same tools" || bad "meta_resume settings reason"; }
mrok x "$C1" "0000000000000000000000000000000000000000" && bad "meta_resume accepted an unknown commit" || ok "meta_resume: an unknown commit refused"
if mrok x HEAD "$C2"; then bad "meta_resume accepted a revision name (commit=HEAD)"
elif grep -q "not a full commit id" "$T/mr.why"; then ok "meta_resume: a revision name (commit=HEAD) refused"
else bad "meta_resume revision name reason: $(cat "$T/mr.why")"; fi
echo
if [ "$fails" -eq 0 ]; then echo "test-exp445: PASS"; else echo "test-exp445: FAIL ($fails)"; exit 1; fi
