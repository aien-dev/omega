#!/usr/bin/env bash
# tools/exp445/run.sh -- generation driver of the machine-first representation experiment (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md). Shell + curl + jq + the repo's C tools; no Python.
#
#   run.sh --config A|B --tasks <public tasks dir> --out <run dir> [--base-url URL] [--model NAME]
#          [--arms text,bytes,structured] [--only id,id] [--attempts N] [--max-tokens N]
#          [--temperature T] [--top-p P] [--seed-base S]
#
# <public tasks dir> holds tasks/cases/<id>/{requirement,prompt} (ola.sh tasks-public output); the
# driver never sees hidden data. Defaults come from tools/exp445/PARAMS (frozen by the protocol; EXP445_PARAMS
# names another file, for tests only), including the configuration's model, endpoint and request extra; any
# override is recorded in meta.txt as protocol=OVERRIDDEN. For every task x arm x attempt:
#   1 system prompt = arms/common.md + the arm document; user prompt = signature + task prompt
#   2 POST <base-url>/chat/completions (max_tokens = the per-round cap), raw request and response saved
#   3 payload = first fenced block; the arm's own reader turns it into a candidate and checks it with
#     public data only (text: OSC compiler; bytes: strict hex + OSC IR decoder; structured: omega_emit),
#     then entry/params/ret and the back end (x445 check)
#   4 if refused: exactly one repair round with the same message format in every arm; the last
#     round's answer is final
#   5 candidate copied to cands/<set_id>/<task>/candidate.osc|oscir, set_id = <config>-<arm>-<attempt>
# Before generating, <out> must hold weights.check and server.json from check_weights.sh (all OK).
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

BASE="" MODEL="" CONFIG="" TASKS="" OUT="" ARMS="$(kv arms)" ONLY="" over=""
ATTEMPTS="$(kv attempts)" MAXT="$(kv max_tokens)" TEMP="$(kv temperature)" TOPP="$(kv top_p)" SEED="$(kv seed_base)"
TIMEOUT="$(kv timeout_s)" RETRIES="$(kv retries)"
while [ $# -gt 0 ]; do
  case "$1" in
    --base-url) BASE="${2%/}"; shift 2 ;;
    --model) MODEL="$2"; shift 2 ;;
    --config) CONFIG="$2"; shift 2 ;;
    --tasks) TASKS="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
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
  die "usage: run.sh --config A|B --tasks DIR --out DIR [--base-url URL] [--model NAME] [options]"
[[ "$CONFIG" =~ ^[A-Za-z0-9]{1,8}$ ]] || die "config must be [A-Za-z0-9]{1,8}"
# The configuration must be named in PARAMS (frozen by the protocol, section 9): model id, endpoint and
# request extra come from there. A different --base-url or --model is recorded as an override, which
# score.sh treats as an invalid run.
FMODEL="$(kv "config.$CONFIG.model")" FBASE="$(kv "config.$CONFIG.base_url")" EXTRA="$(kv "config.$CONFIG.extra")"
[ -n "$FMODEL" ] && [ -n "$FBASE" ] && [ -n "$EXTRA" ] || die "config $CONFIG is not named in $PARAMS (protocol section 9): refused"
jq -e 'type == "object" and ([keys[] | select(IN("model", "messages", "max_tokens", "temperature", "top_p", "seed", "stream"))] | length == 0)' \
  <<< "$EXTRA" > /dev/null 2>&1 || die "config.$CONFIG.extra must be a JSON object that sets none of the fixed request fields"
EXTRA="$(jq -c . <<< "$EXTRA")"
if [ -z "$BASE" ]; then BASE="$FBASE"; elif [ "$BASE" != "$FBASE" ]; then over="$over base_url"; fi
if [ -z "$MODEL" ]; then MODEL="$FMODEL"; elif [ "$MODEL" != "$FMODEL" ]; then over="$over model"; fi
WLIST="$(kv "config.$CONFIG.weights")"; case "$WLIST" in /*|"") ;; *) WLIST="$ROOT/$WLIST" ;; esac
[[ "$MAXT" =~ ^[0-9]+$ ]] && [ "$MAXT" -gt 0 ] || die "max_tokens must be a positive integer (PARAMS or --max-tokens)"
[[ "$ATTEMPTS" =~ ^[1-9]$ ]] || die "attempts 1..9"
cfg="$(echo "$CONFIG" | tr 'A-Z' 'a-z')"
[ -d "$TASKS/tasks/cases" ] || die "$TASKS/tasks/cases missing (use ola.sh tasks-public)"
find "$TASKS/tasks/cases" -path '*/hidden*' | grep -q . && die "$TASKS contains hidden files: the driver must see only requirement + prompt"
B="$ROOT/build/omega_emit"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit >&2
X="$B/x445" EMIT="$B/omega_emit"
. "$HERE/reader.sh"
weights_ok "$OUT" "$WLIST" "$FMODEL" ||
  die "$OUT/weights.check + server.json missing or not all OK: run tools/exp445/check_weights.sh $CONFIG $OUT first (protocol section 9)"
mkdir -p "$OUT/prompts" "$OUT/gen" "$OUT/cands"
OUT="$(realpath -e "$OUT")"

# ---- system prompts, one per arm ----------------------------------------------------------------------
for arm in ${ARMS//,/ }; do
  case "$arm" in
    text|bytes) doc="$HERE/arms/$arm.md" ;;
    structured) doc="$HERE/arms/structured.md" ;;
    *) die "unknown arm $arm" ;;
  esac
  { cat "$HERE/arms/common.md"; echo; cat "$doc"; [ "$arm" = structured ] && cat "$ROOT/tools/omega_emit/SCHEMA.md"; true; } > "$OUT/prompts/$arm.system.tmp"
  if [ -f "$OUT/prompts/$arm.system.txt" ]; then
    cmp -s "$OUT/prompts/$arm.system.tmp" "$OUT/prompts/$arm.system.txt" || die "arm $arm prompt changed since this run started"
    rm "$OUT/prompts/$arm.system.tmp"
  else mv "$OUT/prompts/$arm.system.tmp" "$OUT/prompts/$arm.system.txt"; fi
done
ids=(); while IFS= read -r t; do ids+=("$t"); done < <(ls "$TASKS/tasks/cases" | LC_ALL=C sort)
if [ -n "$ONLY" ]; then sel=(); for t in "${ids[@]}"; do [[ ",$ONLY," == *",$t,"* ]] && sel+=("$t"); done; ids=("${sel[@]}"); fi
[ "${#ids[@]}" -gt 0 ] || die "no tasks selected"
meta="$OUT/meta.txt"
{
  echo "base_url=$BASE"; echo "model=$MODEL"; echo "config=$CONFIG"; echo "request_extra=$EXTRA"; echo "arms=$ARMS"; echo "attempts=$ATTEMPTS"
  echo "max_tokens_per_round=$MAXT"; echo "repair_rounds=1"; echo "temperature=$TEMP"; echo "top_p=$TOPP"; echo "seed_base=$SEED"
  echo "timeout_s=$TIMEOUT"; echo "retries=$RETRIES"
  echo "protocol=$([ -z "$over" ] && echo PARAMS || echo "OVERRIDDEN:$over")"
  echo "commit=$(git -C "$ROOT" rev-parse HEAD)$(git -C "$ROOT" diff --quiet HEAD -- tools tests/one_layer docs/one_layer src/compiler src/sha256.c mk/omega-emit.mk Makefile || echo -dirty)"
  echo "params_sha256=$(sha256sum "$PARAMS" | cut -d' ' -f1)"
  for arm in ${ARMS//,/ }; do echo "prompt_${arm}_sha256=$(sha256sum "$OUT/prompts/$arm.system.txt" | cut -d' ' -f1)"; done
  echo "tasks=${#ids[@]}"
  echo "tasks_public_sha256=$(cd "$TASKS" && find tasks -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
} > "$meta.new"
if [ -f "$meta" ]; then cmp -s "$meta" "$meta.new" || { diff "$meta" "$meta.new" >&2 || true; die "resuming with different settings"; }; rm "$meta.new"
else mv "$meta.new" "$meta"; fi
[ -f "$OUT/log.tsv" ] || printf 'set\ttask\trounds\tprompt_tokens\tcompletion_tokens_r1\tcompletion_tokens_r2\tfinish_r1\tfinish_r2\twall_s\tcheck_r1\tcheck_r2\tcandidate_sha256\n' > "$OUT/log.tsv"
dupk="$(tail -n +2 "$OUT/log.tsv" | cut -f1,2 | LC_ALL=C sort | uniq -d | head -n 1 | tr '\t' ' ')"
[ -z "$dupk" ] || die "log.tsv holds generation $dupk more than once: refused (gen/ is the record; remove the extra row by hand)"

# ask <gen dir> <round> <request json>: one round, committed to disk as soon as it is received.
# Writes rN.req.json, then per try a transport line (rN.transport.tmp) and the body (rN.resp.json on
# success, rN.err.K.json otherwise); renaming rN.transport.tmp to rN.transport commits the round. A
# committed round is never requested again; an interrupted one continues with its next try.
ask() {
  local d="$1" r="$2" q="$3" p="$1/r$2" k e out code t t0 body lb msg
  if [ -f "$p.req.json" ]; then [ "$(cat "$p.req.json")" = "$q" ] || die "$p.req.json differs from the request this run sends: resume refused"
  else printf '%s\n' "$q" > "$p.req.json.tmp"; mv "$p.req.json.tmp" "$p.req.json"; fi
  if [ ! -f "$p.transport" ]; then
    touch "$p.transport.tmp"
    lb="$(tail -n 1 "$p.transport.tmp" | sed -n 's/.* body=\([^ ]*\).*/\1/p')"
    if [ -n "$lb" ] && [ "$lb" != none ] && [ ! -f "$d/$lb" ] && [ -f "$p.try" ]; then mv "$p.try" "$d/$lb"; fi
    rm -f "$p.try"
    if [ "$lb" != "r$r.resp.json" ]; then
      k="$(wc -l < "$p.transport.tmp")"
      while [ "$k" -le "$RETRIES" ]; do
        t0="$EPOCHREALTIME"
        if out="$(curl -sS -m "$TIMEOUT" -o "$p.try" -w '%{http_code} %{time_total}' -H 'Content-Type: application/json' \
                   --data-binary @"$p.req.json" "$BASE/chat/completions" 2> "$p.curl.err")"; then e=0; else e=$?; fi
        code="${out%% *}" t="${out#* }"
        [[ "$code" =~ ^[0-9]{3}$ ]] || code=000
        [[ "$t" =~ ^[0-9]+(\.[0-9]+)?$ ]] || t="$(awk -v a="$t0" -v b="$EPOCHREALTIME" 'BEGIN { printf "%.6f", b - a }')"
        if [ "$e" = 0 ] && [ "$code" = 200 ] && resp_ok "$p.try"; then body="r$r.resp.json"
        elif [ -s "$p.try" ]; then body="r$r.err.$k.json"; else body=none; rm -f "$p.try"; fi
        msg="$(tr '\n\t' '  ' < "$p.curl.err")"; rm -f "$p.curl.err"
        echo "try=$k http=$code curl_exit=$e time_s=$t body=$body msg=$msg" >> "$p.transport.tmp"
        [ "$body" = none ] || mv "$p.try" "$d/$body"
        [ "$body" = "r$r.resp.json" ] && break
        k=$((k + 1))
      done
    fi
    mv "$p.transport.tmp" "$p.transport"
  fi
  round_ok "$p"
}
# hook <point>: tests only (EXP445_TEST_KILL=<point>:<set>:<task>): die like a power cut at that point
hook() { if [ "${EXP445_TEST_KILL:-}" = "$1:$set_id:$task" ]; then kill -KILL $$; fi; }

ti=0
for task in "${ids[@]}"; do
  ti=$((ti + 1))
  req="$TASKS/tasks/cases/$task/requirement"
  user="$(user_msg "$req" "$TASKS/tasks/cases/$task/prompt")"
  for arm in ${ARMS//,/ }; do
    msgs="$(first_msgs "$(cat "$OUT/prompts/$arm.system.txt")" "$user")"
    for at in $(seq 1 "$ATTEMPTS"); do
      set_id="$cfg-$arm-$at"; d="$OUT/gen/$set_id/$task"
      [ -f "$d/done" ] && continue
      mkdir -p "$d" "$OUT/cands/$set_id"
      seed="$(seed_of "$SEED" "$at" "$ti")"
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
