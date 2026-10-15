#!/usr/bin/env bash
# tools/exp445/g0.sh --config A --out <run dir> -- G0, the sampling gate of protocol r3 (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md section 13.4), run before every stage and per configuration.
# On the configuration's frozen endpoint: the dev task g0_task (PARAMS, dev01) in the omegese arm with the
# many-shot prompt, temperature and top_p from PARAMS, g0_seeds requests with seeds seed_of(seed_base, k,
# task index) for k = 1 .. g0_seeds (the seeds of that task's attempts 1, 2, ...), then one repeat of the
# first seed. PASS when at least g0_min_distinct of the g0_seeds answers are distinct and the repeat is
# byte-identical to the first answer (answer = the message content); every request must be answered.
# Writes <run dir>/g0/: this script (g0.sh), prompt.system.txt, the raw requests, transport records and
# responses (rK.*, as run.sh writes rounds), verdict.txt (identity, seeds, derived counts, last line
# G0 PASS or G0 FAIL) and SHA256SUMS. Exit 0 on PASS, 1 on FAIL. run.sh refuses to generate, and score.sh
# refuses a run, without a G0 PASS for the same configuration, server and PARAMS. Refuses to overwrite an
# existing g0/. EXP445_PARAMS names another PARAMS file (tests only).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
die() { echo "g0.sh: $*" >&2; exit 2; }
PARAMS="${EXP445_PARAMS:-$HERE/PARAMS}"
[ -f "$PARAMS" ] || die "no PARAMS file $PARAMS"
kv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$PARAMS"; }
command -v jq > /dev/null || die "jq is required"
command -v curl > /dev/null || die "curl is required"
CONFIG="" OUT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --config) CONFIG="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) die "unknown argument $1" ;;
  esac
done
[ -n "$CONFIG" ] && [ -n "$OUT" ] || die "usage: g0.sh --config A --out <run dir>"
[[ "$CONFIG" =~ ^[A-Za-z0-9]{1,8}$ ]] || die "config must be [A-Za-z0-9]{1,8}"
un="$(kv "config.$CONFIG.unavailable")"
[ -z "$un" ] || die "config $CONFIG is unavailable: $un"
MODEL="$(kv "config.$CONFIG.model")" BASE="$(kv "config.$CONFIG.base_url")" EXTRA="$(kv "config.$CONFIG.extra")" SERVER="$(kv "config.$CONFIG.server")"
[ -n "$MODEL" ] && [ -n "$BASE" ] && [ -n "$EXTRA" ] && [ -n "$SERVER" ] || die "config $CONFIG (model, base_url, extra, server) is not named in $PARAMS"
EXTRA="$(jq -c . <<< "$EXTRA")"
MAXT="$(kv max_tokens)" TEMP="$(kv temperature)" TOPP="$(kv top_p)" SB="$(kv seed_base)" TIMEOUT="$(kv timeout_s)" RETRIES="$(kv retries)"
TASK="$(kv g0_task)" N="$(kv g0_seeds)" MD="$(kv g0_min_distinct)" MODE="$(kv heldout_prompt_mode)"
[[ "$N" =~ ^[1-9][0-9]?$ ]] && [[ "$MD" =~ ^[1-9][0-9]?$ ]] && [ -n "$TASK" ] && [ -n "$MODE" ] || die "g0_task, g0_seeds, g0_min_distinct, heldout_prompt_mode missing in $PARAMS"
DEV="$ROOT/tests/one_layer/exp445/dev/tasks/cases"
[ -f "$DEV/$TASK/requirement" ] && [ -f "$DEV/$TASK/prompt" ] || die "no dev task $TASK"
TI="$(ls "$DEV" | LC_ALL=C sort | awk -v t="$TASK" '$0 == t { print NR }')"
. "$HERE/reader.sh"
mkdir -p "$OUT"; OUT="$(realpath -e "$OUT")"
D="$OUT/g0"
[ ! -e "$D" ] || die "$D exists: a G0 record is never overwritten (move it away to run G0 again)"
mkdir "$D.tmp.$$"; trap 'rm -rf "$D.tmp.$$"' EXIT
W="$D.tmp.$$"
cp "$HERE/g0.sh" "$W/g0.sh"
system_prompt "$HERE/arms" omegese "$MODE" > "$W/prompt.system.txt" || die "cannot build the omegese $MODE prompt"
msgs="$(first_msgs "$(cat "$W/prompt.system.txt")" "$(user_msg "$DEV/$TASK/requirement" "$DEV/$TASK/prompt")")"
seeds=""
for k in $(seq 1 $((N + 1))); do
  s="$(g0_seed "$SB" "$([ "$k" -le "$N" ] && echo "$k" || echo 1)" "$TI")"; seeds="$seeds${seeds:+ }$s"
  ask "$W" "$k" "$(request_json "$MODEL" "$msgs" "$MAXT" "$TEMP" "$TOPP" "$s" "$EXTRA")" || true
done
der="$(g0_derive "$W" "$N")"
dd="${der#distinct=}"; dd="${dd%%/*}"
if [ "$dd" -ge "$MD" ] && [[ "$der" == *"repeat_identical=yes answered=$((N + 1))/$((N + 1))" ]]; then v=PASS; else v=FAIL; fi
{
  echo "config=$CONFIG"; echo "model=$MODEL"; echo "base_url=$BASE"; echo "request_extra=$EXTRA"; echo "server=$SERVER"
  echo "params_sha256=$(sha256sum "$PARAMS" | cut -d' ' -f1)"; echo "commit=$(git -C "$ROOT" rev-parse HEAD 2> /dev/null || echo none)"
  echo "task=$TASK"; echo "arm=omegese"; echo "prompt_mode=$MODE"; echo "prompt_sha256=$(sha256sum < "$W/prompt.system.txt" | cut -d' ' -f1)"
  echo "max_tokens=$MAXT"; echo "temperature=$TEMP"; echo "top_p=$TOPP"; echo "seeds=$seeds"
  echo "rule=PASS needs distinct >= $MD of $N, the repeat (request $((N + 1))) identical to request 1, every request answered"
  echo "derived=$der"
  echo "G0 $v"
} > "$W/verdict.txt"
(cd "$W" && g0_files . | tr '\n' '\0' | xargs -0 sha256sum) > "$W/SHA256SUMS"
mv "$W" "$D"; trap - EXIT
echo "g0.sh: config $CONFIG: $der: G0 $v (record in $D)"
[ "$v" = PASS ]
