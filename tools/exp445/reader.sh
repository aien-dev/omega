# tools/exp445/reader.sh -- sourced by run.sh and score.sh: the one definition of how a request is built,
# how an answer becomes a candidate and how a generation's log row follows from its raw files
# (omega#445, docs/one_layer/EXP445_PROTOCOL.md sections 6 and 10).
# reader <x445> <omega_emit> <arm> <content file> <out prefix> <requirement>
#   writes <out>.payload (first fenced block), <out>.fence, <out>.candidate.osc (text) or
#   <out>.candidate.oscir (bytes, structured) when the arm's reader makes one, and <out>.check (one
#   line: OK or the refusal). The check uses public data only.
reader() {
  local X="$1" EMIT="$2" arm="$3" content="$4" o="$5" req="$6" line=""
  "$X" extract "$content" "$o.payload" > "$o.fence"
  rm -f "$o.candidate.osc" "$o.candidate.oscir"
  case "$arm" in
    text) cp "$o.payload" "$o.candidate.osc"; line="$("$X" check osc "$o.candidate.osc" "$req" || true)" ;;
    bytes) if line="$("$X" hex "$o.payload" "$o.candidate.oscir")"; then line="$("$X" check oscir "$o.candidate.oscir" "$req" || true)"; fi ;;
    structured) if line="$("$EMIT" emit "$o.payload" "$o.candidate.oscir" 2>&1)"; then line="$("$X" check oscir "$o.candidate.oscir" "$req" || true)"
                else rm -f "$o.candidate.oscir"; fi ;;
    omegese) if line="$("$G" emit "$o.payload" "$o.omegese.json" 2>&1)" && line="$("$EMIT" emit "$o.omegese.json" "$o.candidate.oscir" 2>&1)"; then
             line="$("$X" check oscir "$o.candidate.oscir" "$req" || true)"
           else rm -f "$o.candidate.oscir"; fi ;;
    *) line="REFUSED checker: unknown arm $arm" ;;
  esac
  [ -n "$line" ] || line="REFUSED checker: no result"
  printf '%s\n' "$line" | head -n 1 > "$o.check"
}

# ---- requests, rounds and rows: shared by run.sh (to make them) and score.sh (to re-derive and check them)
# user_msg <requirement> <prompt file>: the user turn (signature lines + the public task prompt)
user_msg() { printf 'Write the function described below.\n\n%s\n\n%s\n' "$(grep -E '^(entry|params|ret)( |$)' "$1")" "$(cat "$2")"; }
# first_msgs <system text> <user text>
first_msgs() { jq -n -c --arg s "$1" --arg u "$2" '[{role: "system", content: $s}, {role: "user", content: $u}]'; }
# repair_msgs <round-1 messages> <round-1 answer> <round-1 check line>: the one repair format of every arm
repair_msgs() {
  local fix="Your program was refused: ${3#REFUSED }"$'\n'"Reply with the corrected complete program in one fenced code block."
  jq -c --arg a "$2" --arg f "$fix" '. + [{role: "assistant", content: $a}, {role: "user", content: $f}]' <<< "$1"
}
# request_json <model> <messages> <max_tokens> <temperature> <top_p> <seed> <config extra>
request_json() {
  jq -n -c --arg m "$1" --argjson msgs "$2" --argjson mt "$3" --argjson t "$4" --argjson p "$5" --argjson s "$6" --argjson x "$7" \
    '{model: $m, messages: $msgs, max_tokens: $mt, temperature: $t, top_p: $p, seed: $s, stream: false} + $x'
}
# seed_of <seed_base> <attempt> <task index, 1-based in sorted task order>: one seed per task and attempt
seed_of() { echo $(($1 + 1000 * $2 + $3)); }
resp_ok() { jq -e '.choices[0].message.content | type == "string"' "$1" > /dev/null 2>&1; }
content_of() { jq -r '.choices[0].message.content' "$1"; }
# usage_of <response>: prompt tokens, completion tokens, finish reason (tab-separated). The response is
# untrusted: a token count that is not a non-negative integer (or is missing) prints BAD, a finish reason
# outside [A-Za-z_]{1,32} prints BAD; score.sh refuses a run with a BAD token count.
usage_of() {
  jq -r 'def n: if type == "number" and . >= 0 and . == floor and . < 100000000 then floor | tostring else "BAD" end;
    [(.usage.prompt_tokens | n), (.usage.completion_tokens | n),
     (.choices[0].finish_reason | if type == "string" and test("^[A-Za-z_]{1,32}$") then . else "BAD" end)] | @tsv' "$1"
}
# A round <p> = <gen dir>/rN is committed when <p>.transport exists: one line per try,
#   try=K http=CODE curl_exit=E time_s=T body=rN.resp.json|rN.err.K.json|none msg=<curl message>
# and it succeeded when its last try's body is rN.resp.json.
round_ok() { [ -f "$1.transport" ] && [ "$(tail -n 1 "$1.transport" | sed -n 's/.* body=\([^ ]*\).*/\1/p')" = "${1##*/}.resp.json" ]; }
wall_of() { local f; for f in "$1"/r*.transport; do if [ -f "$f" ]; then cat "$f"; fi; done | sed -n 's/.* time_s=\([^ ]*\).*/\1/p' | awk '{ s += $1 } END { printf "%.6f", s }'; }
# gen_row <set> <task> <raw gen dir> <derived dir>: sets ROW (the log.tsv row) and FINAL (the final round or
# empty) from the raw responses and transport records in <raw gen dir> and the reader's rN.check /
# rN.candidate.* in <derived dir>. Tokens, finish reasons and wall time come from the raw files only.
gen_row() {
  local d="$3" v="$4" rounds=0 pt=0 p2=0 c1=0 c2=- f1=- f2=- k1=- k2=- cs=- ext
  FINAL=""
  if round_ok "$d/r1"; then
    rounds=1; IFS=$'\t' read -r pt c1 f1 < <(usage_of "$d/r1.resp.json"); k1="$(< "$v/r1.check")"
    if [ "$k1" = OK ]; then FINAL=1
    elif round_ok "$d/r2"; then
      rounds=2; IFS=$'\t' read -r p2 c2 f2 < <(usage_of "$d/r2.resp.json"); if [[ "$pt" =~ ^[0-9]+$ && "$p2" =~ ^[0-9]+$ ]]; then pt=$((pt + p2)); else pt=BAD; fi; k2="$(< "$v/r2.check")"; FINAL=2
    else k2="REQUEST FAILED"; fi
  else k1="REQUEST FAILED"; fi
  if [ -n "$FINAL" ]; then
    for ext in osc oscir; do if [ -f "$v/r$FINAL.candidate.$ext" ]; then cs="$(sha256sum < "$v/r$FINAL.candidate.$ext" | cut -d' ' -f1)"; fi; done
  fi
  ROW="$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' "$1" "$2" "$rounds" "$pt" "$c1" "$c2" "$f1" "$f2" "$(wall_of "$d")" \
    "${k1//$'\t'/ }" "${k2//$'\t'/ }" "$cs")"
}
# gen_sums <gen dir> / run_sums <run dir>: the hashed raw record (requests, responses, failed bodies,
# transport records; and per run also the prompts, the G0 record g0/, meta.txt, log.tsv, weights.check, server.json)
gen_sums() { (cd "$1" && for f in r*.req.json r*.resp.json r*.err.*.json r*.transport; do if [ -f "$f" ]; then sha256sum "$f"; fi; done) > "$1/SHA256SUMS"; }
run_files() { (cd "$1" && { find gen prompts -type f \( -name '*.json' -o -name '*.transport' -o -name '*.system.txt' \); if [ -d g0 ]; then find g0 -type f; fi; printf '%s\n' meta.txt log.tsv weights.check server.json; } | LC_ALL=C sort); }
run_sums() { (cd "$1" && run_files . | tr '\n' '\0' | xargs -0 sha256sum) > "$1/SHA256SUMS"; }
# weights_ok <run dir> <hash list> <model id>: weights.check is exactly "<file>: OK" for every file of the
# config's frozen hash list, and server.json (GET /models) lists the model id
weights_ok() {
  [ -f "$1/weights.check" ] && [ -f "$1/server.json" ] && [ -s "$2" ] || return 1
  [ "$(awk 'NF { n = $2; sub(/^\*/, "", n); print n ": OK" }' "$2" | LC_ALL=C sort)" = "$(LC_ALL=C sort "$1/weights.check")" ] || return 1
  jq -e --arg m "$3" '[.data[]?.id] | index($m) != null' "$1/server.json" > /dev/null 2>&1
}

# ---- protocol r3 (section 13): prompts, G0, held-out and the Stage 1 stop rule ----------------------
# system_prompt <arms dir> <arm> <mode>: the system message of an r3 arm (13.2): arms/common.md, the arm
# document, then the worked examples of the prompt mode (many: examples/many.list, one: examples/one.list).
# Each example is the task as the user message gives it (signature lines, then the prompt) followed by the
# program in the arm's own form (text: program.osc, omegese: program.omegese); the task text is the same in
# both arms.
system_prompt() {
  local A="$1" arm="$2" mode="$3" ext list e n=0 k
  case "$arm" in omegese) ext=omegese ;; text) ext=osc ;; *) echo "system_prompt: arm $arm has no r3 prompt" >&2; return 1 ;; esac
  case "$mode" in many|one) list="$A/examples/$mode.list" ;; *) echo "system_prompt: unknown prompt mode $mode" >&2; return 1 ;; esac
  [ -s "$list" ] || { echo "system_prompt: no example list $list" >&2; return 1; }
  k="$(grep -c . "$list")"
  cat "$A/common.md"; echo; cat "$A/$arm.md"; echo
  if [ "$k" = 1 ]; then echo "Worked example: a task as you receive it, then a correct program."
  else echo "Worked examples ($k): each is a task as you receive it, then a correct program."; fi
  while IFS= read -r e; do
    [ -n "$e" ] || continue
    [ -f "$A/examples/$e/program.$ext" ] || { echo "system_prompt: example $e has no program.$ext" >&2; return 1; }
    n=$((n + 1))
    printf '\nExample %d.\n\n%s\n\n%s\n\n```\n%s\n```\n' "$n" "$(grep -E '^(entry|params|ret)( |$)' "$A/examples/$e/requirement")" \
      "$(cat "$A/examples/$e/prompt")" "$(cat "$A/examples/$e/program.$ext")"
  done < "$list"
}
# TOOL_PATHS: the files a run's results depend on. meta.txt records the commit (plus -dirty when any of these
# differ from it); score.sh requires them unchanged between the generation commit and the scoring commit.
TOOL_PATHS=(tools tests/one_layer docs/one_layer src/compiler src/sha256.c mk/omega-emit.mk mk/omegese.mk Makefile)
# meta_resume <repo root> <run's meta.txt> <meta.txt this invocation would write>: may this run resume?
# Yes when the two are identical, or when only commit= differs, neither side is -dirty and TOOL_PATHS are the
# same at both commits: a commit that adds evidence only (the G0 ledger, the Stage 1 interim scores) lands
# between attempt 1 and the --futility resume, and must not strand the run. Prints the reason when not.
meta_resume() {
  local root="$1" a="$2" b="$3" ca cb
  cmp -s "$a" "$b" && return 0
  if ! diff -q <(grep -v '^commit=' "$a") <(grep -v '^commit=' "$b") > /dev/null; then
    echo "settings differ: $(diff <(grep -v '^commit=' "$a") <(grep -v '^commit=' "$b") | grep '^[<>]' | head -n 4 | tr '\n' ' ')"; return 1
  fi
  ca="$(sed -n 's/^commit=//p' "$a")" cb="$(sed -n 's/^commit=//p' "$b")"
  local h40='^[0-9a-f]{40}(-dirty)?$'
  [[ $ca =~ $h40 && $cb =~ $h40 ]] || { echo "a commit is not a full commit id: ${ca:-none} -> ${cb:-none}"; return 1; }
  git -C "$root" cat-file -e "${ca%-dirty}^{commit}" 2> /dev/null || { echo "the run's commit ${ca%-dirty} is not in this repository"; return 1; }
  git -C "$root" diff --quiet "${ca%-dirty}" "${cb%-dirty}" -- "${TOOL_PATHS[@]}" 2> /dev/null ||
    { echo "tool files differ between the run's commit ${ca%-dirty} and ${cb%-dirty}"; return 1; }
  case "$ca $cb" in *-dirty*) echo "commit changed (${ca} -> ${cb}) and a tree is dirty: the tools cannot be compared"; return 1 ;; esac
}
# heldout <task id>...: true when any task id is not a dev task (the held-out pack; protocol section 3)
heldout() { local t; for t in "$@"; do case "$t" in dev*) ;; *) return 0 ;; esac; done; return 1; }
# gate_config <omegese attempt-1 successes> <text attempt-1 successes> <tasks>: protocol 13.6 for one config,
# omegese at least 80% and at most 10 points below text (integer arithmetic, boundaries included)
gate_config() { [ "$3" -gt 0 ] && [ $((100 * $1)) -ge $((80 * $3)) ] && [ $((100 * ($2 - $1))) -le $((10 * $3)) ]; }
# a1_rows_sha <log.tsv>: the attempt-1 rows of a run, sorted, hashed (ties the Stage 1 stop decision to them)
a1_rows_sha() { tail -n +2 "$1" | awk -F'\t' '$1 ~ /-1$/' | LC_ALL=C sort | sha256sum | cut -d' ' -f1; }
# G0, the sampling gate (13.4). <run dir>/g0/ holds g0.sh and sampling-gate.sh (the scripts that made it),
# prompt.system.txt, rK.req.json for K = 1 .. g0_seeds + 1 (the last repeats the seed of the first), gate/
# (the stage S gate's output: gate.txt and every reply; d-rK.json is the reply to rK.req.json), the
# repeat's rK.transport and rK.resp.json, verdict.txt and SHA256SUMS.
# G0 ledger (13.4: on FAIL nothing in that configuration runs). g0.sh writes every verdict as one new file in
# the ledger directory evidence/ONE-LAYER-2/g0-ledger/ (<UTC time>-<config>-<first 16 hex of the record's
# SHA256SUMS sha256>.tsv, one line), BEFORE it moves the record into place, so a crash leaves a ledger entry
# without a record (the safe side). One file per verdict, never an edited file: evidence is append-only
# (.github/workflows/evidence-immutable.yml refuses a change to an existing evidence file), so a later PR can
# add a verdict and no PR can drop a FAIL. Commit new entries before score.sh, whose harness refuses a dirty
# tree. Columns: UTC time, config, server, the configuration identity (g0_ident), PASS or FAIL, the record's
# directory (relative to the repo when inside it), the sha256 of its SHA256SUMS, the PARAMS sha256.
# A FAIL is keyed on the identity, not on the whole PARAMS file, so an edit elsewhere in PARAMS (a comment,
# the futility rule, the commit that opens a stage) does not lift it. g0.sh, run.sh and score.sh refuse a
# configuration with a FAIL for its identity; run.sh and score.sh also need the ledger's PASS line for the run's
# G0 record (g0_on_ledger). EXP445_G0_LEDGER names another ledger directory only when EXP445_PARAMS is set
# (tests only). Limit: the ledger is in this tree, so a FAIL recorded in another worktree is seen once merged.
g0_ledger() { if [ -n "${EXP445_PARAMS:-}" ] && [ -n "${EXP445_G0_LEDGER:-}" ]; then echo "$EXP445_G0_LEDGER"; else echo "$ROOT/evidence/ONE-LAYER-2/g0-ledger"; fi; }
# g0_lines: every ledger entry, oldest first (nothing when the directory is missing or empty)
g0_lines() { local l; l="$(g0_ledger)"; if [ -d "$l" ]; then find "$l" -maxdepth 1 -type f -name '*.tsv' -print0 | LC_ALL=C sort -z | xargs -0 -r cat; fi; }
# g0_append <line>: one new ledger file for one verdict; refuses to overwrite an entry
g0_append() {
  local l f; l="$(g0_ledger)"; mkdir -p "$l" || return 1
  f="$l/$(cut -f1 <<< "$1")-$(cut -f2 <<< "$1")-$(cut -f7 <<< "$1" | cut -c1-16).tsv"
  [ ! -e "$f" ] || { echo "G0 ledger entry $f exists already" >&2; return 1; }
  printf '%s\n' "$1" > "$f.tmp" && mv -n "$f.tmp" "$f" && [ ! -e "$f.tmp" ]
}
# g0_ident <PARAMS file> <config>: sha256 over the values that define what G0 tested: the configuration's
# model, endpoint, request extra and server, the sampling settings, the G0 task, seeds, prompt mode and gate
g0_ident() {
  local k
  for k in "config.$2.model" "config.$2.base_url" "config.$2.extra" "config.$2.server" max_tokens temperature top_p seed_base \
           g0_task g0_seeds g0_gate_sha256 heldout_prompt_mode; do
    printf '%s=%s\n' "$k" "$(K="$k" awk -F= '$1 == ENVIRON["K"] { print substr($0, length(ENVIRON["K"]) + 2); exit }' "$1")"
  done | sha256sum | cut -d' ' -f1
}
# g0_failed <config> <identity>: prints the first FAIL line for them (nothing when none)
g0_failed() { g0_lines | C="$1" I="$2" awk -F'\t' '!f && $2 == ENVIRON["C"] && $4 == ENVIRON["I"] && $5 == "FAIL" { print; f = 1 }'; }  # reads all input: no SIGPIPE under pipefail
# g0_on_ledger <g0 dir> <config> <identity>: 0 when the ledger holds a PASS line for this configuration and
# identity whose SHA256SUMS sha256 is the record's; prints the reason otherwise
g0_on_ledger() {
  local l s; l="$(g0_ledger)"; s="$(sha256sum < "$1/SHA256SUMS" 2> /dev/null | cut -d' ' -f1)"
  [ -d "$l" ] || { echo "no G0 ledger $l"; return 1; }
  [ -n "$(g0_lines | C="$2" I="$3" S="$s" awk -F'\t' '!f && $2 == ENVIRON["C"] && $4 == ENVIRON["I"] && $5 == "PASS" && $7 == ENVIRON["S"] { print; f = 1 }')" ] ||
    { echo "the G0 record in $1 has no PASS line in $l for this configuration"; return 1; }
}
# g0_files <g0 dir>: the hashed files (gate/ included), sorted
g0_files() { (cd "$1" && find . -type f ! -path ./SHA256SUMS -printf '%P\n' | LC_ALL=C sort); }
# g0_seed <seed_base> <k> <task index>: the seed of G0 request k (request g0_seeds + 1 uses k = 1)
g0_seed() { seed_of "$1" "$2" "$3"; }
# g0_reply <g0 dir> <k> <seeds>: the reply file of request k (the gate's for k <= seeds, the repeat's after)
g0_reply() { if [ "$2" -le "$3" ]; then echo "$1/gate/d-r$2.json"; elif round_ok "$1/r$2"; then echo "$1/r$2.resp.json"; else echo "$1/none"; fi; }
# g0_gate_audit <gate dir> <seeds>: checks the stage S gate's record against its own kept replies, so its
# VERDICT line cannot be edited alone (g0_check also needs gate_exit=0): its SHA256SUMS verifies; gate.txt reads
# "gate a 4/4 PASS", "gate b 4/4 PASS", "gate c 4/4 PASS", "gate d PASS ..." and "VERDICT PASS"; and the
# kept replies agree: (a) each replay pair identical, (b) each spread triple sampled with its own seeds and
# distinct or p_repeat^2 >= 0.01, (c) each greedy pair identical, (d) every repro reply sampled with its own
# seed and the replies distinct or min p_repeat^(seeds-1) >= 0.01 (the gate's own rules, sampling-gate.sh).
# Prints PASS or FAIL.
g0_gate_audit() {
  local g="$1" n="$2" i s h u p
  dg() { jq -j '.choices[0].message.content' "$g/$1.json" 2> /dev/null | sha256sum | cut -d' ' -f1; }
  fd() { jq -r ".usage.aien_sampling.$2 // empty" "$g/$1.json" 2> /dev/null; }
  ge() { awk -v x="$1" -v y="$2" 'BEGIN { exit !(x != "" && x >= y) }'; }
  local ok=1
  (cd "$g" && sha256sum -c --quiet --strict SHA256SUMS > /dev/null 2>&1) || ok=0
  for i in a b c; do grep -qx "gate $i 4/4 PASS" "$g/gate.txt" 2> /dev/null || ok=0; done
  grep -q "^gate d PASS " "$g/gate.txt" 2> /dev/null || ok=0
  [ "$(tail -n 1 "$g/gate.txt" 2> /dev/null)" = "VERDICT PASS" ] || ok=0
  [ "$ok" = 1 ] || { echo FAIL; return; }
  for i in 1 2 3 4; do
    [ -f "$g/a-p$i-r1.json" ] && [ "$(dg "a-p$i-r1")" = "$(dg "a-p$i-r2")" ] || { echo FAIL; return; }
    [ -f "$g/c-p$i-s1.json" ] && [ "$(dg "c-p$i-s1")" = "$(dg "c-p$i-s2")" ] || { echo FAIL; return; }
    for s in 1 2 3; do [ "$(fd "b-p$i-s$s" mode)" = sampled ] && [ "$(fd "b-p$i-s$s" seed)" = "$s" ] || { echo FAIL; return; }; done
    u="$(for s in 1 2 3; do dg "b-p$i-s$s"; done | sort -u | wc -l)" p="$(fd "b-p$i-s1" p_repeat)"
    [ "$u" -ge 2 ] || ge "$(awk -v p="$p" 'BEGIN { printf "%.6g", p * p }')" 0.01 || { echo FAIL; return; }
  done
  p=1
  for i in $(seq 1 "$n"); do
    [ "$(fd "d-r$i" mode)" = sampled ] && [ "$(fd "d-r$i" seed)" = "$(jq -r .seed "$g/../r$i.req.json" 2> /dev/null)" ] || { echo FAIL; return; }
    h="$(fd "d-r$i" p_repeat)"; [ -n "$h" ] || { echo FAIL; return; }; ge "$h" "$p" || p="$h"
  done
  u="$(for i in $(seq 1 "$n"); do dg "d-r$i"; done | sort -u | wc -l)"
  if [ "$u" -ge 2 ] || ge "$(awk -v p="$p" -v k="$n" 'BEGIN { printf "%.6g", p ^ (k - 1) }')" 0.01; then echo PASS; else echo FAIL; fi
}
# g0_derive <g0 dir> <seeds>: "gate=PASS|FAIL distinct=D/N repeat_identical=yes|no sampled_own_seed=S/N+1",
# from the raw files: the gate's verdict line, the replies' exact content and their sampling records
g0_derive() {
  local d="$1" n="$2" k f s=0 u rep=no g=FAIL a b
  [ "$(g0_gate_audit "$d/gate" "$n")" = PASS ] && g=PASS
  for k in $(seq 1 $((n + 1))); do f="$(g0_reply "$d" "$k" "$n")"
    if [ -f "$f" ] && jq -e --argjson s "$(jq '.seed' "$d/r$k.req.json")" \
         '.usage.aien_sampling.mode == "sampled" and .usage.aien_sampling.seed == $s and (.choices[0].message.content | type == "string")' "$f" > /dev/null 2>&1; then
      s=$((s + 1)); fi; done
  u="$(for k in $(seq 1 "$n"); do f="$(g0_reply "$d" "$k" "$n")"; if [ -f "$f" ]; then jq -j '.choices[0].message.content' "$f" | sha256sum | cut -d' ' -f1; fi; done | LC_ALL=C sort -u | grep -c . || true)"
  a="$(g0_reply "$d" 1 "$n")" b="$(g0_reply "$d" $((n + 1)) "$n")"
  if [ -f "$a" ] && [ -f "$b" ] && [ "$(jq -j '.choices[0].message.content' "$a" | sha256sum)" = "$(jq -j '.choices[0].message.content' "$b" | sha256sum)" ]; then rep=yes; fi
  echo "gate=$g distinct=$u/$n repeat_identical=$rep sampled_own_seed=$s/$((n + 1))"
}
# g0_pass <derived> <seeds>: the G0 rule on a derived line (distinct answers are recorded, not gated)
g0_pass() { [[ "$1" == "gate=PASS "* && "$1" == *" repeat_identical=yes sampled_own_seed=$(($2 + 1))/$(($2 + 1))" ]]; }
# g0_check <g0 dir> <want: config model base_url extra server params_sha256 gate_sha256> <system prompt file>
#   <user msg> <max_tokens> <temperature> <top_p> <seed_base> <task index> <seeds>
# returns 0 when G0 PASSED for exactly this configuration and server: SHA256SUMS verifies and lists every
# file, the record's gate script is the frozen one, every request is the frozen G0 request, the verdict's
# identity lines match, and the verdict re-derived from the raw files is PASS. Prints the reason otherwise.
g0_check() {
  local d="$1" cfg="$2" model="$3" base="$4" extra="$5" server="$6" psha="$7" gsha="$8" sysf="$9" user="${10}"
  local mt="${11}" tt="${12}" tp="${13}" sb="${14}" ti="${15}" n="${16}" k s msgs v der
  v="$d/verdict.txt"
  [ -n "$gsha" ] && [ -n "$psha" ] || { echo "no frozen gate or PARAMS sha256 to check G0 against"; return 1; }
  [ -f "$v" ] && [ -f "$d/SHA256SUMS" ] || { echo "no G0 record in $d (run tools/exp445/g0.sh first)"; return 1; }
  (cd "$d" && sha256sum -c --quiet --strict SHA256SUMS > /dev/null 2>&1) || { echo "G0 SHA256SUMS does not verify"; return 1; }
  [ "$(g0_files "$d")" = "$(awk '{ print substr($0, 67) }' "$d/SHA256SUMS" | LC_ALL=C sort)" ] || { echo "G0 SHA256SUMS does not list exactly the G0 files"; return 1; }
  gv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$v"; }
  for kv_ in "config=$cfg" "model=$model" "base_url=$base" "request_extra=$extra" "server=$server" "params_sha256=$psha" "gate_sha256=$gsha"; do
    [ "$(gv "${kv_%%=*}")" = "${kv_#*=}" ] || { echo "G0 was made for another ${kv_%%=*} ($(gv "${kv_%%=*}"))"; return 1; }
  done
  [ "$(sha256sum < "$d/sampling-gate.sh" 2> /dev/null | cut -d' ' -f1)" = "$gsha" ] || { echo "G0 was made by another sampling gate script"; return 1; }
  cmp -s "$d/prompt.system.txt" "$sysf" || { echo "G0 prompt is not the current many-shot omegese prompt"; return 1; }
  msgs="$(first_msgs "$(cat "$sysf")" "$user")"
  for k in $(seq 1 $((n + 1))); do
    s="$(g0_seed "$sb" "$([ "$k" -le "$n" ] && echo "$k" || echo 1)" "$ti")"
    [ -f "$d/r$k.req.json" ] && jq -e --argjson b "$(request_json "$model" "$msgs" "$mt" "$tt" "$tp" "$s" "$extra")" '. == $b' "$d/r$k.req.json" > /dev/null 2>&1 ||
      { echo "G0 request $k is not the frozen G0 request"; return 1; }
  done
  der="$(g0_derive "$d" "$n")"
  [ "$(gv derived)" = "$der" ] || { echo "G0 verdict ($(gv derived)) differs from its raw files ($der)"; return 1; }
  if g0_pass "$der" "$n"; then
    [ "$(gv gate_exit)" = 0 ] || { echo "G0 verdict records gate exit $(gv gate_exit), not 0"; return 1; }
    [ "$(tail -n 1 "$v")" = "G0 PASS" ] || { echo "G0 verdict line is not G0 PASS"; return 1; }
    return 0
  fi
  echo "G0 FAILED ($der; PASS needs gate=PASS, the repeat identical and every reply sampled with its own seed)"; return 1
}

# ---- transport, shared by run.sh and g0.sh (globals BASE, TIMEOUT, RETRIES and die() come from the caller)
# Before try K (K >= 1) of a round the driver pauses K x 5 s (EXP445_TEST_BACKOFF_S replaces the 5; tests
# set 0), so a short server hiccup does not use up every try at once; timing only, nothing recorded depends on it.
# ask <gen dir> <round> <request json>: one round, committed to disk as soon as it is received.
# Writes rN.req.json, then per try a transport line (rN.transport.tmp) and the body (rN.resp.json on
# success, rN.err.K.json otherwise); renaming rN.transport.tmp to rN.transport commits the round. A
# committed round is never requested again; an interrupted one continues with its next try.
ask() {
  local d="$1" r="$2" q="$3" p="$1/r$2" k e out code t t0 body lb msg
  if [ -f "$p.req.json" ]; then [ "$(cat "$p.req.json")" = "$q" ] || die "$p.req.json differs from the request this run sends: resume refused"
  else printf '%s\n' "$q" > "$p.req.json.tmp" || die "cannot write $p.req.json.tmp"; mv "$p.req.json.tmp" "$p.req.json" || die "cannot commit $p.req.json"; fi
  if [ ! -f "$p.transport" ]; then
    touch "$p.transport.tmp" || die "cannot write $p.transport.tmp"
    lb="$(tail -n 1 "$p.transport.tmp" | sed -n 's/.* body=\([^ ]*\).*/\1/p')"
    if [ -n "$lb" ] && [ "$lb" != none ] && [ ! -f "$d/$lb" ] && [ -f "$p.try" ]; then mv "$p.try" "$d/$lb" || die "cannot keep $d/$lb"; fi
    rm -f "$p.try"
    if [ "$lb" != "r$r.resp.json" ]; then
      k="$(wc -l < "$p.transport.tmp")"
      while [ "$k" -le "$RETRIES" ]; do
        [ "$k" -eq 0 ] || sleep $((k * ${EXP445_TEST_BACKOFF_S:-5}))
        t0="$EPOCHREALTIME"
        if out="$(curl -sS -m "$TIMEOUT" -o "$p.try" -w '%{http_code} %{time_total}' -H 'Content-Type: application/json' \
                   --data-binary @"$p.req.json" "$BASE/chat/completions" 2> "$p.curl.err")"; then e=0; else e=$?; fi
        code="${out%% *}" t="${out#* }"
        [[ "$code" =~ ^[0-9]{3}$ ]] || code=000
        [[ "$t" =~ ^[0-9]+(\.[0-9]+)?$ ]] || t="$(awk -v a="$t0" -v b="$EPOCHREALTIME" 'BEGIN { printf "%.6f", b - a }')"
        if [ "$e" = 0 ] && [ "$code" = 200 ] && resp_ok "$p.try"; then body="r$r.resp.json"
        elif [ -s "$p.try" ]; then body="r$r.err.$k.json"; else body=none; rm -f "$p.try"; fi
        msg="$(tr '\n\t' '  ' < "$p.curl.err")"; rm -f "$p.curl.err"
        echo "try=$k http=$code curl_exit=$e time_s=$t body=$body msg=$msg" >> "$p.transport.tmp" || die "cannot write $p.transport.tmp"
        [ "$body" = none ] || mv "$p.try" "$d/$body" || die "cannot keep $d/$body"
        [ "$body" = "r$r.resp.json" ] && break
        k=$((k + 1))
      done
    fi
    mv "$p.transport.tmp" "$p.transport" || die "cannot commit $p.transport"
  fi
  round_ok "$p"
}
