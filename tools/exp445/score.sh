#!/usr/bin/env bash
# tools/exp445/score.sh [--configs A,B] <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...] -- score the
# generated candidates of driver runs (run.sh) and write the results table (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md sections 4, 7, 8).
#   1 provenance check, before anything is scored (any failure: RUN INVALID, no tables, exit 1):
#     - every run: meta.txt protocol=PARAMS (no override flag), params_sha256 = the PARAMS in force, its
#       sampling settings equal to PARAMS, the configuration named in PARAMS with its frozen model,
#       endpoint and request extra, the arm prompts equal to the ones the current arm documents give, the
#       public view of these tasks, a clean generation commit whose tools equal the scoring commit's,
#       weights.check all OK for the config's frozen hash list and server.json listing its model id
#     - every run: SHA256SUMS verifies and lists exactly the raw files, prompts, meta.txt, log.tsv,
#       weights.check and server.json
#     - every generation, from its raw files only: each hashed request equals the one PARAMS, the config,
#       the frozen arm prompt, the public task and the seed rule give; a round 2 exists exactly when the
#       arm's reader refused round 1, and its request is the frozen repair of round 1; every round has a
#       transport record (HTTP status, curl exit, time per try; a failed round shows all its tries); the
#       log row (tokens, finish reasons, wall time, checks, candidate hash) is re-derived from the raw
#       responses (reader.sh gen_row) and log.tsv must equal the re-derived rows; the file in cands/ must
#       be the re-derived candidate
#     - the matrix is complete: every task of the tarball x every arm x attempts 1..N x every config
#       in --configs (default: every config named in PARAMS), exactly once; held-out = heldout_tasks
#   2 merge the candidates; tests/one_layer/acceptance/ola.sh experiment (seal, references, replay,
#     probes, clean tree): its RUN INVALID also stops here
#   3 results.txt / results.tsv from the re-derived rows: per config x arm, success on attempt 1 and on
#     any attempt with Wilson 95% intervals, tokens, wall seconds, and the gate of protocol section 8
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
CONFIGS=""
if [ "${1:-}" = --configs ]; then CONFIGS="$2"; shift 2; fi
[ $# -ge 3 ] || die "usage: score.sh [--configs A,B] <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...]"
TAR="$(realpath -e "$1")"; SEALED="$(realpath -e "$2")"; shift 2
runs=(); for r in "$@"; do runs+=("$(realpath -e "$r")"); done
[ -n "$CONFIGS" ] || CONFIGS="$(awk -F= '$1 ~ /^config\.[A-Za-z0-9]+\.model$/ { split($1, p, "."); printf "%s%s", s, p[2]; s = "," }' "$PARAMS")"
B="$ROOT/build/omega_emit" X="$ROOT/build/omega_emit/x445" EMIT="$ROOT/build/omega_emit/omega_emit"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit >&2
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
heldout=0; for t in "${TASKS[@]}"; do case "$t" in dev*) ;; *) heldout=1 ;; esac; done
[ "$heldout" = 0 ] || [ "${#TASKS[@]}" = "$(kv heldout_tasks)" ] || inval "held-out tarball has ${#TASKS[@]} tasks, the protocol fixes $(kv heldout_tasks)"
head_commit="$(git -C "$ROOT" rev-parse HEAD)"
for arm in ${ARMS//,/ }; do
  { cat "$HERE/arms/common.md"; echo; cat "$HERE/arms/$arm.md"; [ "$arm" = structured ] && cat "$ROOT/tools/omega_emit/SCHEMA.md"; true; } > "$W/prompt.$arm"
done
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
  [ -f "$p.transport" ] || { inval "$G $b has no transport record (never committed)"; return; }
  while IFS= read -r line; do
    [[ "$line" =~ ^try=([0-9]+)\ http=([0-9]{3})\ curl_exit=([0-9]+)\ time_s=[0-9]+(\.[0-9]+)?\ body=([^ ]+)\ msg= ]] ||
      { inval "$G $b transport line $((n + 1)) malformed"; return; }
    [ "${BASH_REMATCH[1]}" = "$n" ] || { inval "$G $b transport tries out of order"; return; }
    h="${BASH_REMATCH[2]}" e="${BASH_REMATCH[3]}" body="${BASH_REMATCH[5]}"
    [ "$succ" = 0 ] || { inval "$G $b has a try after its successful one"; return; }
    case "$body" in
      "$b.resp.json") succ=1
        [ "$h" = 200 ] && [ "$e" = 0 ] && resp_ok "$p.resp.json" || inval "$G $b response is not an HTTP 200 answer with content" ;;
      "$b.err.$n.json") [ -f "${p%/*}/$body" ] || inval "$G $b failed body $body missing"
        if [ "$h" = 200 ] && [ "$e" = 0 ] && resp_ok "${p%/*}/$body"; then inval "$G $b try $n is a valid answer filed as a failure"; fi ;;
      none) if [ "$h" = 200 ] && [ "$e" = 0 ]; then inval "$G $b try $n: HTTP 200 without a body"; fi ;;
      *) inval "$G $b try $n names body $body" ;;
    esac
    n=$((n + 1))
  done < "$p.transport"
  [ "$n" -ge 1 ] && [ "$n" -le $((RT + 1)) ] || inval "$G $b has $n tries, the protocol allows 1..$((RT + 1))"
  [ "$succ" = 1 ] || [ "$n" = $((RT + 1)) ] || inval "$G failed round $b has $n tries, the protocol makes $((RT + 1)) with transport evidence"
  [ "$succ" = 1 ] || [ ! -e "$p.resp.json" ] || inval "$G $b has a response file but no successful try"
  for f in "$p".err.*.json; do [ -f "$f" ] || continue; grep -q " body=${f##*/} " "$p.transport" || inval "$G ${f##*/} is not in the transport record"; done
}
# gcheck <run> <set> <task> <gen dir> <config>: every request against PARAMS and the config, the repair
# chain, the transport records, then the row and the candidate re-derived from the raw responses
gcheck() {
  local r="$1" sid="$2" task="$3" d="$4" cl="$5" v="$W/g" arm at req msgs q a1 k1 r2=no why f
  G="$r: $sid $task"
  [[ "$sid" =~ ^([a-z0-9]+)-([a-z]+)-([1-9])$ ]] && [ "${BASH_REMATCH[1]}" = "$cl" ] || { inval "$G: set is not one of config $cl"; return; }
  arm="${BASH_REMATCH[2]}" at="${BASH_REMATCH[3]}"
  [[ ",$ARMS," == *",$arm,"* ]] && [ "$at" -le "$ATT" ] || { inval "$G: arm or attempt outside PARAMS"; return; }
  req="$W/pub/tasks/cases/$task/requirement"
  [ -f "$req" ] || { inval "$G is not a task of this tarball"; return; }
  [ -f "$d/done" ] || { inval "$G never finished"; return; }
  echo "$sid $task" >> "$W/rows"
  rm -rf "$v"; mkdir -p "$v"
  msgs="$(first_msgs "$(cat "$W/prompt.$arm")" "$(user_msg "$req" "$W/pub/tasks/cases/$task/prompt")")"
  q="$(request_json "$FM" "$msgs" "$MT" "$TT" "$TP" "$(seed_of "$SB" "$at" "${TI[$task]}")" "$FX")"
  why="$(req_same "$d/r1.req.json" "$q")" || inval "$G r1 request differs from the frozen request in: $why"
  tcheck "$d/r1"
  if round_ok "$d/r1"; then
    a1="$(content_of "$d/r1.resp.json")"; printf '%s' "$a1" > "$v/r1.content"
    reader "$X" "$EMIT" "$arm" "$v/r1.content" "$v/r1" "$req"; k1="$(cat "$v/r1.check")"
    if [ "$k1" != OK ]; then
      r2=yes
      q="$(request_json "$FM" "$(repair_msgs "$msgs" "$a1" "$k1")" "$MT" "$TT" "$TP" "$(seed_of "$SB" "$at" "${TI[$task]}")" "$FX")"
      why="$(req_same "$d/r2.req.json" "$q")" || inval "$G round 1 was refused ($k1); r2 request is not its frozen repair, differs in: $why"
      tcheck "$d/r2"
      if round_ok "$d/r2"; then printf '%s' "$(content_of "$d/r2.resp.json")" > "$v/r2.content"; reader "$X" "$EMIT" "$arm" "$v/r2.content" "$v/r2" "$req"; fi
    fi
  fi
  if [ "$r2" = no ]; then for f in "$d"/r2.req.json "$d"/r2.transport "$d"/r2.resp.json "$d"/r2.err.*.json; do
    if [ -e "$f" ]; then inval "$G has a round 2 without a round-1 refusal"; break; fi; done; fi
  for f in "$d"/r[03-9]*.req.json "$d"/r[03-9]*.transport "$d"/r[03-9]*.resp.json; do if [ -e "$f" ]; then inval "$G has a round other than 1 and 2"; break; fi; done
  gen_row "$sid" "$task" "$d" "$v"
  # token counts come from the untrusted server response: each must be a non-negative integer (or "-"
  # for an absent round 2); reader.sh usage_of prints BAD otherwise
  awk -F'\t' '{ for (i = 4; i <= 6; i++) if ($i !~ /^[0-9]+$/ && !(i == 6 && $i == "-")) exit 1 }' <<< "$ROW" ||
    inval "$G token counts in the response are not non-negative integers ($(cut -f4-6 <<< "$ROW" | tr '\t' ' '))"
  printf '%s\n' "$ROW" >> "$W/derived"
  local want got n=0; want="$(cut -f12 <<< "$ROW")" got=-
  for f in "$r/cands/$sid/$task"/*; do [ -f "$f" ] || continue; n=$((n + 1)); got="$(sha256sum < "$f" | cut -d' ' -f1)"; done
  [ "$n" -le 1 ] && [ "$got" = "$want" ] || inval "$G candidate in cands/ does not match the candidate re-derived from the response"
}
: > "$W/rows"; : > "$W/all_derived"
HDR="$(printf 'set\ttask\trounds\tprompt_tokens\tcompletion_tokens_r1\tcompletion_tokens_r2\tfinish_r1\tfinish_r2\twall_s\tcheck_r1\tcheck_r2\tcandidate_sha256')"
for r in "${runs[@]}"; do
  m="$r/meta.txt"
  mv_() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$m"; }
  [ -f "$m" ] && [ -f "$r/log.tsv" ] && [ -f "$r/SHA256SUMS" ] && [ -d "$r/cands" ] && [ -d "$r/gen" ] || { inval "$r: not a complete run dir (meta.txt, log.tsv, SHA256SUMS, gen/, cands/)"; continue; }
  c="$(mv_ config)"; cl="$(echo "$c" | tr 'A-Z' 'a-z')"
  [[ ",$CONFIGS," == *",$c,"* ]] || inval "$r: config '$c' is not one of the configs scored ($CONFIGS)"
  [ -n "$(kv "config.$c.model")" ] || inval "$r: config '$c' is not named in PARAMS"
  FM="$(kv "config.$c.model")" FX="$(kv "config.$c.extra" | jq -c . 2>/dev/null || echo '{}')"
  [ "$(mv_ model)" = "$FM" ] || inval "$r: model $(mv_ model) is not config $c's frozen model"
  [ "$(mv_ base_url)" = "$(kv "config.$c.base_url")" ] || inval "$r: endpoint $(mv_ base_url) is not config $c's frozen endpoint"
  [ "$(mv_ request_extra)" = "$FX" ] || inval "$r: request extra differs from config $c's frozen extra"
  [ "$(mv_ protocol)" = PARAMS ] || inval "$r: settings overridden on the command line ($(mv_ protocol))"
  [ "$(mv_ params_sha256)" = "$params_sha" ] || inval "$r: PARAMS changed since generation (params_sha256 differs)"
  [ "$(mv_ arms)" = "$ARMS" ] && [ "$(mv_ attempts)" = "$ATT" ] || inval "$r: arms/attempts differ from PARAMS"
  for kp in max_tokens_per_round:max_tokens temperature:temperature top_p:top_p seed_base:seed_base timeout_s:timeout_s retries:retries; do
    [ "$(mv_ "${kp%%:*}")" = "$(kv "${kp#*:}")" ] || inval "$r: meta.txt ${kp%%:*} differs from PARAMS"; done
  [ "$(mv_ repair_rounds)" = 1 ] || inval "$r: meta.txt repair_rounds is not 1"
  [ "$(mv_ tasks_public_sha256)" = "$pub_sha" ] || inval "$r: generated from a different public task view"
  wl="$(kv "config.$c.weights")"; case "$wl" in /*|"") ;; *) wl="$ROOT/$wl" ;; esac
  weights_ok "$r" "$wl" "$FM" || inval "$r: weights.check / server.json missing or not all OK for config $c (tools/exp445/check_weights.sh)"
  for arm in ${ARMS//,/ }; do
    [ "$(mv_ "prompt_${arm}_sha256")" = "$(sha256sum < "$W/prompt.$arm" | cut -d' ' -f1)" ] &&
      cmp -s "$r/prompts/$arm.system.txt" "$W/prompt.$arm" || inval "$r: arm $arm prompt differs from the current arm documents"
  done
  gc="$(mv_ commit)"
  if [ "$TEST" != 1 ]; then
    case "$gc" in *-dirty) inval "$r: generated from a dirty tree ($gc)" ;; esac
    git -C "$ROOT" diff --quiet "${gc%-dirty}" "$head_commit" -- tools tests/one_layer docs/one_layer 2> /dev/null ||
      inval "$r: tools at generation commit ${gc%-dirty} differ from the scoring commit"
  fi
  (cd "$r" && sha256sum -c --quiet --strict SHA256SUMS > /dev/null 2>&1) || inval "$r: SHA256SUMS does not verify (a raw file, prompt, meta.txt or log.tsv changed)"
  run_files "$r" > "$W/have"
  awk '{ print substr($0, 67) }' "$r/SHA256SUMS" | LC_ALL=C sort > "$W/listed"
  cmp -s "$W/have" "$W/listed" || inval "$r: SHA256SUMS does not list exactly the raw files, prompts, meta.txt, log.tsv, weights.check and server.json"
  [ "$(head -n 1 "$r/log.tsv")" = "$HDR" ] || inval "$r: log.tsv header"
  # every generation, from its raw files; log.tsv must equal the re-derived rows
  : > "$W/derived"
  for d in "$r"/gen/*/*/; do [ -d "$d" ] || continue; d="${d%/}"; gcheck "$r" "$(basename "$(dirname "$d")")" "$(basename "$d")" "$d" "$cl"; done
  LC_ALL=C sort "$W/derived" > "$W/derived_s"; tail -n +2 "$r/log.tsv" | LC_ALL=C sort > "$W/log_s"
  if ! cmp -s "$W/derived_s" "$W/log_s"; then
    inval "$r: log.tsv differs from the rows re-derived from the raw files (first: $(diff "$W/derived_s" "$W/log_s" | sed -n 's/^[<>] //p' | head -n 1 | cut -f1,2 | tr '\t' ' '))"
  fi
  cat "$W/derived" >> "$W/all_derived"
  for s in "$r"/cands/*/*/; do [ -d "$s" ] || continue; sid="$(basename "$(dirname "$s")")"; t="$(basename "$s")"
    [ -f "$r/gen/$sid/$t/done" ] || inval "$r: cands/$sid/$t has no finished generation"; done
done
# matrix
: > "$W/want"
for c in ${CONFIGS//,/ }; do for arm in ${ARMS//,/ }; do for at in $(seq 1 "$ATT"); do for t in "${TASKS[@]}"; do
  echo "$(echo "$c" | tr 'A-Z' 'a-z')-$arm-$at $t" >> "$W/want"; done; done; done; done
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
echo "provenance OK: ${#runs[@]} runs, $(wc -l < "$W/want_s") generations, configs $CONFIGS" >> "$OUT/validity.txt"

# ---- 2. harness ------------------------------------------------------------------------------------
M="$W/cands"; mkdir "$M"; cp "$W/all_derived" "$OUT/log.tsv"   # the rows re-derived from the raw files
for r in "${runs[@]}"; do
  for s in "$r"/cands/*/; do [ -d "$s" ] || continue; cp -R "$s" "$M/$(basename "$s")"; done
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
# per config x arm rows: n, attempt-1 accepted, tasks, any-attempt accepted, generations, accepted, repaired,
# repaired accepted, completion tokens (sum, mean per generation), prompt tokens mean, wall seconds
awk -F'\t' '
  FNR == NR { split($1, p, "-"); key = p[1] "\t" p[2]; att = p[3]; acc = ($3 == "ACCEPT");
              keys[key] = 1; tasks[key, $2] = 1; if (att == 1) { n1[key]++; a1[key] += acc }
              if (acc) any[key, $2] = 1; gen[key]++; gacc[key] += acc; ok[$1, $2] = acc; next }
  { last[$1, $2] = $0 }
  END {
    for (k in last) { split(last[k], f, "\t"); split(f[1], p, "-"); key = p[1] "\t" p[2]
      c = f[5] + (f[6] == "-" ? 0 : f[6]); tok[key] += c; pt[key] += f[4]; wall[key] += f[9]; lg[key]++
      if (f[3] == 2) { rep[key]++; if (ok[f[1], f[2]]) repok[key]++ } }
    for (key in keys) { na = 0; for (kt in tasks) { split(kt, q, SUBSEP); if (q[1] == key) { nt[key]++; if (any[key, q[2]]) na++ } }
      printf "%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%.1f\t%.1f\t%.1f\t%.2f\n", key, n1[key], a1[key], nt[key], na, gen[key], gacc[key],
             rep[key] + 0, repok[key] + 0, tok[key], (lg[key] ? tok[key] / lg[key] : 0), (lg[key] ? pt[key] / lg[key] : 0), wall[key] }
  }' "$OUT/verdicts.tsv" "$OUT/log.tsv" | LC_ALL=C sort > "$OUT/rows.tsv"
{
  printf 'config\tarm\ttasks\tsuccess_attempt1\tp1\tp1_lo\tp1_hi\tsuccess_any\tpany\tpany_lo\tpany_hi\tgenerations\tgen_accepted\trepaired\trepaired_accepted\tcompletion_tokens_total\tcompletion_tokens_mean\tprompt_tokens_mean\twall_s_total\n'
  while IFS=$'\t' read -r c a n1 a1 nt na g ga rp ro tt tm pm wt; do
    w1="$("$X" wilson "$a1" "$n1" | tr ' ' '\t')"; wa="$("$X" wilson "$na" "$nt" | tr ' ' '\t')"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$c" "$a" "$n1" "$a1/$n1" "$w1" "$na/$nt" "$wa" "$g" "$ga" "$rp" "$ro" "$tt" "$tm" "$pm" "$wt"
  done < "$OUT/rows.tsv"
} > "$OUT/results.tsv"
# gate (protocol section 8): per config, on attempt-1 successes; overall only when every config of
# PARAMS is scored
allcfg="$(awk -F= '$1 ~ /^config\.[A-Za-z0-9]+\.model$/ { split($1, p, "."); print tolower(p[2]) }' "$PARAMS" | LC_ALL=C sort | paste -sd,)"
scored="$(echo "$CONFIGS" | tr 'A-Z,' 'a-z\n' | LC_ALL=C sort | paste -sd,)"
gate="$(awk -F'\t' -v all="$allcfg" -v scored="$scored" 'NR == 1 { next }
  { split($4, q, "/"); k[$1, $2] = q[1]; n[$1, $2] = q[2]; cfg[$1] = 1; tm[$1, $2] = $17 }
  END {
    sg = 1; bw = 1; nc = 0
    for (c in cfg) { nc++
      s = k[c, "structured"]; t = k[c, "text"]; b = k[c, "bytes"]; N = n[c, "structured"]
      sp = (100 * s >= 80 * N) && (100 * (t - s) <= 10 * N)
      bp = (b >= s) && (tm[c, "bytes"] < tm[c, "structured"])
      printf "gate config %s: structured %d/%d, text %d/%d: structured %s (needs >= 80%% and at most 10 points below text)\n", c, s, N, t, n[c, "text"], sp ? "PASS" : "FAIL"
      printf "bytes rule config %s: bytes %d/%d vs structured %d/%d, mean completion tokens %.1f vs %.1f: bytes %s\n", c, b, n[c, "bytes"], s, N, tm[c, "bytes"], tm[c, "structured"], bp ? "WINS" : "does not win"
      sg = sg && sp; bw = bw && bp }
    if (scored != all) { printf "GATE NOT EVALUATED: configs scored %s, the protocol needs %s\n", scored, all; exit }
    printf "GATE %s (structured passes in every config)\n", sg ? "PASS" : "FAIL"
    printf "BYTES %s (bytes wins only with >= structured successes and fewer mean completion tokens in every config)\n", bw ? "WIN" : "NO WIN"
  }' "$OUT/results.tsv")"
{
  echo "${mode}exp445 results (harness: $valid; $(sed -n 's/^references_accepted=/references accepted /p' "$ev/score.txt"); provenance OK)"
  echo "success = harness ACCEPT on hidden inputs; attempt 1 is the pre-registered primary measure; 95% Wilson intervals"
  echo
  awk -F'\t' 'NR == 1 { next }
    { printf "config %-3s arm %-10s attempt1 %-7s %.3f [%.3f, %.3f]   any-of-attempts %-7s %.3f [%.3f, %.3f]\n", $1, $2, $4, $5, $6, $7, $8, $9, $10, $11
      printf "                          completion tokens %.0f total, %.1f per generation; prompt %.1f per generation; wall %.1f s; repaired %d (accepted %d)\n", $16, $17, $18, $19, $14, $15
      if ($2 == "text") textm[$1] = $17; mean[$1, $2] = $17; cfgs[$1] = 1 }
    END { for (c in cfgs) if (textm[c] > 0) printf "token cost relative to text, config %s: bytes x%.2f, structured x%.2f (mean completion tokens per generation)\n", c, mean[c, "bytes"] / textm[c], mean[c, "structured"] / textm[c] }' "$OUT/results.tsv"
  echo
  echo "$gate"
} > "$OUT/results.txt"
cat "$OUT/results.txt"
echo "score.sh: results in $OUT" >&2
