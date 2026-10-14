#!/usr/bin/env bash
# tools/exp445/run.sh -- generation driver of the machine-first representation experiment (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md). Shell + curl + jq + the repo's C tools; no Python.
#
#   run.sh --base-url URL --model NAME --config A|B --tasks <public tasks dir> --out <run dir>
#          [--arms text,bytes,structured] [--only id,id] [--attempts N] [--max-tokens N]
#          [--temperature T] [--top-p P] [--seed-base S]
#
# <public tasks dir> holds tasks/cases/<id>/{requirement,prompt} (ola.sh tasks-public output); the
# driver never sees hidden data. Defaults come from tools/exp445/PARAMS (frozen by the protocol); any
# override is recorded in meta.txt as protocol=OVERRIDDEN. For every task x arm x attempt:
#   1 system prompt = arms/common.md + the arm document; user prompt = signature + task prompt
#   2 POST <base-url>/chat/completions (max_tokens = the per-round cap), raw request and response saved
#   3 payload = first fenced block; the arm's own reader turns it into a candidate and checks it with
#     public data only (text: OSC compiler; bytes: strict hex + OSC IR decoder; structured: omega_emit),
#     then entry/params/ret and the back end (x445 check)
#   4 if refused: exactly one repair round with the same message format in every arm; the last
#     round's answer is final
#   5 candidate copied to cands/<set_id>/<task>/candidate.osc|oscir, set_id = <config>-<arm>-<attempt>
# Writes <out>/meta.txt, prompts/, gen/<set_id>/<task>/, cands/, log.tsv and SHA256SUMS (sha256 of
# every raw request/response). Re-running with the same --out resumes (finished generations kept).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "run.sh: $*" >&2; exit 2; }
kv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$HERE/PARAMS"; }
command -v jq > /dev/null || die "jq is required"
command -v curl > /dev/null || die "curl is required"

BASE="" MODEL="" CONFIG="" TASKS="" OUT="" ARMS="text,bytes,structured" ONLY="" over=""
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
[ -n "$BASE" ] && [ -n "$MODEL" ] && [ -n "$CONFIG" ] && [ -n "$TASKS" ] && [ -n "$OUT" ] ||
  die "usage: run.sh --base-url URL --model NAME --config A|B --tasks DIR --out DIR [options]"
[[ "$CONFIG" =~ ^[A-Za-z0-9]{1,8}$ ]] || die "config must be [A-Za-z0-9]{1,8}"
[[ "$MAXT" =~ ^[0-9]+$ ]] && [ "$MAXT" -gt 0 ] || die "max_tokens must be a positive integer (PARAMS or --max-tokens)"
[[ "$ATTEMPTS" =~ ^[1-9]$ ]] || die "attempts 1..9"
cfg="$(echo "$CONFIG" | tr 'A-Z' 'a-z')"
[ -d "$TASKS/tasks/cases" ] || die "$TASKS/tasks/cases missing (use ola.sh tasks-public)"
find "$TASKS/tasks/cases" -path '*/hidden*' | grep -q . && die "$TASKS contains hidden files: the driver must see only requirement + prompt"
B="$ROOT/build/omega_emit"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit >&2
X="$B/x445" EMIT="$B/omega_emit"
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
  echo "base_url=$BASE"; echo "model=$MODEL"; echo "config=$CONFIG"; echo "arms=$ARMS"; echo "attempts=$ATTEMPTS"
  echo "max_tokens_per_round=$MAXT"; echo "repair_rounds=1"; echo "temperature=$TEMP"; echo "top_p=$TOPP"; echo "seed_base=$SEED"
  echo "timeout_s=$TIMEOUT"; echo "retries=$RETRIES"
  echo "protocol=$([ -z "$over" ] && echo PARAMS || echo "OVERRIDDEN:$over")"
  echo "commit=$(git -C "$ROOT" rev-parse HEAD)$(git -C "$ROOT" diff --quiet HEAD -- tools tests/one_layer || echo -dirty)"
  echo "params_sha256=$(sha256sum "$HERE/PARAMS" | cut -d' ' -f1)"
  for arm in ${ARMS//,/ }; do echo "prompt_${arm}_sha256=$(sha256sum "$OUT/prompts/$arm.system.txt" | cut -d' ' -f1)"; done
  echo "tasks=${#ids[@]}"
  echo "tasks_public_sha256=$(cd "$TASKS" && find tasks -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
} > "$meta.new"
if [ -f "$meta" ]; then cmp -s "$meta" "$meta.new" || { diff "$meta" "$meta.new" >&2 || true; die "resuming with different settings"; }; rm "$meta.new"
else mv "$meta.new" "$meta"; fi
[ -f "$OUT/log.tsv" ] || printf 'set\ttask\trounds\tprompt_tokens\tcompletion_tokens_r1\tcompletion_tokens_r2\tfinish_r1\tfinish_r2\twall_s\tcheck_r1\tcheck_r2\tcandidate_sha256\n' > "$OUT/log.tsv"

# ask <dir> <round> <messages json>: one request with retries on transport or 5xx errors.
# Sets: content, ptok, ctok, finish, wall. Writes rN.req.json, rN.resp.json (+ rN.err.K on failures).
ask() {
  local d="$1" r="$2" msgs="$3" seed="$4" k code t
  jq -n -c --arg m "$MODEL" --argjson msgs "$msgs" --argjson mt "$MAXT" --argjson t "$TEMP" --argjson p "$TOPP" --argjson s "$seed" \
    '{model: $m, messages: $msgs, max_tokens: $mt, temperature: $t, top_p: $p, seed: $s, stream: false}' > "$d/r$r.req.json"
  wall=0; content=""; ptok=0; ctok=0; finish=error
  for k in $(seq 0 "$RETRIES"); do
    if out="$(curl -sS -m "$TIMEOUT" -o "$d/r$r.resp.json" -w '%{http_code} %{time_total}' -H 'Content-Type: application/json' \
               --data-binary @"$d/r$r.req.json" "$BASE/chat/completions" 2> "$d/r$r.curl.err")"; then :; else out="000 0"; fi
    code="${out% *}"; t="${out#* }"; wall="$(awk -v a="$wall" -v b="$t" 'BEGIN { printf "%.3f", a + b }')"
    if [ "$code" = 200 ] && jq -e '.choices[0].message.content | type == "string"' "$d/r$r.resp.json" > /dev/null 2>&1; then
      content="$(jq -r '.choices[0].message.content' "$d/r$r.resp.json")"
      ptok="$(jq -r '.usage.prompt_tokens // 0' "$d/r$r.resp.json")"; ctok="$(jq -r '.usage.completion_tokens // 0' "$d/r$r.resp.json")"
      finish="$(jq -r '.choices[0].finish_reason // "-"' "$d/r$r.resp.json")"
      rm -f "$d/r$r.curl.err"
      return 0
    fi
    [ -f "$d/r$r.resp.json" ] && mv "$d/r$r.resp.json" "$d/r$r.err.$k.json"
    echo "http=$code attempt=$k" >> "$d/r$r.errors"
  done
  return 1
}

# candidate <arm> <dir> <round> <requirement>: payload -> candidate file + one check line in rN.check
candidate() {
  local arm="$1" d="$2" r="$3" req="$4" line
  "$X" extract "$d/r$r.content" "$d/r$r.payload" > "$d/r$r.fence"
  rm -f "$d/r$r.candidate.osc" "$d/r$r.candidate.oscir"
  case "$arm" in
    text) cp "$d/r$r.payload" "$d/r$r.candidate.osc"; line="$("$X" check osc "$d/r$r.candidate.osc" "$req" || true)" ;;
    bytes) if line="$("$X" hex "$d/r$r.payload" "$d/r$r.candidate.oscir")"; then line="$("$X" check oscir "$d/r$r.candidate.oscir" "$req" || true)"; fi ;;
    structured) if line="$("$EMIT" emit "$d/r$r.payload" "$d/r$r.candidate.oscir" 2>&1)"; then line="$("$X" check oscir "$d/r$r.candidate.oscir" "$req" || true)"
                else rm -f "$d/r$r.candidate.oscir"; fi ;;
  esac
  [ -n "$line" ] || line="REFUSED checker: no result"
  printf '%s\n' "$line" | head -n 1 > "$d/r$r.check"
}

ti=0
for task in "${ids[@]}"; do
  ti=$((ti + 1))
  req="$TASKS/tasks/cases/$task/requirement"
  sig="$(grep -E '^(entry|params|ret)( |$)' "$req")"
  user="$(printf 'Write the function described below.\n\n%s\n\n%s\n' "$sig" "$(cat "$TASKS/tasks/cases/$task/prompt")")"
  for arm in ${ARMS//,/ }; do
    for at in $(seq 1 "$ATTEMPTS"); do
      set_id="$cfg-$arm-$at"; d="$OUT/gen/$set_id/$task"
      [ -f "$d/done" ] && continue
      rm -rf "$d"; mkdir -p "$d" "$OUT/cands/$set_id"
      seed=$((SEED + 1000 * at + ti))
      sys="$(cat "$OUT/prompts/$arm.system.txt")"
      msgs="$(jq -n -c --arg s "$sys" --arg u "$user" '[{role: "system", content: $s}, {role: "user", content: $u}]')"
      rounds=0 ptot=0 c1=0 c2=- f1=- f2=- w=0 k1=- k2=- final=""
      if ask "$d" 1 "$msgs" "$seed"; then
        rounds=1; printf '%s' "$content" > "$d/r1.content"; ptot=$ptok; c1=$ctok; f1=$finish; w=$wall
        candidate "$arm" "$d" 1 "$req"; k1="$(cat "$d/r1.check")"; final=1
        if [ "$k1" != OK ]; then
          fix="Your program was refused: ${k1#REFUSED }"$'\n'"Reply with the corrected complete program in one fenced code block."
          msgs2="$(jq -c --arg a "$content" --arg f "$fix" '. + [{role: "assistant", content: $a}, {role: "user", content: $f}]' <<< "$msgs")"
          if ask "$d" 2 "$msgs2" "$seed"; then
            rounds=2; printf '%s' "$content" > "$d/r2.content"; ptot=$((ptot + ptok)); c2=$ctok; f2=$finish
            w="$(awk -v a="$w" -v b="$wall" 'BEGIN { printf "%.3f", a + b }')"
            candidate "$arm" "$d" 2 "$req"; k2="$(cat "$d/r2.check")"; final=2
          else w="$(awk -v a="$w" -v b="$wall" 'BEGIN { printf "%.3f", a + b }')"; k2="REQUEST FAILED"; final=""; fi
        fi
      else w=$wall; k1="REQUEST FAILED"; fi
      csha=-
      rm -rf "$OUT/cands/$set_id/$task"
      for ext in osc oscir; do
        if [ -n "$final" ] && [ -f "$d/r$final.candidate.$ext" ]; then
          mkdir -p "$OUT/cands/$set_id/$task"; cp "$d/r$final.candidate.$ext" "$OUT/cands/$set_id/$task/candidate.$ext"
          csha="$(sha256sum "$d/r$final.candidate.$ext" | cut -d' ' -f1)"
        fi
      done
      printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$set_id" "$task" "$rounds" "$ptot" "$c1" "$c2" "$f1" "$f2" "$w" \
        "$(tr '\t' ' ' <<< "$k1")" "$(tr '\t' ' ' <<< "$k2")" "$csha" >> "$OUT/log.tsv"
      (cd "$d" && sha256sum r*.req.json r*.resp.json 2>/dev/null) > "$d/SHA256SUMS" || true
      touch "$d/done"
      echo "run.sh: $set_id $task rounds=$rounds tokens=$c1/$c2 check1=$k1 check2=$k2" >&2
    done
  done
done
(cd "$OUT" && find gen prompts -type f \( -name '*.json' -o -name '*.system.txt' \) -print0 | LC_ALL=C sort -z | xargs -0 sha256sum) > "$OUT/SHA256SUMS"
echo "run.sh: done, $(($(wc -l < "$OUT/log.tsv") - 1)) generations in $OUT"
