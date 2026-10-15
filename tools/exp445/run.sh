#!/usr/bin/env bash
# tools/exp445/run.sh -- generation driver of the machine-first representation experiment (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md, revision r3 = section 13). Shell + curl + jq + the repo's C tools; no Python.
#
#   run.sh --config A --tasks <public tasks dir> --out <run dir> [--prompt-mode many|one]
#          [--futility <score dir>/futility.txt] [--base-url URL] [--model NAME] [--arms omegese,text]
#          [--only id,id] [--attempts N] [--max-tokens N] [--temperature T] [--top-p P] [--seed-base S]
#
# <public tasks dir> holds tasks/cases/<id>/{requirement,prompt} (ola.sh tasks-public output); the
# driver never sees hidden data. Defaults come from tools/exp445/PARAMS (frozen by the protocol; EXP445_PARAMS
# names another file, for tests only), including the configuration's model, endpoint, request extra and
# server; any override is recorded in meta.txt as protocol=OVERRIDDEN. --prompt-mode picks one of PARAMS
# prompt_modes (default heldout_prompt_mode) and is recorded in meta.txt as prompt_mode; a held-out run
# must use heldout_prompt_mode. Refused: a PARAMS that is not revision r3, a configuration marked
# unavailable (config.<X>.unavailable, with its reason), an arm other than omegese and text (r3 drops bytes
# and structured), a held-out run while the configuration's server is still a PLACEHOLDER.
# Order (13.4): attempt, then task, then arm. For every generation:
#   1 system prompt = reader.sh system_prompt (arms/common.md + the arm document + the worked examples of
#     the prompt mode); user prompt = signature + task prompt
#   2 POST <base-url>/chat/completions (max_tokens = the per-round cap), raw request and response saved
#   3 payload = first fenced block; the arm's own reader turns it into a candidate and checks it with
#     public data only (text: OSC compiler; omegese: tools/omegese then omega_emit,
#     docs/one_layer/OMEGESE.md), then entry/params/ret and the back end (x445 check)
#   4 if refused: exactly one repair round with the same message format in every arm; the last
#     round's answer is final
#   5 candidate copied to cands/<set_id>/<task>/candidate.osc|oscir, set_id = <config>-<arm>-<attempt>
# Before generating, <out> must hold weights.check and server.json from check_weights.sh (all OK) and a G0
# PASS (g0.sh) for this configuration, server and PARAMS, with its PASS line on the G0 ledger; a G0 FAIL on the
# ledger for this configuration's identity (reader.sh g0_ident) refuses the run, checked again before each attempt.
# Stage 1 stop rule (13.4): on a held-out run, after attempt 1 the driver stops unless --futility names the
# futility.txt that `score.sh --interim` wrote for this run's attempt-1 rows with decision=CONTINUE; with
# decision=STOP it runs nothing more.
# Each round is committed as soon as it is received (rN.req.json, rN.transport with HTTP status, curl
# exit and time per try, rN.resp.json or rN.err.K.json); log.tsv rows are derived from those raw
# files (reader.sh gen_row). Writes <out>/meta.txt, prompts/, gen/<set_id>/<task>/, cands/, log.tsv and
# SHA256SUMS. Re-running with the same --out resumes: finished generations are kept, a committed round
# is never requested again, and a row already in log.tsv is not appended twice.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "run.sh: $*" >&2; exit 2; }
PARAMS="${EXP445_PARAMS:-$HERE/PARAMS}"
[ -f "$PARAMS" ] || die "no PARAMS file $PARAMS"
kv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$PARAMS"; }
command -v jq > /dev/null || die "jq is required"
command -v curl > /dev/null || die "curl is required"
. "$HERE/reader.sh"
[ "$(kv revision)" = r3 ] || die "$PARAMS is not protocol revision r3: r1/r2 generation is retired (protocol section 13)"

BASE="" MODEL="" CONFIG="" TASKS="" OUT="" ARMS="$(kv arms)" ONLY="" over="" MODE="$(kv heldout_prompt_mode)" FUT=""
ATTEMPTS="$(kv attempts)" MAXT="$(kv max_tokens)" TEMP="$(kv temperature)" TOPP="$(kv top_p)" SEED="$(kv seed_base)"
TIMEOUT="$(kv timeout_s)" RETRIES="$(kv retries)"
while [ $# -gt 0 ]; do
  case "$1" in
    --base-url) BASE="${2%/}"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --tasks) TASKS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --prompt-mode) MODE="$2"; shift 2 ;;
    --futility) FUT="$2"; shift 2 ;;
    --arms) ARMS="$2"; over="$over arms"; shift 2 ;;
    --only) ONLY="$2"; over="$over only"; shift 2 ;;
    --attempts) ATTEMPTS="$2"; over="$over attempts"; shift 2 ;;
    --max-tokens) MAXT="$2"; over="$over max_tokens"; shift 2 ;;
    --temperature) TEMP="$2"; over="$over temperature"; shift 2 ;;
    --top-p) TOPP="$2"; over="$over top_p"; shift 2 ;;
    --seed-base) SEED="$2"; over="$over seed_base"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$CONFIG" ] && [ -n "$TASKS" ] && [ -n "$OUT" ] ||
  die "usage: run.sh --config A --tasks DIR --out DIR [--prompt-mode many|one] [--futility FILE] [options]"
[[ "$CONFIG" =~ ^[A-Za-z0-9]{1,8}$ ]] || die "config must be [A-Za-z0-9]{1,8}"
UNAV="$(kv "config.$CONFIG.unavailable")"
[ -z "$UNAV" ] || die "config $CONFIG is unavailable: $UNAV"
# The configuration must be named in PARAMS (frozen by the protocol, sections 9 and 13.3): model id,
# endpoint, request extra and server come from there. A different --base-url or --model is recorded as an
# override, which score.sh treats as an invalid run.
FMODEL="$(kv "config.$CONFIG.model")" FBASE="$(kv "config.$CONFIG.base_url")" EXTRA="$(kv "config.$CONFIG.extra")" SERVER="$(kv "config.$CONFIG.server")"
[ -n "$FMODEL" ] && [ -n "$FBASE" ] && [ -n "$EXTRA" ] && [ -n "$SERVER" ] || die "config $CONFIG is not named in $PARAMS (model, base_url, extra, server; protocol 13.3): refused"
jq -e 'type == "object" and ([keys[] | select(IN("model", "messages", "max_tokens", "temperature", "top_p", "seed", "stream"))] | length == 0)' \
  <<< "$EXTRA" > /dev/null 2>&1 || die "config.$CONFIG.extra must be a JSON object that sets none of the fixed request fields"
EXTRA="$(jq -c . <<< "$EXTRA")"
if [ -z "$BASE" ]; then BASE="$FBASE"; elif [ "$BASE" != "$FBASE" ]; then over="$over base_url"; fi
if [ -z "$MODEL" ]; then MODEL="$FMODEL"; elif [ "$MODEL" != "$FMODEL" ]; then over="$over model"; fi
for arm in ${ARMS//,/ }; do
  case "$arm" in omegese|text) ;; bytes|structured) die "arm $arm is dropped by protocol r3 (section 13.1): refused" ;; *) die "unknown arm $arm" ;; esac
done
[[ ",$(kv prompt_modes)," == *",$MODE,"* ]] || die "prompt mode '$MODE' is not one of PARAMS prompt_modes ($(kv prompt_modes))"
WLIST="$(kv "config.$CONFIG.weights")"; case "$WLIST" in /*|"") ;; *) WLIST="$ROOT/$WLIST" ;; esac
[[ "$MAXT" =~ ^[0-9]+$ ]] && [ "$MAXT" -gt 0 ] || die "max_tokens must be a positive integer (PARAMS or --max-tokens)"
[[ "$ATTEMPTS" =~ ^[1-9]$ ]] || die "attempts 1..9"
[[ "$RETRIES" =~ ^[0-9]$ ]] || die "retries must be 0..9 (PARAMS)"
[[ "$TIMEOUT" =~ ^[1-9][0-9]*$ ]] || die "timeout_s must be a positive integer (PARAMS)"
[[ "${EXP445_TEST_BACKOFF_S:-5}" =~ ^[0-9]+$ ]] || die "EXP445_TEST_BACKOFF_S must be an integer (the retry pause, reader.sh ask)"
cfg="$(echo "$CONFIG" | tr 'A-Z' 'a-z')"
[ -d "$TASKS/tasks/cases" ] || die "$TASKS/tasks/cases missing (use ola.sh tasks-public)"
[ -z "$(find "$TASKS/tasks/cases" -path "*/hidden*" -print -quit)" ] || die "$TASKS contains hidden files: the driver must see only requirement + prompt"
ids=(); while IFS= read -r t; do ids+=("$t"); done < <(ls "$TASKS/tasks/cases" | LC_ALL=C sort)
HELD=0; if heldout "${ids[@]}"; then HELD=1; fi
if [ "$HELD" = 1 ]; then
  case "$SERVER" in PLACEHOLDER*) die "config $CONFIG server is still a placeholder in PARAMS: held-out generation waits for the commit that freezes it (protocol 13.3)" ;; esac
  [ "$MODE" = "$(kv heldout_prompt_mode)" ] || die "a held-out run uses the $(kv heldout_prompt_mode) prompt mode (protocol 13.4), not $MODE"
fi
B="$ROOT/build/omega_emit"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit >&2
case ",$ARMS," in *,omegese,*) make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omegese >&2 ;; esac
X="$B/x445" EMIT="$B/omega_emit" G="$ROOT/build/omegese/omegese"
weights_ok "$OUT" "$WLIST" "$FMODEL" ||
  die "$OUT/weights.check + server.json missing or not all OK: run tools/exp445/check_weights.sh $CONFIG $OUT first (protocol section 9)"
# G0 (13.4): a PASS for this configuration, server and PARAMS, re-derived from its raw responses
DEVC="$ROOT/tests/one_layer/exp445/dev/tasks/cases" G0T="$(kv g0_task)"
W0="$(mktemp -d "${TMPDIR:-/tmp}/exp445-run.XXXXXXXX")"; trap 'rm -rf "$W0"' EXIT
system_prompt "$HERE/arms" omegese "$(kv heldout_prompt_mode)" > "$W0/g0.system" || die "cannot build the omegese prompt"
[[ "$SERVER" != *$'\t'* ]] || die "config.$CONFIG.server must not contain a tab (it is a G0 ledger column)"
IDENT="$(g0_ident "$PARAMS" "$CONFIG")"
g0_barred() { # dies when the G0 ledger holds a FAIL for this configuration's identity (checked at start and per attempt)
  local fl; fl="$(g0_failed "$CONFIG" "$IDENT")"
  [ -z "$fl" ] || die "a G0 FAIL for config $CONFIG on this server and settings is in $(g0_ledger) ($(cut -f1,5,6 <<< "$fl" | tr "\t" " ")): nothing in that configuration runs (protocol 13.4)"
}
g0_barred
why="$(g0_check "$OUT/g0" "$CONFIG" "$FMODEL" "$FBASE" "$EXTRA" "$SERVER" "$(sha256sum "$PARAMS" | cut -d" " -f1)" "$(kv g0_gate_sha256)" "$W0/g0.system" \
  "$(user_msg "$DEVC/$G0T/requirement" "$DEVC/$G0T/prompt")" "$(kv max_tokens)" "$(kv temperature)" "$(kv top_p)" "$(kv seed_base)" \
  "$(ls "$DEVC" | LC_ALL=C sort | awk -v t="$G0T" '$0 == t { print NR }')" "$(kv g0_seeds)")" && why="$(g0_on_ledger "$OUT/g0" "$CONFIG" "$IDENT")" ||
  die "no G0 PASS for config $CONFIG and this server in $OUT/g0: $why (run tools/exp445/g0.sh --config $CONFIG --out $OUT; protocol 13.4)"
mkdir -p "$OUT/prompts" "$OUT/gen" "$OUT/cands"
OUT="$(realpath -e "$OUT")"
# one driver per run dir: a second run.sh on the same --out would interleave tries and log rows
exec 9> "$OUT/.lock"; flock -n 9 || die "another run.sh holds $OUT (lock $OUT/.lock)"

# ---- system prompts, one per arm ----------------------------------------------------------------------
for arm in ${ARMS//,/ }; do
  system_prompt "$HERE/arms" "$arm" "$MODE" > "$OUT/prompts/$arm.system.tmp" || die "cannot build the $arm $MODE prompt"
  if [ -f "$OUT/prompts/$arm.system.txt" ]; then
    cmp -s "$OUT/prompts/$arm.system.tmp" "$OUT/prompts/$arm.system.txt" || die "arm $arm prompt changed since this run started"
    rm "$OUT/prompts/$arm.system.tmp"
  else mv "$OUT/prompts/$arm.system.tmp" "$OUT/prompts/$arm.system.txt"; fi
done
if [ -n "$ONLY" ]; then sel=(); for t in "${ids[@]}"; do [[ ",$ONLY," == *",$t,"* ]] && sel+=("$t"); done; ids=("${sel[@]}"); fi
[ "${#ids[@]}" -gt 0 ] || die "no tasks selected"
meta="$OUT/meta.txt"
{
  echo "revision=r3"; echo "base_url=$BASE"; echo "model=$MODEL"; echo "config=$CONFIG"; echo "server=$SERVER"; echo "request_extra=$EXTRA"
  echo "arms=$ARMS"; echo "prompt_mode=$MODE"; echo "attempts=$ATTEMPTS"
  echo "max_tokens_per_round=$MAXT"; echo "repair_rounds=1"; echo "temperature=$TEMP"; echo "top_p=$TOPP"; echo "seed_base=$SEED"
  echo "timeout_s=$TIMEOUT"; echo "retries=$RETRIES"; echo "order=attempt,task,arm"
  echo "protocol=$([ -z "$over" ] && echo PARAMS || echo "OVERRIDDEN:$over")"
  echo "commit=$(git -C "$ROOT" rev-parse HEAD)$(st="$(git -C "$ROOT" status --porcelain -- tools tests/one_layer docs/one_layer src/compiler src/sha256.c mk/omega-emit.mk mk/omegese.mk Makefile)" && [ -z "$st" ] || echo -dirty)"
  echo "params_sha256=$(sha256sum "$PARAMS" | cut -d" " -f1)"; echo "g0_sha256=$(sha256sum < "$OUT/g0/SHA256SUMS" | cut -d" " -f1)"
  for arm in ${ARMS//,/ }; do echo "prompt_${arm}_sha256=$(sha256sum "$OUT/prompts/$arm.system.txt" | cut -d' ' -f1)"; done
  echo "tasks=${#ids[@]}"
  echo "tasks_public_sha256=$(cd "$TASKS" && find tasks -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
} > "$meta.new"
if [ -f "$meta" ]; then cmp -s "$meta" "$meta.new" || { diff "$meta" "$meta.new" >&2 || true; die "resuming with different settings"; }; rm "$meta.new"
else mv "$meta.new" "$meta"; fi
[ -f "$OUT/log.tsv" ] || printf 'set\ttask\trounds\tprompt_tokens\tcompletion_tokens_r1\tcompletion_tokens_r2\tfinish_r1\tfinish_r2\twall_s\tcheck_r1\tcheck_r2\tcandidate_sha256\n' > "$OUT/log.tsv"
dupk="$(tail -n +2 "$OUT/log.tsv" | cut -f1,2 | LC_ALL=C sort | uniq -d | head -n 1 | tr '\t' ' ')"
[ -z "$dupk" ] || die "log.tsv holds generation $dupk more than once: refused (gen/ is the record; remove the extra row by hand)"

# hook <point>: tests only (EXP445_TEST_KILL=<point>:<set>:<task>): die like a power cut at that point
hook() { if [ "${EXP445_TEST_KILL:-}" = "$1:$set_id:$task" ]; then kill -KILL $$; fi; }
# task index = 1-based position in the sorted task list (protocol section 7), fixed before --only
declare -A TI; i=0; while IFS= read -r t; do i=$((i + 1)); TI[$t]=$i; done < <(ls "$TASKS/tasks/cases" | LC_ALL=C sort)

# stop_gate: the Stage 1 stop rule between attempt 1 and attempt 2 of a held-out run (protocol 13.4)
stop_gate() {
  local line d sha
  sha="$(a1_rows_sha "$OUT/log.tsv")"
  if [ -z "$FUT" ]; then
    run_sums "$OUT"
    echo "run.sh: attempt 1 finished; Stage 1 stop rule (protocol 13.4): score attempt 1 with tools/exp445/score.sh --interim, then rerun with --futility <score dir>/futility.txt" >&2
    exit 0
  fi
  [ -f "$FUT" ] || die "no futility file $FUT"
  line="$(awk -v r="run=$OUT config=" "index(\$0, r) == 1" "$FUT")"
  [ -n "$line" ] || die "$FUT has no decision for run $OUT"
  [[ " $line " == *" a1_rows_sha256=$sha "* ]] || die "$FUT was made from other attempt-1 rows than this run's log.tsv: refused"
  d="$(sed -n 's/.* decision=\([A-Z]*\).*/\1/p' <<< "$line")"
  case "$d" in
    CONTINUE) echo "run.sh: stop rule: $line" >&2 ;;
    STOP) run_sums "$OUT"; echo "run.sh: stop rule fired, attempts 2 and later are not run: $line" >&2; exit 0 ;;
    *) die "$FUT: no decision for run $OUT" ;;
  esac
}

for at in $(seq 1 "$ATTEMPTS"); do
  g0_barred   # a G0 FAIL recorded while this run is under way stops it before the next attempt
  if [ "$at" = 2 ] && [ "$HELD" = 1 ]; then stop_gate; fi
  for task in "${ids[@]}"; do
    req="$TASKS/tasks/cases/$task/requirement"
    user="$(user_msg "$req" "$TASKS/tasks/cases/$task/prompt")"
    for arm in ${ARMS//,/ }; do
      msgs="$(first_msgs "$(cat "$OUT/prompts/$arm.system.txt")" "$user")"
      set_id="$cfg-$arm-$at"; d="$OUT/gen/$set_id/$task"
      [ -f "$d/done" ] && continue
      mkdir -p "$d" "$OUT/cands/$set_id"
      seed="$(seed_of "$SEED" "$at" "${TI[$task]}")"
      if ask "$d" 1 "$(request_json "$MODEL" "$msgs" "$MAXT" "$TEMP" "$TOPP" "$seed" "$EXTRA")"; then
        a1="$(content_of "$d/r1.resp.json")"; printf '%s' "$a1" > "$d/r1.content"
        reader "$X" "$EMIT" "$arm" "$d/r1.content" "$d/r1" "$req"
        hook after-r1
        k1="$(cat "$d/r1.check")"
        if [ "$k1" != OK ] && ask "$d" 2 "$(request_json "$MODEL" "$(repair_msgs "$msgs" "$a1" "$k1")" "$MAXT" "$TEMP" "$TOPP" "$seed" "$EXTRA")"; then
          printf '%s' "$(content_of "$d/r2.resp.json")" > "$d/r2.content"
          reader "$X" "$EMIT" "$arm" "$d/r2.content" "$d/r2" "$req"
        fi
      fi
      # finalize, idempotent: candidate, then the row once, then the done marker
      gen_row "$set_id" "$task" "$d" "$d"
      rm -rf "$OUT/cands/$set_id/$task"
      for ext in osc oscir; do
        if [ -n "$FINAL" ] && [ -f "$d/r$FINAL.candidate.$ext" ]; then
          mkdir -p "$OUT/cands/$set_id/$task"; cp "$d/r$FINAL.candidate.$ext" "$OUT/cands/$set_id/$task/candidate.$ext"
        fi
      done
      have="$(awk -F'\t' -v s="$set_id" -v t="$task" '$1 == s && $2 == t' "$OUT/log.tsv")"
      if [ -z "$have" ]; then printf '%s\n' "$ROW" >> "$OUT/log.tsv"
      elif [ "$have" != "$ROW" ]; then die "log.tsv already holds a different row for $set_id $task: refused (gen/ is the record; remove that row by hand)"; fi
      hook after-log
      gen_sums "$d"
      touch "$d/done"
      echo "run.sh: $(cut -f1-3,5,6,10,11 <<< "$ROW" | tr '\t' ' ')" >&2
    done
  done
done
run_sums "$OUT"
echo "run.sh: done, $(($(wc -l < "$OUT/log.tsv") - 1)) generations in $OUT"
