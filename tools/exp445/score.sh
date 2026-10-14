#!/usr/bin/env bash
# tools/exp445/score.sh <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...] -- score the generated
# candidates of one or more driver runs (run.sh) with the sealed harness and write the results table
# (omega#445, docs/one_layer/EXP445_PROTOCOL.md).
#   1 merge every run's cands/<set_id>/ into one private candidates dir (set ids must not collide)
#   2 tests/one_layer/acceptance/ola.sh experiment <tasks.tar> <TASKS.sha256> <merged>  (clean tree,
#     seal, references, replay, probes: RUN VALID|INVALID)
#   3 results.txt / results.tsv next to the first run dir's parent: per config x arm, success on
#     attempt 1 and on any attempt, each with a Wilson 95% interval, tokens and wall seconds from log.tsv
# OLA_ALLOW_DIRTY / OLA_EVIDENCE_ROOT pass through to ola.sh (selftests only; they make the run INVALID).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "score.sh: $*" >&2; exit 2; }
[ $# -ge 3 ] || die "usage: score.sh <tasks.tar> <TASKS.sha256> <run dir> [<run dir> ...]"
TAR="$(realpath -e "$1")"; SEALED="$(realpath -e "$2")"; shift 2
runs=(); for r in "$@"; do runs+=("$(realpath -e "$r")"); [ -d "${runs[-1]}/cands" ] || die "$r has no cands/"; done
X="$ROOT/build/omega_emit/x445"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit >&2
OUT="${runs[0]}/../score-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir "$OUT"; OUT="$(realpath -e "$OUT")"
M="$(mktemp -d "${TMPDIR:-/tmp}/exp445-cands.XXXXXXXX")"; trap 'rm -rf "$M"' EXIT
: > "$OUT/log.tsv"
for r in "${runs[@]}"; do
  for s in "$r"/cands/*/; do
    [ -d "$s" ] || continue
    sid="$(basename "$s")"; [ ! -e "$M/$sid" ] || die "set $sid appears in two runs"
    cp -R "$s" "$M/$sid"
  done
  tail -n +2 "$r/log.tsv" >> "$OUT/log.tsv"
  echo "$r $(sha256sum "$r/SHA256SUMS" | cut -d' ' -f1)" >> "$OUT/runs.txt"
done
# every generated set needs its directory even when empty (all no_candidate)
while IFS=$'\t' read -r sid _; do mkdir -p "$M/$sid"; done < "$OUT/log.tsv"
"$ROOT/tests/one_layer/acceptance/ola.sh" experiment "$TAR" "$SEALED" "$M" > "$OUT/experiment.out" 2> "$OUT/experiment.err" ||
  { tail -5 "$OUT/experiment.err" >&2; die "ola.sh experiment failed"; }
ev="$(sed -n 's/^ola.sh: evidence in //p' "$OUT/experiment.err" | tail -1)"
case "$ev" in /*) ;; *) ev="$ROOT/$ev" ;; esac
[ -f "$ev/score.txt" ] || die "no harness score.txt"
echo "harness_run=$ev" > "$OUT/meta.txt"
cp "$ev/score.txt" "$OUT/harness_score.txt"
valid="$(tail -1 "$ev/score.txt")"

# verdicts: set task verdict reason
for v in "$ev"/sets/*/verdicts.txt; do s="$(basename "$(dirname "$v")")"; [ "$s" = references ] && continue
  awk -v s="$s" '{ print s "\t" $1 "\t" $2 "\t" $3 }' "$v"; done > "$OUT/verdicts.tsv"
# per config x arm rows: n, attempt-1 accepted, any-attempt accepted, attempts, generations accepted,
# repaired, completion tokens (sum, mean per generation), prompt tokens mean, wall seconds (sum, mean)
awk -F'\t' '
  FNR == NR { split($1, p, "-"); key = p[1] "\t" p[2]; att = p[3]; acc = ($3 == "ACCEPT");
              keys[key] = 1; tasks[key, $2] = 1; if (att == 1) { n1[key]++; a1[key] += acc }
              if (acc) any[key, $2] = 1; gen[key]++; gacc[key] += acc; ok[$1, $2] = acc; next }
  { split($1, p, "-"); key = p[1] "\t" p[2]; last[$1, $2] = $0 }
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
{
  echo "exp445 results (harness: $valid; $(sed -n 's/^references_accepted=/references accepted /p' "$ev/score.txt"))"
  echo "success = harness ACCEPT on hidden inputs; attempt 1 is the pre-registered primary measure; 95% Wilson intervals"
  echo
  awk -F'\t' 'NR == 1 { next }
    { printf "config %-3s arm %-10s attempt1 %-7s %.3f [%.3f, %.3f]   any-of-attempts %-7s %.3f [%.3f, %.3f]\n", $1, $2, $4, $5, $6, $7, $8, $9, $10, $11
      printf "                          completion tokens %.0f total, %.1f per generation; prompt %.1f per generation; wall %.1f s; repaired %d (accepted %d)\n", $16, $17, $18, $19, $14, $15
      if ($2 == "text") textm[$1] = $17; mean[$1, $2] = $17; cfgs[$1] = 1 }
    END { for (c in cfgs) if (textm[c] > 0) printf "token cost relative to text, config %s: bytes x%.2f, structured x%.2f (mean completion tokens per generation)\n", c, mean[c, "bytes"] / textm[c], mean[c, "structured"] / textm[c] }' "$OUT/results.tsv"
} > "$OUT/results.txt"
cat "$OUT/results.txt"
echo "score.sh: results in $OUT" >&2
