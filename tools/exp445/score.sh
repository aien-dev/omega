#!/usr/bin/env bash
# tools/exp445/score.sh [--configs A,B] <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...] -- score the
# generated candidates of driver runs (run.sh) and write the results table (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md sections 4, 7, 8).
#   1 provenance check, before anything is scored (any failure: RUN INVALID, no tables, exit 1):
#     - every run: meta.txt protocol=PARAMS (no override flag), params_sha256 = the PARAMS in force,
#       the configuration named in PARAMS with its frozen model, endpoint and request extra, the arm
#       prompts equal to the ones the current arm documents give, the public view of these tasks,
#       a clean generation commit whose tools equal the scoring commit's
#     - every run: SHA256SUMS verifies and lists exactly the saved requests, responses and prompts
#     - every generation: its candidate is re-derived from the hashed response with the same reader
#       (reader.sh) and must match log.tsv's candidate_sha256 and the file in cands/
#     - the matrix is complete: every task of the tarball x every arm x attempts 1..N x every config
#       in --configs (default: every config named in PARAMS), exactly once; held-out = heldout_tasks
#   2 merge the candidates; tests/one_layer/acceptance/ola.sh experiment (seal, references, replay,
#     probes, clean tree): its RUN INVALID also stops here
#   3 results.txt / results.tsv: per config x arm, success on attempt 1 and on any attempt with Wilson
#     95% intervals, tokens, wall seconds, hex penalty, and the gate of protocol section 8
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
ARMS="$(kv arms)" ATT="$(kv attempts)" params_sha="$(sha256sum "$PARAMS" | cut -d' ' -f1)"
heldout=0; for t in "${TASKS[@]}"; do case "$t" in dev*) ;; *) heldout=1 ;; esac; done
[ "$heldout" = 0 ] || [ "${#TASKS[@]}" = "$(kv heldout_tasks)" ] || inval "held-out tarball has ${#TASKS[@]} tasks, the protocol fixes $(kv heldout_tasks)"
head_commit="$(git -C "$ROOT" rev-parse HEAD)"
for arm in ${ARMS//,/ }; do
  { cat "$HERE/arms/common.md"; echo; cat "$HERE/arms/$arm.md"; [ "$arm" = structured ] && cat "$ROOT/tools/omega_emit/SCHEMA.md"; true; } > "$W/prompt.$arm"
done
: > "$W/rows"
for r in "${runs[@]}"; do
  m="$r/meta.txt"
  mv_() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$m"; }
  [ -f "$m" ] && [ -f "$r/log.tsv" ] && [ -f "$r/SHA256SUMS" ] && [ -d "$r/cands" ] || { inval "$r: not a complete run dir (meta.txt, log.tsv, SHA256SUMS, cands/)"; continue; }
  c="$(mv_ config)"
  [[ ",$CONFIGS," == *",$c,"* ]] || inval "$r: config '$c' is not one of the configs scored ($CONFIGS)"
  [ -n "$(kv "config.$c.model")" ] || inval "$r: config '$c' is not named in PARAMS"
  [ "$(mv_ model)" = "$(kv "config.$c.model")" ] || inval "$r: model $(mv_ model) is not config $c's frozen model"
  [ "$(mv_ base_url)" = "$(kv "config.$c.base_url")" ] || inval "$r: endpoint $(mv_ base_url) is not config $c's frozen endpoint"
  [ "$(mv_ request_extra)" = "$(kv "config.$c.extra" | jq -c . 2>/dev/null)" ] || inval "$r: request extra differs from config $c's frozen extra"
  [ "$(mv_ protocol)" = PARAMS ] || inval "$r: settings overridden on the command line ($(mv_ protocol))"
  [ "$(mv_ params_sha256)" = "$params_sha" ] || inval "$r: PARAMS changed since generation (params_sha256 differs)"
  [ "$(mv_ arms)" = "$ARMS" ] && [ "$(mv_ attempts)" = "$ATT" ] || inval "$r: arms/attempts differ from PARAMS"
  [ "$(mv_ tasks_public_sha256)" = "$pub_sha" ] || inval "$r: generated from a different public task view"
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
  (cd "$r" && sha256sum -c --quiet --strict SHA256SUMS > /dev/null 2>&1) || inval "$r: SHA256SUMS does not verify (a request, response or prompt changed)"
  (cd "$r" && find gen prompts -type f \( -name '*.json' -o -name '*.system.txt' \) | LC_ALL=C sort) > "$W/have"
  awk '{ print substr($0, 67) }' "$r/SHA256SUMS" | LC_ALL=C sort > "$W/listed"
  cmp -s "$W/have" "$W/listed" || inval "$r: SHA256SUMS does not list exactly the saved requests, responses and prompts"
  [ "$(head -1 "$r/log.tsv")" = "$(printf 'set\ttask\trounds\tprompt_tokens\tcompletion_tokens_r1\tcompletion_tokens_r2\tfinish_r1\tfinish_r2\twall_s\tcheck_r1\tcheck_r2\tcandidate_sha256')" ] ||
    inval "$r: log.tsv header"
  # every generation: re-derive the candidate from the hashed response
  while IFS=$'\t' read -r sid task rounds _ _ _ _ _ _ k1 k2 csha; do
    echo "$sid $task" >> "$W/rows"
    arm="$(echo "$sid" | cut -d- -f2)"; d="$r/gen/$sid/$task"; req="$W/pub/tasks/cases/$task/requirement"
    [ -f "$req" ] || { inval "$r: $sid $task is not a task of this tarball"; continue; }
    [ -f "$d/done" ] || inval "$r: $sid $task never finished"
    final=""
    if [ "$rounds" = 1 ] && [ "$k2" = - ]; then final=1; elif [ "$rounds" = 2 ]; then final=2; fi
    want=-
    if [ -n "$final" ]; then
      if [ -f "$d/r$final.resp.json" ]; then
        content="$(jq -r '.choices[0].message.content' "$d/r$final.resp.json")"; printf '%s' "$content" > "$W/content"
        reader "$X" "$EMIT" "$arm" "$W/content" "$W/re" "$req"
        for ext in osc oscir; do [ -f "$W/re.candidate.$ext" ] && want="$(sha256sum < "$W/re.candidate.$ext" | cut -d' ' -f1)"; done
        [ "$(cat "$W/re.check")" = "$([ "$final" = 1 ] && echo "$k1" || echo "$k2")" ] || inval "$r: $sid $task check line does not follow from the response"
      else inval "$r: $sid $task final response missing"; fi
    fi
    [ "$csha" = "$want" ] || inval "$r: $sid $task log candidate_sha256 does not match the candidate re-derived from the response"
    got=-; n=0
    for f in "$r/cands/$sid/$task"/*; do [ -f "$f" ] || continue; n=$((n + 1)); got="$(sha256sum < "$f" | cut -d' ' -f1)"; done
    [ "$n" -le 1 ] && [ "$got" = "$want" ] || inval "$r: $sid $task candidate in cands/ does not match the re-derived candidate"
  done < <(tail -n +2 "$r/log.tsv")
  for s in "$r"/cands/*/*/; do [ -d "$s" ] || continue; sid="$(basename "$(dirname "$s")")"; t="$(basename "$s")"
    awk -F'\t' -v s="$sid" -v t="$t" '$1 == s && $2 == t { f = 1 } END { exit !f }' "$r/log.tsv" || inval "$r: cands/$sid/$t has no log row"; done
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
M="$W/cands"; mkdir "$M"; : > "$OUT/log.tsv"
for r in "${runs[@]}"; do
  for s in "$r"/cands/*/; do [ -d "$s" ] || continue; cp -R "$s" "$M/$(basename "$s")"; done
  tail -n +2 "$r/log.tsv" >> "$OUT/log.tsv"
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
