#!/usr/bin/env bash
# tools/gen/reducer/reduce.sh -- GEN reducer and decision receipt writer (omega#477, GEN-2; master #475).
# Format: tools/gen/runner/FORMATS.md section 5 (decision receipt v1). Statistics: build/gen/genstat
# (tools/gen/reducer/genstat.c: Wilson, exact one-sided McNemar, Newcombe method 10, the verdict rule;
# checked against Agent A's stats vectors). Shell + jq + the repo's C tools; no Python.
#
#   reduce.sh --run <run dir> --harness <ola experiment run dir> [--pins FILE] [--grading FILE] --out <new dir>
#
# <run dir>: written by tools/gen/runner/run.sh. <harness run dir>: evidence/ONE-LAYER-2/runs/<id> of
# `ola.sh experiment <tasks.tar> <TASKS.sha256> <run dir>/cands` (receipts in <harness>/../../receipts).
# --pins: key=value lines; each key must name a run meta.txt key with exactly that value, except
# tasks_corpus_sha256 (the harness task corpus). --grading: an independent grading receipt; only its sha256
# is recorded. Validity (any failure = INVALID, every failure listed):
#   the run's SHA256SUMS verifies and lists every file; meta.txt mask=off and the pins; every manifest's
#   name is its sha256, its blobs exist under their sha256, its prefix is the meta prefix of its condition;
#   every round-1 request is rebuilt byte for byte from the manifest blobs, the meta model / extra /
#   max_tokens and the seed and sampling rule of its attempt; greedy gate PASS (recomputed from its replies);
#   G0 recomputed from its record; log.tsv has exactly one v1 row per generation, done markers present;
#   harness score RUN VALID (GEN_TEST=1 only: RUN INVALID whose only defect is clean_tree=no); replay
#   verdicts identical; every verdict's receipt present, hashing to its name, naming that case, verdict and
#   candidate; the harness candidates.sha256 = the run's cands/; the harness sets = exactly the run's sets,
#   each with every task once; genstat wilson4 = x445 wilson for every arm (cross-check of two programs).
# Writes <out>/aggregate.txt, receipt.txt, receipt.sha256. Exit 0 whatever the verdict; 2 on usage errors.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
die() { echo "gen reduce.sh: $*" >&2; exit 2; }
TEST="${GEN_TEST:-0}"
RUN="" H="" PINS="" GRAD="" OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --run) RUN="$2"; shift 2 ;; --harness) H="$2"; shift 2 ;; --pins) PINS="$2"; shift 2 ;;
    --grading) GRAD="$2"; shift 2 ;; --out) OUT="$2"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$RUN" ] && [ -n "$H" ] && [ -n "$OUT" ] || die "usage: reduce.sh --run DIR --harness DIR [--pins FILE] [--grading FILE] --out DIR"
[ -f "$RUN/meta.txt" ] && [ -f "$H/score.txt" ] || die "no run meta.txt or harness score.txt"
[ ! -e "$OUT" ] || die "$OUT exists: never overwritten"
[ -z "$PINS" ] || [ -f "$PINS" ] || die "no pins file $PINS"
[ -z "$GRAD" ] || [ -f "$GRAD" ] || die "no grading receipt $GRAD"
command -v jq > /dev/null || die "jq is required"
. "$ROOT/tools/exp445/reader.sh"
GS="$ROOT/build/gen/genstat" X="$ROOT/build/omega_emit/x445"
[ -x "$GS" ] && [ -x "$X" ] || die "build first: make test-gen-runner builds build/gen/genstat; x445 = make omega-emit"
RUN="$(realpath -e "$RUN")" H="$(realpath -e "$H")"
RECD="$(realpath -m "$H/../../receipts")"
sha() { sha256sum < "$1" | cut -d' ' -f1; }
shas() { printf '%s' "$1" | sha256sum | cut -d' ' -f1; }
mk() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$RUN/meta.txt"; }
W="$(mktemp -d "${TMPDIR:-/tmp}/gen-reduce.XXXXXXXX")"; trap 'rm -rf "$W"' EXIT
: > "$W/bad"
bad() { printf '%s\n' "${*//[$'\t\n']/ }" >> "$W/bad"; }
cnt() { grep -c . "$1" 2> /dev/null || true; }

# ---- run record --------------------------------------------------------------------------------------------
[ "$(mk schema)" = omega.gen.run_meta.v1 ] || bad "run meta.txt schema is not omega.gen.run_meta.v1"
if [ -f "$RUN/SHA256SUMS" ] && (cd "$RUN" && sha256sum -c --strict --quiet SHA256SUMS > /dev/null 2>&1); then
  [ "$(cd "$RUN" && find . -type f ! -path ./SHA256SUMS ! -path ./.lock -printf '%P\n' | LC_ALL=C sort)" = "$(cut -c67- "$RUN/SHA256SUMS")" ] ||
    bad "run SHA256SUMS does not list exactly the run's files"
else bad "run SHA256SUMS missing or does not verify"; fi
[ "$(mk mask)" = off ] || bad "run mask is '$(mk mask)', not off"
if [ -f "$RUN/protocol.json" ] && [ "$(sha "$RUN/protocol.json")" = "$(mk protocol_json_sha256)" ]; then
  jq -e '.decision | .schema == "omega.gen.decision.v1" and .primary == "attempt1_round1_functional" and .controls == ["A","B","C"] and .delta == 0.1 and .alpha == 0.05
    and .test == "mcnemar_exact_one_sided" and .ci == "newcombe10" and .fail_control == "C" and .sanity_ab_max == 2 and .z95 == 1.959963984540054 and .z90 == 1.644853626951472' \
    "$RUN/protocol.json" > /dev/null 2>&1 ||
    bad "protocol.json decision block is not the rule this reducer applies"
else bad "run protocol.json missing or not the one meta.txt names"; fi
[ "$(mk test_mode)" = no ] || [ "$TEST" = 1 ] || bad "the run was made in test mode (GEN_TEST=1)"
if [ -n "$PINS" ]; then
  while IFS= read -r l; do
    case "$l" in ''|'#'*) continue ;; esac
    k="${l%%=*}" v="${l#*=}"
    [ "$k" = tasks_corpus_sha256 ] && continue
    [ "$(grep -c "^$k=" "$RUN/meta.txt" || true)" = 1 ] && [ "$(mk "$k")" = "$v" ] || bad "pin $k: run has '$(mk "$k")', pinned '$v'"
  done < "$PINS"
fi
MODEL="$(mk model)" EXTRA="$(mk request_extra)" MAXT="$(mk max_tokens)" SB="$(mk seed_base)" ATT="$(mk attempts)"
GTEMP="$(mk greedy_temperature)" GTOPP="$(mk greedy_top_p)" TEMP="$(mk temperature)" TOPP="$(mk top_p)"
mapfile -t IDS < <(awk -F'\t' '$1 == "A" { print $2 }' "$RUN/manifests.tsv" 2> /dev/null | LC_ALL=C sort)
N="${#IDS[@]}"
[ "$N" -gt 0 ] && [ "$N" = "$(mk tasks)" ] || bad "manifests.tsv names $N tasks, meta.txt $(mk tasks)"
[ "$(sha "$RUN/manifests.tsv")" = "$(mk manifests_sha256)" ] || bad "manifests.tsv is not the one meta.txt names"
declare -A MAN TI
i=0; for t in "${IDS[@]}"; do i=$((i + 1)); TI[$t]=$i; done
while IFS=$'\t' read -r cd t mh; do
  MAN[$cd:$t]="$mh"; f="$RUN/manifests/$mh"
  [ -f "$f" ] && [ "$(sha "$f")" = "$mh" ] || { bad "manifest $cd $t is missing or not named by its sha256"; continue; }
  mfv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$f"; }
  [ "$(mfv schema)" = omega.gen.prompt_manifest.v1 ] && [ "$(mfv condition)" = "$cd" ] && [ "$(mfv task)" = "$t" ] || bad "manifest $mh: schema / condition / task"
  ph="$(mfv prefix_sha256)" sh="$(mfv suffix_sha256)" uh="$(mfv user_sha256)"
  want="$(mk prefix_sha256.B)"; [ "$cd" = A ] && want="$(mk prefix_sha256.A)"
  [ "$ph" = "$want" ] || bad "manifest $cd $t: prefix is not the meta prefix of condition $cd"
  for b in "$ph" "$sh" "$uh"; do [ -f "$RUN/blobs/$b" ] && [ "$(sha "$RUN/blobs/$b")" = "$b" ] || bad "manifest $cd $t: blob $b missing or altered"; done
  if [ "$cd" = A ] || [ "$cd" = B ]; then [ "$sh" = "$(shas "")" ] && [ "$(mfv retrieved)" = - ] || bad "manifest $cd $t carries retrieved material"; fi
done < "$RUN/manifests.tsv"
for cd in A B C D; do for t in "${IDS[@]}"; do [ -n "${MAN[$cd:$t]:-}" ] || bad "no manifest for $cd $t"; done; done

# greedy gate, recomputed from its replies
GG="$RUN/gate/greedy" GGATE=FAIL
if [ "$(tail -n 1 "$GG/verdict.txt" 2> /dev/null)" = "GREEDY PASS" ]; then
  GGATE=PASS
  IFS=, read -r -a GT <<< "$(mk greedy_gate_tasks)"
  for t in "${GT[@]}"; do
    d="$GG/$t"
    if round_ok "$d/r1" && round_ok "$d/r2" && [ "$(jq -j '.choices[0].message.content' "$d/r1.resp.json" | sha256sum)" = "$(jq -j '.choices[0].message.content' "$d/r2.resp.json" | sha256sum)" ] &&
       [ "$(jq -r '.usage.aien_sampling.mode' "$d/r1.resp.json" "$d/r2.resp.json" | sort -u)" = greedy ]; then :; else GGATE=FAIL; bad "greedy gate verdict PASS but task $t's replies do not support it"; fi
  done
else bad "greedy gate is not PASS"; fi
# G0, recomputed from its record (reader.sh g0_derive / g0_pass)
G0=NOT_RUN
if [ -d "$RUN/gate/g0" ]; then
  G0="$(tail -n 1 "$RUN/gate/g0/verdict.txt" | sed -n 's/^G0 \(PASS\|FAIL\)$/\1/p')"
  der="$(g0_derive "$RUN/gate/g0" "$(mk g0_seeds)" 2> /dev/null || true)"
  if g0_pass "$der" "$(mk g0_seeds)"; then g=PASS; else g=FAIL; fi
  [ "$g" = "$G0" ] && grep -qxF "derived=$der" "$RUN/gate/g0/verdict.txt" || { bad "G0 verdict '$G0' does not follow from its record ($der)"; G0=FAIL; }
fi
NA=1; P3=UNAVAILABLE
if [ "$G0" = PASS ]; then NA="$ATT"; P3=AVAILABLE; fi
SETS=(); for at in $(seq 1 "$NA"); do for c in a b c d; do SETS+=("$c-$at" "$c-$at-final"); done; done
mapfile -t SETS < <(printf '%s\n' "${SETS[@]}" | LC_ALL=C sort)

# run rows and requests
HDR="$(printf '%s\t' set task rounds prompt_tokens completion_tokens_r1 completion_tokens_r2 finish_r1 finish_r2 wall_s check_r1 check_r2 candidate_sha256 \
  condition attempt seed temperature top_p max_tokens prompt_tokens_r1 prompt_tokens_r2 tries_r1 tries_r2 mode_r1 mode_r2 seed_reported_r1 wall_s_r1 \
  candidate_r1_sha256 manifest_sha256 retrieval_sha256 retrieved suffix_bytes)"; HDR="${HDR%$'\t'}"
[ "$(head -n 1 "$RUN/log.tsv")" = "$HDR" ] || bad "log.tsv header is not run row v1"
tail -n +2 "$RUN/log.tsv" > "$W/rows"
[ "$(awk -F'\t' 'NF != 31' "$W/rows" | grep -c . || true)" = 0 ] || bad "log.tsv has rows without 31 columns"
[ "$(cut -f1,2 "$W/rows" | LC_ALL=C sort | uniq -d | grep -c . || true)" = 0 ] || bad "log.tsv has two rows for one generation"
[ "$(cnt "$W/rows")" = "$((4 * N * NA))" ] || bad "log.tsv has $(cnt "$W/rows") rows, the run needs $((4 * N * NA)) (4 conditions x $N tasks x $NA attempts)"
[ "$(awk -F'\t' '$14 > 1' "$W/rows" | grep -c . || true)" = 0 ] || [ "$G0" = PASS ] || bad "log.tsv has sampled attempts without a G0 PASS"
while IFS=$'\t' read -r -a r; do
  s="${r[0]}" t="${r[1]}" cd="${r[12]}" at="${r[13]}" d="$RUN/gen/${r[0]}/${r[1]}"
  [ "$s" = "$(tr A-Z a-z <<< "$cd")-$at" ] && [ -n "${TI[$t]:-}" ] || { bad "log.tsv row $s $t: set, condition, attempt or task"; continue; }
  [ -f "$d/done" ] || bad "generation $s $t has no done marker"
  tt="$GTEMP" tp="$GTOPP"; [ "$at" = 1 ] || { tt="$TEMP" tp="$TOPP"; }
  seed="$(seed_of "$SB" "$at" "${TI[$t]}")"
  [ "${r[14]}" = "$seed" ] && [ "${r[15]}" = "$tt" ] && [ "${r[16]}" = "$tp" ] && [ "${r[17]}" = "$MAXT" ] && [ "${r[27]}" = "${MAN[$cd:$t]:-}" ] ||
    bad "log.tsv row $s $t: seed / temperature / top_p / max_tokens / manifest differ from the run rule"
  mf="$RUN/manifests/${MAN[$cd:$t]:-none}"
  [ -f "$mf" ] || continue
  ph="$(sed -n 's/^prefix_sha256=//p' "$mf")" sh="$(sed -n 's/^suffix_sha256=//p' "$mf")" uh="$(sed -n 's/^user_sha256=//p' "$mf")"
  sfx="$(cat "$RUN/blobs/$sh" 2> /dev/null; printf x)"; sys="$(cat "$RUN/blobs/$ph" 2> /dev/null)${sfx%x}"
  q="$(request_json "$MODEL" "$(first_msgs "$sys" "$(cat "$RUN/blobs/$uh" 2> /dev/null)")" "$MAXT" "$tt" "$tp" "$seed" "$EXTRA")"
  [ -f "$d/r1.req.json" ] && [ "$(cat "$d/r1.req.json")" = "$q" ] || bad "request $s $t round 1 is not the request its manifest, pins and seed rule give"
  [ "${r[29]}" = "$(sed -n 's/^retrieved=//p' "$mf")" ] && [ "${r[30]}" = "$(sed -n 's/^suffix_bytes=//p' "$mf")" ] || bad "log.tsv row $s $t: retrieved / suffix_bytes differ from the manifest"
done < "$W/rows"

# ---- harness record ----------------------------------------------------------------------------------------
sc="$H/score.txt"
last="$(tail -n 1 "$sc")" prev="$(tail -n 2 "$sc" | head -n 1)"
if [ "$last" != "RUN VALID" ]; then
  if [ "$TEST" = 1 ] && [ "$last" = "RUN INVALID" ] && [ "$prev" = "replay_identical=yes probes=PASS clean_tree=no" ] &&
     grep -qx "references_accepted=$N/$N" "$sc" && ! grep -q 'errors=[1-9]' "$sc"; then :
  else bad "harness score is '$last' ($prev)"; fi
fi
[ "$(cnt "$H/sets/references/verdicts.txt")" = "$N" ] || bad "harness references verdicts do not cover $N tasks"
[ "$(cut -d' ' -f1 "$H/sets/references/verdicts.txt" | LC_ALL=C sort)" = "$(printf '%s\n' "${IDS[@]}")" ] || bad "harness tasks are not the run's tasks"
HCS="$(sed -n 's/^corpus_sha256=//p' "$H/meta.txt" 2> /dev/null)"
if [ -n "$PINS" ] && grep -q '^tasks_corpus_sha256=' "$PINS"; then
  [ "$(sed -n 's/^tasks_corpus_sha256=//p' "$PINS")" = "$HCS" ] || bad "pin tasks_corpus_sha256: harness has '$HCS'"
fi
[ "$(ls "$H/sets" | grep -vx references | LC_ALL=C sort)" = "$(printf '%s\n' "${SETS[@]}")" ] || bad "harness sets are not exactly the run's sets (${SETS[*]})"
[ "$(cat "$H/candidates.sha256" 2> /dev/null)" = "$(cd "$RUN/cands" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum)" ] ||
  bad "harness candidates.sha256 is not the run's cands/"
declare -A V R
for s in "${SETS[@]}"; do
  vf="$H/sets/$s/verdicts.txt"
  [ -f "$vf" ] && [ -f "$H/sets/$s/replay/verdicts.txt" ] && cmp -s "$vf" "$H/sets/$s/replay/verdicts.txt" || bad "set $s: verdicts missing or replay differs"
  [ -f "$vf" ] || continue
  [ "$(cut -d' ' -f1 "$vf" | LC_ALL=C sort)" = "$(printf '%s\n' "${IDS[@]}")" ] || bad "set $s does not hold every task exactly once"
  while read -r t v why rs rest; do
    [ -z "${rest:-}" ] && { [ "$v" = ACCEPT ] || [ "$v" = REJECT ]; } || { bad "set $s task $t: verdict line"; continue; }
    V[$s:$t]="$v" R[$s:$t]="$why"
    cf="$RUN/cands/$s/$t/candidate.oscir" want=-; [ -f "$cf" ] && want="$(sha "$cf")"
    rf="$RECD/$rs"
    [ -f "$rf" ] && [ "$(sha "$rf")" = "$rs" ] && grep -qx "case=$t" "$rf" && grep -qx "verdict=$v" "$rf" && grep -qx "candidate_sha256=$want" "$rf" ||
      bad "set $s task $t: receipt $rs missing, altered, or not this case / verdict / candidate"
  done < "$vf"
done
# cross-check two Wilson programs (x445 = 4 decimals, z 1.959964)
for c in A B C D; do
  k="$(for t in "${IDS[@]}"; do [ "${V[$(tr A-Z a-z <<< "$c")-1:$t]:-}" = ACCEPT ] && echo; done | wc -l)"
  [ "$("$GS" wilson4 "$k" "$N")" = "$("$X" wilson "$k" "$N")" ] || bad "genstat wilson4 and x445 wilson disagree at $k/$N"
done

# ---- aggregate -----------------------------------------------------------------------------------------------
NB="$(cnt "$W/bad")"
COMPLETE=1
for s in "${SETS[@]}"; do for t in "${IDS[@]}"; do [ -n "${V[$s:$t]:-}" ] || COMPLETE=0; done; done
[ "$COMPLETE" = 1 ] || bad "the paired matrix is incomplete"
NB="$(cnt "$W/bad")"
for t in "${IDS[@]}"; do
  printf '%s' "$t"; for c in a b c d; do [ "${V[$c-1:$t]:-}" = ACCEPT ] && printf '\t1' || printf '\t0'; done; printf '\n'
done > "$W/matrix.tsv"
[ "$COMPLETE" = 1 ] && "$GS" paired "$W/matrix.tsv" > "$W/paired"
gv() { sed -n "s/^$1=//p" "$W/paired"; }
fx() { awk -v a="$1" -v b="$2" 'BEGIN { if (b + 0 == 0) print "NA"; else printf "%.6f\n", a / b }'; }
col() { awk -F'\t' -v s="$1" -v c="$2" '$1 == s && $c ~ /^[0-9]+(\.[0-9]+)?$/ { x += $c } END { printf "%s\n", x + 0 }' "$W/rows"; }
if [ "$NB" -gt 0 ]; then VERDICT=INVALID; else VERDICT="$(gv decision)"; fi
{
  echo "schema=omega.gen.decision.v1"; echo "verdict=$VERDICT"; echo "test_mode=$([ "$TEST" = 1 ] || [ "$(mk test_mode)" != no ] && echo yes || echo no)"
  if [ "$NB" -gt 0 ]; then echo "reasons=$NB"; awk '{ print "reason." NR "=" $0 }' "$W/bad"
  else echo "reasons=1"; echo "reason.1=$(gv decision_code): $(gv decision_detail)"; fi
  echo "pin.revision=$(mk revision)"; echo "pin.protocol_sha256=$(mk protocol_sha256)"; echo "pin.params_sha256=$(mk params_sha256)"
  echo "pin.exp445_params_sha256=$(mk exp445_params_sha256)"; echo "pin.config=$(mk config)"; echo "pin.model=$MODEL"; echo "pin.base_url=$(mk base_url)"
  echo "pin.server=$(mk server)"; echo "pin.request_extra=$EXTRA"; echo "pin.weights_list_sha256=$(mk weights_list_sha256)"; echo "pin.mask=$(mk mask)"
  echo "pin.commit=$(mk commit)"; echo "pin.source_sha256=$(mk source_sha256)"; echo "pin.tasks_public_sha256=$(mk tasks_public_sha256)"
  echo "pin.tasks_corpus_sha256=${HCS:--}"; echo "pin.prefix_sha256.A=$(mk prefix_sha256.A)"; echo "pin.prefix_sha256.B=$(mk prefix_sha256.B)"
  echo "pin.examples_sha256=$(mk examples_sha256)"; echo "pin.provider_sha256=$(mk provider_sha256)"; echo "pin.provider_contract_sha256=$(mk provider_contract_sha256)"
  echo "pin.corpus_manifest_sha256=$(mk corpus_manifest_sha256.raw),$(mk corpus_manifest_sha256.abstraction)"; echo "pin.retrieval_mode=$(mk retrieval_mode)"; echo "pin.k=$(mk k)"; echo "pin.cap_bytes=$(mk cap_bytes)"
  echo "pin.manifests_sha256=$(mk manifests_sha256)"
  printf '%s\n' rule.primary=attempt1_round1_functional rule.controls=A,B,C rule.delta=0.100000 rule.alpha=0.050000 rule.fail_control=C rule.ci=newcombe10 rule.sanity_ab_max=2
  echo "gate.greedy=$GGATE"; echo "gate.g0=$G0"; echo "pass3=$P3"; echo "tasks=$N"
  if [ "$COMPLETE" = 1 ]; then
    for c in A B C D; do
      l="$(tr A-Z a-z <<< "$c")" k=0 kf=0 k3=0 ck=0
      for t in "${IDS[@]}"; do
        [ "${V[$l-1:$t]}" = ACCEPT ] && k=$((k + 1)); [ "${V[$l-1-final:$t]}" = ACCEPT ] && kf=$((kf + 1))
        a=0; for at in $(seq 1 "$NA"); do [ "${V[$l-$at:$t]}" = ACCEPT ] && a=1; done; k3=$((k3 + a))
      done
      ck="$(awk -F'\t' -v s="$l-1" '$1 == s && $10 == "OK"' "$W/rows" | grep -c . || true)"
      read -r p lo hi <<< "$("$GS" wilson "$k" "$N")"
      pt="$(col "$l-1" 19)" ct="$(col "$l-1" 5)"
      echo "arm.$c.success_a1=$k/$N"; echo "arm.$c.p_a1=$p"; echo "arm.$c.wilson95_lo=$lo"; echo "arm.$c.wilson95_hi=$hi"
      echo "arm.$c.checker_ok_a1=$ck/$N"; echo "arm.$c.success_a1_final=$kf/$N"
      [ "$P3" = AVAILABLE ] && echo "arm.$c.pass3=$k3/$N" || echo "arm.$c.pass3=UNAVAILABLE"
      echo "arm.$c.prompt_tokens_a1=$pt"; echo "arm.$c.completion_tokens_a1=$ct"
      echo "arm.$c.completion_tokens_per_correct_a1=$(fx "$ct" "$k")"; echo "arm.$c.tokens_per_correct_a1=$(fx "$((pt + ct))" "$k")"
      echo "arm.$c.suffix_bytes_mean=$(fx "$(col "$l-1" 31)" "$N")"
    done
    grep '^pair\.\|^sanity\.' "$W/paired"
    for t in "${IDS[@]}"; do echo "paired.$t=$(grep -P "^\Q$t\E\t" "$W/matrix.tsv" | cut -f2- | tr -d '\t')"; done
    for c in A B C D; do l="$(tr A-Z a-z <<< "$c")"
      for t in "${IDS[@]}"; do [ "${V[$l-1:$t]}" = ACCEPT ] || echo "${R[$l-1:$t]}"; done | LC_ALL=C sort | uniq -c | awk -v c="$c" '{ print "fail." c "." $2 "=" $1 }'
    done
    for c in A B C D; do l="$(tr A-Z a-z <<< "$c")"
      awk -F'\t' -v s="$l-1" '$1 == s { k = $10; if (k == "OK") st = "OK"; else if (k == "REQUEST FAILED") st = "request_failed"; else { split(k, w, " "); st = w[2]; sub(/:$/, "", st) } print st }' "$W/rows" |
        LC_ALL=C sort | uniq -c | awk -v c="$c" '{ print "check." c "." $2 "=" $1 }'
    done
  fi
} > "$W/aggregate.txt"
grep -qP '\t' "$W/aggregate.txt" && die "internal: a tab in aggregate.txt"
{
  cat "$W/aggregate.txt"
  if [ "$COMPLETE" = 1 ]; then for c in A B C D; do l="$(tr A-Z a-z <<< "$c")" k="$(grep "^arm.$c.success_a1=" "$W/aggregate.txt" | sed 's/.*=\([0-9]*\)\/.*/\1/')"
    ws="$(col "$l-1" 26)"; echo "time.$c.wall_s_a1=$(fx "$ws" 1)"; echo "time.$c.wall_s_per_correct_a1=$(fx "$ws" "$k")"; done; fi
  echo "input.run_sha256sums=$(sha "$RUN/SHA256SUMS" 2> /dev/null || echo -)"; echo "input.run_meta_sha256=$(sha "$RUN/meta.txt")"; echo "input.log_sha256=$(sha "$RUN/log.tsv")"
  echo "input.harness_score_sha256=$(sha "$sc")"; echo "input.harness_candidates_sha256=$(sha "$H/candidates.sha256" 2> /dev/null || echo -)"
  for s in $(ls "$H/sets" | LC_ALL=C sort); do echo "input.harness_set.$s=$(sha "$H/sets/$s/verdicts.txt" 2> /dev/null || echo -)"; done
  echo "input.grading_receipt_sha256=$([ -n "$GRAD" ] && sha "$GRAD" || echo -)"; echo "input.pins_sha256=$([ -n "$PINS" ] && sha "$PINS" || echo -)"
  echo "aggregate_sha256=$(sha "$W/aggregate.txt")"
} > "$W/receipt.txt"
mkdir -p "$OUT"; cp "$W/aggregate.txt" "$W/receipt.txt" "$OUT/"
(cd "$OUT" && sha256sum receipt.txt > receipt.sha256)
echo "gen reduce.sh: verdict $VERDICT ($(sed -n 's/^reason.1=//p' "$OUT/aggregate.txt" | head -c 200)); receipt $OUT/receipt.txt"
