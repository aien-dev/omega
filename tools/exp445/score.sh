#!/usr/bin/env bash
# tools/exp445/score.sh [--configs A] [--interim] <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...] -- score
# the generated candidates of driver runs (run.sh) and write the results table (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md sections 4, 7, 10 and revision r3, section 13). Scores r3 runs only:
# a run made under r1/r2 (meta.txt without revision=r3) is refused with that reason.
#   1 provenance check, before anything is scored (any failure: RUN INVALID, no tables, exit 1):
#     - PARAMS is revision r3; every run: meta.txt revision=r3, protocol=PARAMS (no override flag),
#       params_sha256 = the PARAMS in force, its sampling settings equal to PARAMS, the configuration named
#       in PARAMS (not unavailable) with its frozen model, endpoint, request extra and server, a prompt mode
#       of PARAMS prompt_modes (the same in every run scored together; a held-out run: heldout_prompt_mode,
#       and a server that is no longer a PLACEHOLDER), the arm prompts equal to the ones the current arm
#       documents and worked examples give for that mode, the public view of these tasks, a clean
#       generation commit whose tools equal the scoring commit's, weights.check all OK for the config's
#       frozen hash list and server.json listing its model id, and a G0 PASS (g0/, reader.sh g0_check)
#       for the config, server and PARAMS, with its PASS line on the G0 ledger and no G0 FAIL there for the
#       config's identity (reader.sh g0_on_ledger, g0_failed)
#     - every run: SHA256SUMS verifies and lists exactly the raw files, prompts, g0/, meta.txt, log.tsv,
#       weights.check and server.json
#     - every generation, from its raw files only: each hashed request equals the one PARAMS, the config,
#       the frozen arm prompt, the public task and the seed rule give; a round 2 exists exactly when the
#       arm's reader refused round 1, and its request is the frozen repair of round 1; every round has a
#       transport record (HTTP status, curl exit, time per try; a failed round shows all its tries); the
#       log row (tokens, finish reasons, wall time, checks, candidate hash) is re-derived from the raw
#       responses (reader.sh gen_row) and log.tsv must equal the re-derived rows; the file in cands/ must
#       be the re-derived candidate
#     - the matrix is complete: every task of the tarball x every arm x attempts 1..N x every config
#       in --configs (default: every available config of PARAMS), exactly once; held-out = heldout_tasks.
#       A held-out config whose run holds attempt 1 only is complete when the Stage 1 stop rule fired
#       (checked in step 3).
#   2 merge the candidates; tests/one_layer/acceptance/ola.sh experiment (seal, references, replay,
#     probes, clean tree): its RUN INVALID also stops here
#   3 results.txt / results.tsv from the re-derived rows: per config x arm, success on attempt 1 (final
#     candidate, and round 1 alone beside it) and on any attempt with Wilson 95% intervals, tokens, wall
#     seconds, distinct final candidates per task across attempts (13.5, distinct.tsv); per config the cost ratio omegese / text, the paired attempt-1 view with the exact two-sided
#     McNemar test, the Stage 1 stop rule, and the gate of protocol 13.6 (evaluated only when every config
#     named in PARAMS is scored; config A alone is labelled interim).
# --interim (Stage 1 stop rule, 13.4): attempt 1 only (later attempts in the runs are ignored), and
#   futility.txt with one line per run: run=<dir> config=<X> omegese_attempt1=<k>/<n> futility_max=<m>
#   a1_rows_sha256=<sha> decision=STOP|CONTINUE, which run.sh --futility reads.
# EXP445_TEST=1 (dry-run tests only): accepts EXP445_PARAMS, a dirty tree (OLA_ALLOW_DIRTY), and a
# harness RUN INVALID caused by the dirty tree alone; the output then says TEST MODE, NOT EVIDENCE.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "score.sh: $*" >&2; exit 2; }
TEST="${EXP445_TEST:-0}"
PARAMS="$HERE/PARAMS"
if [ "$TEST" = 1 ]; then PARAMS="${EXP445_PARAMS:-$PARAMS}"; else unset OLA_ALLOW_DIRTY OLA_EVIDENCE_ROOT EXP445_PARAMS; fi
kv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$PARAMS"; }
[ "$(kv revision)" = r3 ] || die "$PARAMS is not protocol revision r3: r1/r2 scoring is retired (protocol section 13)"
CONFIGS="" INTERIM=0
while [ $# -gt 0 ]; do
  case "$1" in
    --configs) CONFIGS="$2"; shift 2 ;;
    --interim) INTERIM=1; shift ;;
    *) break ;;
  esac
done
[ $# -ge 3 ] || die "usage: score.sh [--configs A] [--interim] <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...]"
TAR="$(realpath -e "$1")"; SEALED="$(realpath -e "$2")"; shift 2
runs=(); for r in "$@"; do runs+=("$(realpath -e "$r")"); done
# named = every config of PARAMS (the gate needs all of them); available = those not marked unavailable
NAMED="$(awk -F= '$1 ~ /^config\.[A-Za-z0-9]+\.model$/ { split($1, p, "."); printf "%s%s", s, p[2]; s = "," }' "$PARAMS")"
AVAIL=""; for c in ${NAMED//,/ }; do [ -n "$(kv "config.$c.unavailable")" ] || AVAIL="$AVAIL${AVAIL:+,}$c"; done
[ -n "$CONFIGS" ] || CONFIGS="$AVAIL"
for c in ${CONFIGS//,/ }; do un="$(kv "config.$c.unavailable")"; [ -z "$un" ] || die "config $c is unavailable: $un"; done
B="$ROOT/build/omega_emit" X="$ROOT/build/omega_emit/x445" EMIT="$ROOT/build/omega_emit/omega_emit" G="$ROOT/build/omegese/omegese"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit omegese >&2
. "$HERE/reader.sh"
OUT="${runs[0]}/../score-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir "$OUT"; OUT="$(realpath -e "$OUT")"
W="$(mktemp -d "${TMPDIR:-/tmp}/exp445-score.XXXXXXXX")"; trap 'rm -rf "$W"' EXIT
echo "score.sh: output in $OUT" >&2

# ---- 1. provenance -------------------------------------------------------------------------------
bad=0
inval() { echo "INVALID: $*" >> "$OUT/validity.txt"; bad=$((bad + 1)); }
: > "$OUT/validity.txt"
"$ROOT/tests/one_layer/acceptance/ola.sh" tasks-public "$TAR" "$SEALED" "$W/pub" > /dev/null 2> "$W/pub.err" || { cat "$W/pub.err" >&2; die "tasks-public refused the tasks"; }
pub_sha="$(cd "$W/pub" && find tasks -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum | cut -d' ' -f1)"
mapfile -t TASKS < <(ls "$W/pub/tasks/cases" | LC_ALL=C sort)
declare -A TI; i=0; for t in "${TASKS[@]}"; do i=$((i + 1)); TI[$t]=$i; done
ARMS="$(kv arms)" ATT="$(kv attempts)" params_sha="$(sha256sum "$PARAMS" | cut -d' ' -f1)"
MT="$(kv max_tokens)" TT="$(kv temperature)" TP="$(kv top_p)" SB="$(kv seed_base)" RT="$(kv retries)"
heldout=0; if heldout "${TASKS[@]}"; then heldout=1; fi
[ "$heldout" = 0 ] || [ "${#TASKS[@]}" = "$(kv heldout_tasks)" ] || inval "held-out tarball has ${#TASKS[@]} tasks, the protocol fixes $(kv heldout_tasks)"
head_commit="$(git -C "$ROOT" rev-parse HEAD)"
# the arm prompts of every prompt mode, and the G0 prompt and task (13.4)
for mode in $(kv prompt_modes | tr ',' ' '); do for arm in ${ARMS//,/ }; do
  system_prompt "$HERE/arms" "$arm" "$mode" > "$W/prompt.$arm.$mode" || die "cannot build the $arm $mode prompt"
done; done
DEVC="$ROOT/tests/one_layer/exp445/dev/tasks/cases" G0T="$(kv g0_task)"
system_prompt "$HERE/arms" omegese "$(kv heldout_prompt_mode)" > "$W/g0.system" || die "cannot build the G0 prompt"
G0U="$(user_msg "$DEVC/$G0T/requirement" "$DEVC/$G0T/prompt")" G0I="$(ls "$DEVC" | LC_ALL=C sort | awk -v t="$G0T" '$0 == t { print NR }')"
MODE=""
# req_same <saved request> <expected json>: equal as JSON; otherwise prints the top-level fields that differ
req_same() {
  [ -f "$1" ] || { echo "(missing)"; return 1; }
  if jq -e --argjson b "$2" '. == $b' "$1" > /dev/null 2>&1; then return 0; fi
  jq -r -n --slurpfile a "$1" --argjson b "$2" '[($a[0] | keys[]), ($b | keys[])] | unique | map(select($a[0][.] != $b[.])) | join(",")' 2> /dev/null || echo "(not JSON)"
  return 1
}
# tcheck <round prefix>: the transport record of a committed round (protocol section 10)
tcheck() {
  local p="$1" b="${1##*/}" n=0 line h e body succ=0 f
  [ -f "$p.transport" ] || { inval "$GL $b has no transport record (never committed)"; return; }
  while IFS= read -r line; do
    [[ "$line" =~ ^try=([0-9]+)\ http=([0-9]{3})\ curl_exit=([0-9]+)\ time_s=[0-9]+(\.[0-9]+)?\ body=([^ ]+)\ msg= ]] ||
      { inval "$GL $b transport line $((n + 1)) malformed"; return; }
    [ "${BASH_REMATCH[1]}" = "$n" ] || { inval "$GL $b transport tries out of order"; return; }
    h="${BASH_REMATCH[2]}" e="${BASH_REMATCH[3]}" body="${BASH_REMATCH[5]}"
    [ "$succ" = 0 ] || { inval "$GL $b has a try after its successful one"; return; }
    case "$body" in
      "$b.resp.json") succ=1
        [ "$h" = 200 ] && [ "$e" = 0 ] && resp_ok "$p.resp.json" || inval "$GL $b response is not an HTTP 200 answer with content" ;;
      "$b.err.$n.json") [ -f "${p%/*}/$body" ] || inval "$GL $b failed body $body missing"
        if [ "$h" = 200 ] && [ "$e" = 0 ] && resp_ok "${p%/*}/$body"; then inval "$GL $b try $n is a valid answer filed as a failure"; fi ;;
      none) if [ "$h" = 200 ] && [ "$e" = 0 ]; then inval "$GL $b try $n: HTTP 200 without a body"; fi ;;
      *) inval "$GL $b try $n names body $body" ;;
    esac
    n=$((n + 1))
  done < "$p.transport"
  [ "$n" -ge 1 ] && [ "$n" -le $((RT + 1)) ] || inval "$GL $b has $n tries, the protocol allows 1..$((RT + 1))"
  [ "$succ" = 1 ] || [ "$n" = $((RT + 1)) ] || inval "$GL failed round $b has $n tries, the protocol makes $((RT + 1)) with transport evidence"
  [ "$succ" = 1 ] || [ ! -e "$p.resp.json" ] || inval "$GL $b has a response file but no successful try"
  for f in "$p".err.*.json; do [ -f "$f" ] || continue; grep -q " body=${f##*/} " "$p.transport" || inval "$GL ${f##*/} is not in the transport record"; done
}
# gcheck <run> <set> <task> <gen dir> <config>: every request against PARAMS and the config, the repair
# chain, the transport records, then the row and the candidate re-derived from the raw responses
gcheck() {
  local r="$1" sid="$2" task="$3" d="$4" cl="$5" v="$W/g" arm at req msgs q a1 k1 r2=no why f
  GL="$r: $sid $task"
  [[ "$sid" =~ ^([a-z0-9]+)-([a-z]+)-([1-9])$ ]] && [ "${BASH_REMATCH[1]}" = "$cl" ] || { inval "$GL: set is not one of config $cl"; return; }
  arm="${BASH_REMATCH[2]}" at="${BASH_REMATCH[3]}"
  [[ ",$ARMS," == *",$arm,"* ]] && [ "$at" -le "$ATT" ] || { inval "$GL: arm or attempt outside PARAMS"; return; }
  req="$W/pub/tasks/cases/$task/requirement"
  [ -f "$req" ] || { inval "$GL is not a task of this tarball"; return; }
  [ -f "$d/done" ] || { inval "$GL never finished"; return; }
  echo "$sid $task" >> "$W/rows"
  rm -rf "$v"; mkdir -p "$v"
  msgs="$(first_msgs "$(cat "$W/prompt.$arm")" "$(user_msg "$req" "$W/pub/tasks/cases/$task/prompt")")"
  q="$(request_json "$FM" "$msgs" "$MT" "$TT" "$TP" "$(seed_of "$SB" "$at" "${TI[$task]}")" "$FX")"
  why="$(req_same "$d/r1.req.json" "$q")" || inval "$GL r1 request differs from the frozen request in: $why"
  tcheck "$d/r1"
  if round_ok "$d/r1"; then
    a1="$(content_of "$d/r1.resp.json")"; printf '%s' "$a1" > "$v/r1.content"
    reader "$X" "$EMIT" "$arm" "$v/r1.content" "$v/r1" "$req"; k1="$(cat "$v/r1.check")"
    if [ "$k1" != OK ]; then
      r2=yes
      q="$(request_json "$FM" "$(repair_msgs "$msgs" "$a1" "$k1")" "$MT" "$TT" "$TP" "$(seed_of "$SB" "$at" "${TI[$task]}")" "$FX")"
      why="$(req_same "$d/r2.req.json" "$q")" || inval "$GL round 1 was refused ($k1); r2 request is not its frozen repair, differs in: $why"
      tcheck "$d/r2"
      if round_ok "$d/r2"; then printf '%s' "$(content_of "$d/r2.resp.json")" > "$v/r2.content"; reader "$X" "$EMIT" "$arm" "$v/r2.content" "$v/r2" "$req"; fi
    fi
  fi
  if [ "$r2" = no ]; then for f in "$d"/r2.req.json "$d"/r2.transport "$d"/r2.resp.json "$d"/r2.err.*.json; do
    if [ -e "$f" ]; then inval "$GL has a round 2 without a round-1 refusal"; break; fi; done; fi
  for f in "$d"/r[0-9]*.req.json "$d"/r[0-9]*.transport "$d"/r[0-9]*.resp.json; do b="${f##*/r}"; if [ -e "$f" ] && [ "${b%%.*}" != 1 ] && [ "${b%%.*}" != 2 ]; then inval "$GL has a round other than 1 and 2"; break; fi; done
  gen_row "$sid" "$task" "$d" "$v"
  # token counts come from the untrusted server response: each must be a non-negative integer (or "-"
  # for an absent round 2); reader.sh usage_of prints BAD otherwise
  awk -F'\t' '{ for (i = 4; i <= 6; i++) if ($i !~ /^[0-9]+$/ && !(i == 6 && $i == "-")) exit 1 }' <<< "$ROW" ||
    inval "$GL token counts in the response are not non-negative integers ($(cut -f4-6 <<< "$ROW" | tr '\t' ' '))"
  printf '%s\n' "$ROW" >> "$W/derived"
  local want got n=0; want="$(cut -f12 <<< "$ROW")" got=-
  for f in "$r/cands/$sid/$task"/*; do [ -f "$f" ] || continue; n=$((n + 1)); got="$(sha256sum < "$f" | cut -d' ' -f1)"; done
  [ "$n" -le 1 ] && [ "$got" = "$want" ] || inval "$GL candidate in cands/ does not match the candidate re-derived from the response"
}
: > "$W/rows"; : > "$W/all_derived"
HDR="$(printf 'set\ttask\trounds\tprompt_tokens\tcompletion_tokens_r1\tcompletion_tokens_r2\tfinish_r1\tfinish_r2\twall_s\tcheck_r1\tcheck_r2\tcandidate_sha256')"
declare -A RUNCFG STOPPED
for r in "${runs[@]}"; do
  m="$r/meta.txt"
  mv_() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$m"; }
  [ -f "$m" ] && [ -f "$r/log.tsv" ] && [ -f "$r/SHA256SUMS" ] && [ -d "$r/cands" ] && [ -d "$r/gen" ] || { inval "$r: not a complete run dir (meta.txt, log.tsv, SHA256SUMS, gen/, cands/)"; continue; }
  if [ "$(mv_ revision)" != r3 ]; then
    inval "$r: not an r3 run (meta.txt has no revision=r3): r1/r2 runs are not scored under protocol r3 (section 13.1)"; continue
  fi
  c="$(mv_ config)"; cl="$(echo "$c" | tr 'A-Z' 'a-z')"; RUNCFG[$r]="$c"
  [[ ",$CONFIGS," == *",$c,"* ]] || inval "$r: config '$c' is not one of the configs scored ($CONFIGS)"
  [ -n "$(kv "config.$c.model")" ] || inval "$r: config '$c' is not named in PARAMS"
  [ -z "$(kv "config.$c.unavailable")" ] || inval "$r: config '$c' is unavailable in PARAMS"
  FM="$(kv "config.$c.model")" FX="$(kv "config.$c.extra" | jq -c . 2>/dev/null || echo "MALFORMED (not JSON)")" FS="$(kv "config.$c.server")"
  [ "$(mv_ model)" = "$FM" ] || inval "$r: model $(mv_ model) is not config $c's frozen model"
  [ "$(mv_ base_url)" = "$(kv "config.$c.base_url")" ] || inval "$r: endpoint $(mv_ base_url) is not config $c's frozen endpoint"
  [ "$(mv_ request_extra)" = "$FX" ] || inval "$r: request extra differs from config $c's frozen extra"
  [ -n "$FS" ] && [ "$(mv_ server)" = "$FS" ] || inval "$r: server '$(mv_ server)' is not config $c's frozen server"
  [ "$(mv_ protocol)" = PARAMS ] || inval "$r: settings overridden on the command line ($(mv_ protocol))"
  [ "$(mv_ params_sha256)" = "$params_sha" ] || inval "$r: PARAMS changed since generation (params_sha256 differs)"
  [ "$(mv_ arms)" = "$ARMS" ] && [ "$(mv_ attempts)" = "$ATT" ] || inval "$r: arms/attempts differ from PARAMS"
  [ "$(mv_ order)" = attempt,task,arm ] || inval "$r: generation order is not attempt,task,arm (protocol 13.4)"
  for kp in max_tokens_per_round:max_tokens temperature:temperature top_p:top_p seed_base:seed_base timeout_s:timeout_s retries:retries; do
    [ "$(mv_ "${kp%%:*}")" = "$(kv "${kp#*:}")" ] || inval "$r: meta.txt ${kp%%:*} differs from PARAMS"; done
  [ "$(mv_ repair_rounds)" = 1 ] || inval "$r: meta.txt repair_rounds is not 1"
  [ "$(mv_ tasks_public_sha256)" = "$pub_sha" ] || inval "$r: generated from a different public task view"
  rm_="$(mv_ prompt_mode)"
  if [[ ",$(kv prompt_modes)," != *",$rm_,"* ]]; then inval "$r: prompt mode '$rm_' is not one of PARAMS prompt_modes"; rm_="$(kv heldout_prompt_mode)"; fi
  if [ -z "$MODE" ]; then MODE="$rm_"; elif [ "$MODE" != "$rm_" ]; then inval "$r: prompt mode $rm_ differs from the other runs' ($MODE): score each prompt mode on its own"; fi
  if [ "$heldout" = 1 ]; then
    [ "$rm_" = "$(kv heldout_prompt_mode)" ] || inval "$r: a held-out run must use the $(kv heldout_prompt_mode) prompt mode (protocol 13.4), not $rm_"
    case "$FS" in PLACEHOLDER*) inval "$r: config $c's server is still a placeholder in PARAMS; held-out runs need the frozen server (protocol 13.3)" ;; esac
  fi
  wl="$(kv "config.$c.weights")"; case "$wl" in /*|"") ;; *) wl="$ROOT/$wl" ;; esac
  weights_ok "$r" "$wl" "$FM" || inval "$r: weights.check / server.json missing or not all OK for config $c (tools/exp445/check_weights.sh)"
  why="$(g0_check "$r/g0" "$c" "$FM" "$(kv "config.$c.base_url")" "$FX" "$FS" "$params_sha" "$(kv g0_gate_sha256)" "$W/g0.system" "$G0U" "$MT" "$TT" "$TP" "$SB" "$G0I" \
    "$(kv g0_seeds)")" && why="$(g0_on_ledger "$r/g0" "$c" "$(g0_ident "$PARAMS" "$c")")" || inval "$r: no G0 PASS for config $c and its server: $why"
  fl="$(g0_failed "$c" "$(g0_ident "$PARAMS" "$c")")"
  [ -z "$fl" ] || inval "$r: a G0 FAIL for config $c on this server and settings is in $(g0_ledger) ($(cut -f1,5,6 <<< "$fl" | tr "\t" " ")): nothing in that configuration counts (protocol 13.4)"
  [ -n "$(mv_ g0_sha256)" ] && [ "$(mv_ g0_sha256)" = "$(sha256sum < "$r/g0/SHA256SUMS" 2> /dev/null | cut -d" " -f1)" ] || inval "$r: the G0 record is not the one this run was generated under (meta.txt g0_sha256)"
  for arm in ${ARMS//,/ }; do
    [ "$(mv_ "prompt_${arm}_sha256")" = "$(sha256sum < "$W/prompt.$arm.$rm_" | cut -d' ' -f1)" ] &&
      cmp -s "$r/prompts/$arm.system.txt" "$W/prompt.$arm.$rm_" || inval "$r: arm $arm prompt differs from the current arm documents and $rm_ examples"
    cp "$W/prompt.$arm.$rm_" "$W/prompt.$arm"
  done
  gc="$(mv_ commit)"
  if [ "$TEST" != 1 ]; then
    case "$gc" in *-dirty) inval "$r: generated from a dirty tree ($gc)" ;; esac
    git -C "$ROOT" diff --quiet "${gc%-dirty}" "$head_commit" -- "${TOOL_PATHS[@]}" 2> /dev/null ||
      inval "$r: tools at generation commit ${gc%-dirty} differ from the scoring commit"
  fi
  (cd "$r" && sha256sum -c --quiet --strict SHA256SUMS > /dev/null 2>&1) || inval "$r: SHA256SUMS does not verify (a raw file, prompt, G0 record, meta.txt or log.tsv changed)"
  run_files "$r" > "$W/have"
  awk '{ print substr($0, 67) }' "$r/SHA256SUMS" | LC_ALL=C sort > "$W/listed"
  cmp -s "$W/have" "$W/listed" || inval "$r: SHA256SUMS does not list exactly the raw files, prompts, g0/, meta.txt, log.tsv, weights.check and server.json"
  [ "$(head -n 1 "$r/log.tsv")" = "$HDR" ] || inval "$r: log.tsv header"
  # every generation, from its raw files; log.tsv must equal the re-derived rows (--interim: attempt 1 only)
  : > "$W/derived"
  for d in "$r"/gen/*/*/; do [ -d "$d" ] || continue; d="${d%/}"; sid="$(basename "$(dirname "$d")")"
    if [ "$INTERIM" = 1 ] && [[ "$sid" != *-1 ]]; then continue; fi
    gcheck "$r" "$sid" "$(basename "$d")" "$d" "$cl"; done
  LC_ALL=C sort "$W/derived" > "$W/derived_s"; tail -n +2 "$r/log.tsv" | awk -F'\t' -v i="$INTERIM" 'i == 0 || $1 ~ /-1$/' | LC_ALL=C sort > "$W/log_s"
  if ! cmp -s "$W/derived_s" "$W/log_s"; then
    inval "$r: log.tsv differs from the rows re-derived from the raw files (first: $(diff "$W/derived_s" "$W/log_s" | sed -n 's/^[<>] //p' | head -n 1 | cut -f1,2 | tr '\t' ' '))"
  fi
  cat "$W/derived" >> "$W/all_derived"
  for s in "$r"/cands/*/*/; do [ -d "$s" ] || continue; sid="$(basename "$(dirname "$s")")"; t="$(basename "$s")"
    [ -f "$r/gen/$sid/$t/done" ] || inval "$r: cands/$sid/$t has no finished generation"; done
done
# matrix. --interim: attempt 1 only. A held-out config with no generation after attempt 1 is scored as
# stopped by the Stage 1 rule (13.4); step 3 checks that the rule really fired.
: > "$W/want"
for c in ${CONFIGS//,/ }; do cl="$(echo "$c" | tr 'A-Z' 'a-z')" na="$ATT"
  if [ "$INTERIM" = 1 ]; then na=1
  elif [ "$heldout" = 1 ] && [ "$ATT" -gt 1 ] && ! grep -q "^$cl-[a-z]*-[2-9] " "$W/rows"; then na=1; STOPPED[$cl]=1; fi
  for arm in ${ARMS//,/ }; do for at in $(seq 1 "$na"); do for t in "${TASKS[@]}"; do echo "$cl-$arm-$at $t" >> "$W/want"; done; done; done
done
dups="$(LC_ALL=C sort "$W/rows" | uniq -d | wc -l)"
[ "$dups" = 0 ] || inval "$dups generations appear more than once"
LC_ALL=C sort -u "$W/rows" > "$W/have_rows"; LC_ALL=C sort "$W/want" > "$W/want_s"
miss="$(comm -13 "$W/have_rows" "$W/want_s" | wc -l)" extra="$(comm -23 "$W/have_rows" "$W/want_s" | wc -l)"
[ "$miss" = 0 ] || inval "$miss of $(wc -l < "$W/want_s") generations missing (first: $(comm -13 "$W/have_rows" "$W/want_s" | head -1))"
[ "$extra" = 0 ] || inval "$extra generations outside the matrix (first: $(comm -23 "$W/have_rows" "$W/want_s" | head -1))"
if [ "$bad" -gt 0 ]; then
  echo "RUN INVALID: $bad provenance problems, nothing scored (see $OUT/validity.txt)" | tee -a "$OUT/validity.txt" >&2
  head -5 "$OUT/validity.txt" >&2
  exit 1
fi
echo "provenance OK: ${#runs[@]} runs, $(wc -l < "$W/want_s") generations, configs $CONFIGS, prompt mode $MODE$([ "$INTERIM" = 1 ] && echo ', interim: attempt 1 only')" >> "$OUT/validity.txt"

# ---- 2. harness ------------------------------------------------------------------------------------
M="$W/cands"; mkdir "$M"; cp "$W/all_derived" "$OUT/log.tsv"   # the rows re-derived from the raw files
for r in "${runs[@]}"; do
  for s in "$r"/cands/*/; do [ -d "$s" ] || continue; sid="$(basename "$s")"
    if [ "$INTERIM" = 1 ] && [[ "$sid" != *-1 ]]; then continue; fi
    cp -R "$s" "$M/$sid"; done
  echo "$r $(sha256sum "$r/SHA256SUMS" | cut -d' ' -f1)" >> "$OUT/runs.txt"
done
while IFS=$'\t' read -r sid _; do mkdir -p "$M/$sid"; done < "$OUT/log.tsv"
"$ROOT/tests/one_layer/acceptance/ola.sh" experiment "$TAR" "$SEALED" "$M" > "$OUT/experiment.out" 2> "$OUT/experiment.err" ||
  { tail -5 "$OUT/experiment.err" >&2; die "ola.sh experiment failed"; }
ev="$(sed -n 's/^ola.sh: evidence in //p' "$OUT/experiment.err" | tail -1)"
case "$ev" in /*) ;; *) ev="$ROOT/$ev" ;; esac
[ -f "$ev/score.txt" ] || die "no harness score.txt"
echo "harness_run=$ev" > "$OUT/meta.txt"
cp "$ev/score.txt" "$OUT/harness_score.txt"
valid="$(tail -1 "$ev/score.txt")" mode=""
if [ "$valid" != "RUN VALID" ]; then
  if [ "$TEST" = 1 ] && grep -q '^references_accepted=\([0-9]*\)/\1$' "$ev/score.txt" && ! grep -q 'errors=[1-9]' "$ev/score.txt" &&
     grep -qx 'replay_identical=yes probes=PASS clean_tree=no' "$ev/score.txt"; then mode="TEST MODE, NOT EVIDENCE (dirty tree): "
  else
    echo "RUN INVALID: the harness run is not valid ($(sed -n 's/^replay_identical/replay_identical/p' "$ev/score.txt"); $(grep '^references_accepted' "$ev/score.txt")); nothing scored" | tee -a "$OUT/validity.txt" >&2
    exit 1
  fi
fi
[ "$TEST" = 1 ] && [ -z "$mode" ] && mode="TEST MODE, NOT EVIDENCE: "

# ---- 3. results ------------------------------------------------------------------------------------
for v in "$ev"/sets/*/verdicts.txt; do s="$(basename "$(dirname "$v")")"; [ "$s" = references ] && continue
  awk -v s="$s" '{ print s "\t" $1 "\t" $2 "\t" $3 }' "$v"; done > "$OUT/verdicts.tsv"
# per config x arm rows: n, attempt-1 accepted, attempt-1 accepted in round 1, tasks, any-attempt accepted,
# generations, accepted, repaired, repaired accepted, completion tokens (sum, mean per generation), prompt
# tokens mean, wall seconds
awk -F'\t' '
  FNR == NR { acc[$1, $2] = ($3 == "ACCEPT"); next }
  { split($1, p, "-"); key = p[1] "\t" p[2]; att = p[3]; a = acc[$1, $2] + 0
    keys[key] = 1; tasks[key, $2] = 1; if (att == 1) { n1[key]++; a1[key] += a; if ($3 == 1) r1[key] += a }
    if (a) any[key, $2] = 1; gen[key]++; gacc[key] += a
    c = $5 + ($6 == "-" ? 0 : $6); tok[key] += c; pt[key] += $4; wall[key] += $9
    if ($3 == 2) { rep[key]++; repok[key] += a } }
  END {
    for (key in keys) { na = 0; for (kt in tasks) { split(kt, q, SUBSEP); if (q[1] == key) { nt[key]++; if (any[key, q[2]]) na++ } }
      printf "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.2f\n", key, n1[key], a1[key], r1[key], nt[key], na, gen[key], gacc[key],
             rep[key] + 0, repok[key] + 0, tok[key], tok[key] / gen[key], pt[key] / gen[key], wall[key] }
  }' "$OUT/verdicts.tsv" "$OUT/log.tsv" | LC_ALL=C sort > "$OUT/rows.tsv"
{
  printf 'config\tarm\ttasks\tsuccess_attempt1\tp1\tp1_lo\tp1_hi\tsuccess_attempt1_round1\tsuccess_any\tpany\tpany_lo\tpany_hi\tgenerations\tgen_accepted\trepaired\trepaired_accepted\tcompletion_tokens_total\tcompletion_tokens_mean\tprompt_tokens_mean\twall_s_total\n'
  while IFS=$'\t' read -r c a n1 a1 r1 nt na g ga rp ro tt tm pm wt; do
    w1="$("$X" wilson "$a1" "$n1" | tr ' ' '\t')"; wa="$("$X" wilson "$na" "$nt" | tr ' ' '\t')"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$a" "$n1" "$a1/$n1" "$w1" "$r1/$n1" "$na/$nt" "$wa" "$g" "$ga" "$rp" "$ro" "$tt" "$tm" "$pm" "$wt"
  done < "$OUT/rows.tsv"
} > "$OUT/results.tsv"
# distinct candidates per task (13.5): per config x arm x task, the attempts run and the distinct
# final-candidate hashes among them ("-", no candidate, counts as one value)
awk -F'\t' '{ split($1, p, "-"); k = p[1] "\t" p[2] "\t" $2; n[k]++; if (!((k, $12) in seen)) { seen[k, $12] = 1; d[k]++ } }
  END { for (k in n) printf "%s\t%d\t%d\n", k, n[k], d[k] }' "$OUT/log.tsv" | LC_ALL=C sort > "$OUT/distinct.tsv"
# paired attempt-1 view per config (13.6): tasks where only omegese succeeds (b) and only text (c), exact
# two-sided McNemar test on (b, c)
: > "$OUT/paired.tsv"
for c in ${CONFIGS//,/ }; do cl="$(echo "$c" | tr 'A-Z' 'a-z')"
  read -r both bo bt none < <(awk -F'\t' -v o="$cl-omegese-1" -v t="$cl-text-1" '
    $1 == o { so[$2] = ($3 == "ACCEPT"); k[$2] = 1 } $1 == t { st[$2] = ($3 == "ACCEPT"); k[$2] = 1 }
    END { for (x in k) { if (so[x] && st[x]) b++; else if (so[x]) bo++; else if (st[x]) bt++; else n++ } printf "%d %d %d %d\n", b, bo, bt, n }' "$OUT/verdicts.tsv")
  printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$cl" "$both" "$bo" "$bt" "$none" "$("$X" mcnemar "$bo" "$bt")" >> "$OUT/paired.tsv"
done
# Stage 1 stop rule (13.4), per config of a held-out run: omegese attempt-1 successes <= futility_max
FMAX="$(kv futility_max)" stopbad=0
declare -A FUTD
while IFS=$'\t' read -r c a n1 a1 _; do
  [ "$a" = omegese ] || continue
  if [ "$a1" -le "$FMAX" ]; then FUTD[$c]=STOP; else FUTD[$c]=CONTINUE; fi
  if [ "$INTERIM" = 0 ] && [ "$heldout" = 1 ] && [ "$ATT" -gt 1 ]; then
    if [ "${STOPPED[$c]:-0}" = 1 ] && [ "${FUTD[$c]}" = CONTINUE ]; then
      echo "INVALID: config $c: attempts 2..$ATT are missing but the stop rule did not fire (omegese attempt 1 $a1/$n1 > $FMAX)" >> "$OUT/validity.txt"; stopbad=1; fi
    if [ "${STOPPED[$c]:-0}" = 0 ] && [ "${FUTD[$c]}" = STOP ]; then
      echo "INVALID: config $c: the stop rule fired (omegese attempt 1 $a1/$n1 <= $FMAX) but attempts 2..$ATT were run" >> "$OUT/validity.txt"; stopbad=1; fi
  fi
done < "$OUT/rows.tsv"
if [ "$stopbad" = 1 ]; then echo "RUN INVALID: the Stage 1 stop rule was not followed (see $OUT/validity.txt)" | tee -a "$OUT/validity.txt" >&2; exit 1; fi
if [ "$INTERIM" = 1 ]; then
  : > "$OUT/futility.txt"
  for r in "${runs[@]}"; do c="${RUNCFG[$r]}"; cl="$(echo "$c" | tr 'A-Z' 'a-z')"
    k="$(awk -F'\t' -v c="$cl" '$1 == c && $2 == "omegese" { print $4 "/" $3 }' "$OUT/rows.tsv")"; [ -n "$k" ] || k="0/0"
    echo "run=$r config=$c omegese_attempt1=$k futility_max=$FMAX a1_rows_sha256=$(a1_rows_sha "$r/log.tsv") decision=${FUTD[$cl]:-STOP}" >> "$OUT/futility.txt"
  done
fi
# gate (13.6): per config on attempt-1 successes; the experiment's gate only when every config named in
# PARAMS is scored (config A alone is interim, 13.3)
allcfg="$(echo "$NAMED" | tr 'A-Z,' 'a-z\n' | LC_ALL=C sort | paste -sd,)"
scored="$(echo "$CONFIGS" | tr 'A-Z,' 'a-z\n' | LC_ALL=C sort | paste -sd,)"
gl="" g=1
for c in $(awk -F'\t' 'NR > 1 { print $1 }' "$OUT/results.tsv" | LC_ALL=C sort -u); do
  o="" n="" t="" nt=""; read -r o n < <(awk -F'\t' -v c="$c" '$1 == c && $2 == "omegese" { split($4, q, "/"); print q[1], q[2] }' "$OUT/results.tsv") || true
  read -r t nt < <(awk -F'\t' -v c="$c" '$1 == c && $2 == "text" { split($4, q, "/"); print q[1], q[2] }' "$OUT/results.tsv") || true
  if gate_config "${o:-0}" "${t:-0}" "${n:-0}"; then p=PASS; else p=FAIL g=0; fi
  gl="${gl}gate config $c: omegese ${o:-0}/${n:-0}, text ${t:-0}/${nt:-0}: $p (needs omegese >= 80% and at most 10 points below text)"$'\n'
done
if [ "$INTERIM" = 1 ]; then gl="${gl}GATE NOT EVALUATED: interim score of attempt 1 (Stage 1 stop rule)"
elif [ "$scored" != "$allcfg" ]; then gl="${gl}GATE NOT EVALUATED: configs scored $scored, the protocol needs $allcfg; INTERIM result (protocol 13.3)"
else gl="${gl}GATE $([ "$g" = 1 ] && echo PASS || echo FAIL) (omegese passes in every config)"; fi
gate="$gl"
{
  echo "${mode}exp445 r3 results$([ "$INTERIM" = 1 ] && echo ', INTERIM: attempt 1 only') (harness: $valid; $(sed -n 's/^references_accepted=/references accepted /p' "$ev/score.txt"); provenance OK; prompt mode $MODE)"
  echo "success = harness ACCEPT on hidden inputs of the final candidate; attempt 1 is the pre-registered primary measure; round 1 = accepted without the repair round; 95% Wilson intervals"
  echo
  awk -F'\t' 'NR == 1 { next }
    { printf "config %-3s arm %-8s attempt1 %-7s %.3f [%.3f, %.3f]   round 1 only %-7s   any-of-attempts %-7s %.3f [%.3f, %.3f]\n", $1, $2, $4, $5, $6, $7, $8, $9, $10, $11, $12
      printf "                        completion tokens %.0f total, %.1f per generation; prompt %.1f per generation; wall %.1f s; repaired %d (accepted %d)\n", $17, $18, $19, $20, $15, $16
      mean[$1, $2] = $18; pm[$1, $2] = $19; cfgs[$1] = 1 }
    END { for (c in cfgs) if (mean[c, "text"] > 0)
      printf "cost config %s: omegese / text mean completion tokens per generation x%.2f (%.1f / %.1f); prompt tokens per generation %.1f / %.1f\n", c, mean[c, "omegese"] / mean[c, "text"], mean[c, "omegese"], mean[c, "text"], pm[c, "omegese"], pm[c, "text"] }' "$OUT/results.tsv"
  awk -F'\t' '{ k = $1 "\t" $2; t[k]++; a[k] += $4; s[k] += $5; if ($4 > 1 && $5 == 1) same[k]++; if ($4 > 1) multi[k]++ }
    END { for (k in t) { split(k, q, "\t"); printf "distinct candidates config %s arm %s: %.2f per task over %.2f attempts; every attempt identical on %d of %d tasks with 2+ attempts\n", q[1], q[2], s[k] / t[k], a[k] / t[k], same[k], multi[k] } }' "$OUT/distinct.tsv" | LC_ALL=C sort
  while IFS=$'\t' read -r c both bo bt none p; do
    echo "paired attempt 1 config $c: both $both, only omegese $bo, only text $bt, neither $none; exact two-sided McNemar p = $p"
  done < "$OUT/paired.tsv"
  if [ "$heldout" = 1 ]; then for c in "${!FUTD[@]}"; do
    echo "stop rule config $c (omegese attempt 1 <= $FMAX): ${FUTD[$c]}$([ "${STOPPED[$c]:-0}" = 1 ] && echo ', attempts 2 and later not run')"; done; fi
  echo
  echo "$gate"
} > "$OUT/results.txt"
cat "$OUT/results.txt"
echo "score.sh: results in $OUT" >&2
