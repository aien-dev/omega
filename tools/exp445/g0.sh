#!/usr/bin/env bash
# tools/exp445/g0.sh --config A --out <run dir> -- G0, the sampling gate of protocol r3 (omega#445,
# docs/one_layer/EXP445_PROTOCOL.md section 13.4), run before every stage and per configuration.
# Wraps the stage S sampling gate of aien-sovereign-core (scripts/sampling-gate.sh at f881710a, vendored
# byte for byte as tools/exp445/sampling-gate.sh; its sha256 is frozen in PARAMS g0_gate_sha256) and adds
# one exp445 check:
#   1 the 8 exp445 requests (the dev task g0_task in the omegese arm, many-shot prompt, temperature and top_p
#     from PARAMS, seeds seed_of(seed_base, k, task index) for k = 1 .. g0_seeds: the seeds of that task's
#     attempts) are the gate's repro requests (its gate d): the gate re-sends each and passes when the
#     replies differ or when identical replies are plausible from the server's reported confidence
#     (p_repeat^(k-1) >= 0.01), with every reply reporting mode=sampled and its own seed; its gates a, b, c
#     (replay, spread, greedy) run on its own four prompts
#   2 one repeat of the first seed, sent by this script (reader.sh ask, with a transport record)
# PASS when the sampling gate passes (VERDICT PASS), the repeat is byte-identical to the gate's reply to
# the first seed, and all g0_seeds + 1 replies report mode=sampled with their own seed. The number of
# distinct answers is recorded, not gated.
# Writes <run dir>/g0/: g0.sh and sampling-gate.sh (the scripts that made it), prompt.system.txt,
# rK.req.json (K = 1 .. g0_seeds + 1), gate/ (the gate's output, replies kept: REDACT_REPRO=0, the dev task
# is public), r<g0_seeds+1>.transport and .resp.json, verdict.txt (identity, seeds, derived values, last
# line G0 PASS or G0 FAIL) and SHA256SUMS. Exit 0 on PASS, 1 on FAIL. run.sh refuses to generate, and
# score.sh refuses a run, without a G0 PASS for the same configuration, server and PARAMS (reader.sh
# g0_check re-derives it). A G0 record is never overwritten: after a FAIL nothing runs in that run dir
# (13.4). A transport failure (the gate exits 2 or runs out of time, or the repeat gets no response) writes
# no record and exits 2: it says nothing about the sampler. Every verdict is appended to the G0 ledger (reader.sh
# g0_ledger) before the record is moved into place; with a FAIL there for this configuration's identity (reader.sh
# g0_ident: its model, endpoint, request extra, server, sampling settings and G0 settings), g0.sh refuses with exit 3
# (not a transport failure: do not retry), and run.sh and score.sh refuse, in any run dir. EXP445_PARAMS names another PARAMS file (tests only).
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
[[ "$SERVER" != *$'\t'* ]] || die "config.$CONFIG.server must not contain a tab (it is a G0 ledger column)"
EXTRA="$(jq -c . <<< "$EXTRA")" || die "config.$CONFIG.extra is not JSON"
MAXT="$(kv max_tokens)" TEMP="$(kv temperature)" TOPP="$(kv top_p)" SB="$(kv seed_base)" TIMEOUT="$(kv timeout_s)" RETRIES="$(kv retries)"
TASK="$(kv g0_task)" N="$(kv g0_seeds)" MODE="$(kv heldout_prompt_mode)" GSHA="$(kv g0_gate_sha256)"
[[ "$TIMEOUT" =~ ^[1-9][0-9]*$ ]] || die "timeout_s must be a positive integer in $PARAMS"
[[ "$N" =~ ^[1-9][0-9]?$ ]] && [ -n "$TASK" ] && [ -n "$MODE" ] && [ -n "$GSHA" ] || die "g0_task, g0_seeds, g0_gate_sha256, heldout_prompt_mode missing in $PARAMS"
GATE="$HERE/sampling-gate.sh"
[ "$(sha256sum < "$GATE" | cut -d' ' -f1)" = "$GSHA" ] || die "$GATE is not the frozen sampling gate (sha256 differs from PARAMS g0_gate_sha256)"
DEV="$ROOT/tests/one_layer/exp445/dev/tasks/cases"
[ -f "$DEV/$TASK/requirement" ] && [ -f "$DEV/$TASK/prompt" ] || die "no dev task $TASK"
TI="$(ls "$DEV" | LC_ALL=C sort | awk -v t="$TASK" '$0 == t { print NR }')"
. "$HERE/reader.sh"
PSHA="$(sha256sum "$PARAMS" | cut -d" " -f1)" IDENT="$(g0_ident "$PARAMS" "$CONFIG")"
fl="$(g0_failed "$CONFIG" "$IDENT")"
if [ -n "$fl" ]; then
  echo "g0.sh: a G0 FAIL for config $CONFIG on this server and settings is in $(g0_ledger) ($(cut -f1,5,6 <<< "$fl" | tr "\t" " ")): nothing in that configuration runs (protocol 13.4)" >&2
  exit 3
fi
mkdir -p "$OUT"; OUT="$(realpath -e "$OUT")"
D="$OUT/g0"
[ ! -e "$D" ] || die "$D exists: a G0 record is never overwritten; after a G0 FAIL nothing runs in this run dir (protocol 13.4)"
mkdir "$D.tmp.$$"; trap 'rm -rf "$D.tmp.$$"' EXIT
W="$D.tmp.$$"
cp "$HERE/g0.sh" "$GATE" "$W/"
system_prompt "$HERE/arms" omegese "$MODE" > "$W/prompt.system.txt" || die "cannot build the omegese $MODE prompt"
msgs="$(first_msgs "$(cat "$W/prompt.system.txt")" "$(user_msg "$DEV/$TASK/requirement" "$DEV/$TASK/prompt")")"
seeds=""
for k in $(seq 1 $((N + 1))); do
  s="$(g0_seed "$SB" "$([ "$k" -le "$N" ] && echo "$k" || echo 1)" "$TI")"; seeds="$seeds${seeds:+ }$s"
  printf '%s\n' "$(request_json "$MODEL" "$msgs" "$MAXT" "$TEMP" "$TOPP" "$s" "$EXTRA")" > "$W/r$k.req.json"
done
# 1 the stage S gate, with the N exp445 requests as its repro requests (exit 0 PASS, 1 FAIL, 2 transport).
# GREEDY_GOLDEN is not passed in (the gate runs the same way for every caller); the whole gate is bounded
# by timeout_s per request it sends (4 prompts x 7 + N), since its curl calls have no time limit
base="${BASE%/}"; base="${base%/v1}"
ge=0; (cd "$W" && timeout "$(( (28 + N) * TIMEOUT ))" env -u GREEDY_GOLDEN REDACT_REPRO=0 ./sampling-gate.sh "$base" "$MODEL" gate $(seq -f 'r%g.req.json' 1 "$N")) > "$W/gate.out" 2>&1 || ge=$?
# a transport failure (gate exit 2, or the time limit) says nothing about the sampler: no record is written
[ "$ge" = 0 ] || [ "$ge" = 1 ] || die "the sampling gate could not complete (exit $ge: $(tail -n 2 "$W/gate.out" | tr '\n' ' ')); no G0 record written, rerun when the server answers"
# 2 the repeat of the first seed
ask "$W" "$((N + 1))" "$(cat "$W/r$((N + 1)).req.json")" || true
round_ok "$W/r$((N + 1))" || die "the repeat of seed 1 got no response ($(tail -n 1 "$W/r$((N + 1)).transport" 2> /dev/null)); no G0 record written, rerun when the server answers"
der="$(g0_derive "$W" "$N")"
if g0_pass "$der" "$N"; then v=PASS; else v=FAIL; fi
{
  echo "config=$CONFIG"; echo "model=$MODEL"; echo "base_url=$BASE"; echo "request_extra=$EXTRA"; echo "server=$SERVER"
  echo "params_sha256=$PSHA";echo "commit=$(git -C "$ROOT" rev-parse HEAD 2> /dev/null || echo none)"
  echo "gate=$(kv g0_gate_source)"; echo "gate_sha256=$GSHA"; echo "gate_exit=$ge"
  echo "task=$TASK"; echo "arm=omegese"; echo "prompt_mode=$MODE"; echo "prompt_sha256=$(sha256sum < "$W/prompt.system.txt" | cut -d' ' -f1)"
  echo "max_tokens=$MAXT"; echo "temperature=$TEMP"; echo "top_p=$TOPP"; echo "seeds=$seeds"
  echo "rule=PASS needs the sampling gate VERDICT PASS (its gate d on the $N exp445 requests), the repeat (request $((N + 1))) byte-identical to the reply to request 1, every reply mode=sampled with its own seed; distinct answers are recorded, not gated"
  echo "derived=$der"
  echo "G0 $v"
} > "$W/verdict.txt"
(cd "$W" && g0_files . | tr '\n' '\0' | xargs -0 sha256sum) > "$W/SHA256SUMS"
# the ledger line first (13.4): a crash after it leaves a FAIL barred without a record, never a record unbarred
mkdir -p "$(dirname "$(g0_ledger)")"
printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$(date -u +%Y%m%dT%H%M%SZ)" "$CONFIG" "$SERVER" "$IDENT" "$v" "${D#"$ROOT/"}" \
  "$(sha256sum < "$W/SHA256SUMS" | cut -d" " -f1)" "$PSHA" >> "$(g0_ledger)"
mv "$W" "$D"; trap - EXIT
echo "g0.sh: config $CONFIG: $der: G0 $v (record in $D)"
[ "$v" = PASS ]
