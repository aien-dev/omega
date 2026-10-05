#!/bin/bash
# r15_qualify.sh -- the R15 qualification run (spec/r15-performance-proof.md
# §9, §10, §12, §13, §16 C1 item 7).
#
#   tools/r15_qualify.sh [--detach] <host|silicon> [rounds=12] [l1_runs=5] [l2_runs=5]
#   (env: AIENOS_R7_DIR, PHYSICS_DIR for the recorded commits; R15_OUT_BASE to
#   write a dry run outside evidence/)
#
# Runs, in order: W-EPISODE trials (rounds x RES-4/RES-1/SEQ/RES-1-NODIGEST,
# balanced Latin square), Level 1 (A,C,D,E,G,W0,W1 for RES-1 and SEQ
# interleaved, B for RES-1), Level 2 (RES-1/SEQ interleaved). Every process
# writes its own raw file; a failed process is recorded and NEVER rerun (§10).
# Then SHA256SUMS and the reducer. Output: evidence/R15/raw/<run-id>/.
#
# --detach re-launches this script in its own session (setsid nohup), so an
# agent or terminal going away cannot stop a long run; progress is written in
# plain words to <out>/progress.log.
#
# The only machine setting changed is kernel.perf_event_paranoid: 4 -> 1 for
# the run, restored to 4 on every exit path, and the run fails if it does not
# read back 4. No systemd.
set -u

if [ "${1:-}" = "--detach" ]; then
    shift
    setsid nohup "$0" "$@" </dev/null >/dev/null 2>&1 &
    disown
    echo "started in the background (pid $!)"
    exit 0
fi

MODE=${1:?host or silicon}
ROUNDS=${2:-12}
L1RUNS=${3:-5}
L2RUNS=${4:-5}
case "$MODE" in host|silicon) ;; *) echo "mode must be host or silicon" >&2; exit 64;; esac

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 1
COMMIT=$(git rev-parse HEAD)
DIRTY=$([ -z "$(git status --porcelain --untracked-files=no)" ] && echo false || echo true)
RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-${COMMIT:0:12}-$MODE
OUT=${R15_OUT_BASE:-$HERE/evidence/R15/raw}/$RUN_ID
mkdir -p "$OUT" || exit 1
LOG=$OUT/progress.log
say() { echo "$(date '+%H:%M:%S') $*" >> "$LOG"; }

B=$HERE/build/rx_r15_perf_$MODE
BND=$HERE/build/rx_r15_perf_${MODE}_nodigest
RED=$HERE/build/r15_reduce
for f in "$B" "$BND" "$RED"; do
    [ -x "$f" ] || { say "missing $f: build it first (make r15-perf-$MODE)"; exit 2; }
done

# ---- kernel.perf_event_paranoid 4 -> 1 -> 4 (C1 item 7) ---------------------
PARANOID_BEFORE=$(cat /proc/sys/kernel/perf_event_paranoid)
STATE=$HERE/tools/r15_machine_state.sh
restore() {
    "$STATE" stop "$OUT" 2>/dev/null
    sudo -n sysctl -q kernel.perf_event_paranoid=4 >/dev/null 2>&1
    PARANOID_RESTORED=$(cat /proc/sys/kernel/perf_event_paranoid)
    echo "$PARANOID_RESTORED" > "$OUT/paranoid-restored.txt"
    if [ "$PARANOID_RESTORED" != 4 ]; then say "WARNING: perf_event_paranoid did not return to 4 (reads $PARANOID_RESTORED)"; fi
}
trap restore EXIT
trap 'say "stopped by a signal"; exit 130' INT TERM HUP
sudo -n sysctl -q kernel.perf_event_paranoid=1 >/dev/null 2>&1
PARANOID_DURING=$(cat /proc/sys/kernel/perf_event_paranoid)

# ---- machine.json (§13) ------------------------------------------------------
jstr() { printf '"%s"' "$(printf '%s' "$1" | tr -d '"\\' | tr '\n' ' ')"; }
midrs=$(for c in /sys/devices/system/cpu/cpu[0-9]*; do
    printf '%s:%s ' "${c##*cpu}" "$(cat "$c/regs/identification/midr_el1" 2>/dev/null)"; done)
govs=$(for c in /sys/devices/system/cpu/cpu[0-9]*; do
    printf '%s:%s:%s ' "${c##*cpu}" "$(cat "$c/cpufreq/scaling_governor" 2>/dev/null)" \
        "$(cat "$c/cpufreq/scaling_cur_freq" 2>/dev/null)"; done)
temps=$(cat /sys/class/thermal/thermal_zone*/temp 2>/dev/null | tr '\n' ' ')
spbm_ver=$(cat /sys/module/aien_spbm_readonly/srcversion 2>/dev/null)
top=$(ps -eo pcpu,comm --sort=-pcpu | sed -n 2,11p | awk '{printf "%s:%s ", $2, $1}')
servers=$(ps -eo comm,args | grep -i -E 'astrosage|lm-studio|lmstudio|llama|vllm|ollama' | grep -v grep | awk '{print $1}' | sort -u | tr '\n' ' ')
aienos=$(git -C "${AIENOS_R7_DIR:-../aienos-capability-c}" rev-parse HEAD 2>/dev/null)
physics=$(git -C "${PHYSICS_DIR:-../physics}" rev-parse HEAD 2>/dev/null)
{
    printf '{"run_id":%s,"mode":%s,"candidate_commit":%s,"tree_dirty":%s,' \
        "$(jstr "$RUN_ID")" "$(jstr "$MODE")" "$(jstr "$COMMIT")" "$DIRTY"
    printf '"hostname":%s,"machine_id_sha256":%s,"kernel":%s,' \
        "$(jstr "$(hostname)")" "$(jstr "$(sha256sum /etc/machine-id | cut -d' ' -f1)")" "$(jstr "$(uname -r)")"
    printf '"midr":%s,"governor_freq":%s,"boost":%s,"mem_kb":%s,' \
        "$(jstr "$midrs")" "$(jstr "$govs")" "$(jstr "$(cat /sys/devices/system/cpu/cpufreq/boost 2>/dev/null)")" \
        "$(jstr "$(awk '/MemTotal/{print $2}' /proc/meminfo)")"
    printf '"gpu":%s,"secure_boot":%s,"lockdown":%s,' \
        "$(jstr "$(nvidia-smi --query-gpu=name,pci.bus_id,driver_version,temperature.gpu --format=csv,noheader 2>/dev/null)")" \
        "$(jstr "$(mokutil --sb-state 2>/dev/null)")" "$(jstr "$(cat /sys/kernel/security/lockdown 2>/dev/null)")"
    printf '"perf_event_paranoid":{"original":%s,"qualification":%s,"restored":"see paranoid-restored.txt"},' \
        "$PARANOID_BEFORE" "$PARANOID_DURING"
    printf '"thermal_mc":%s,"spbm_reader_srcversion":%s,"loadavg":%s,"top_cpu":%s,"model_servers":%s,' \
        "$(jstr "$temps")" "$(jstr "$spbm_ver")" "$(jstr "$(cat /proc/loadavg)")" "$(jstr "$top")" "$(jstr "$servers")"
    printf '"aienos_commit":%s,"physics_commit":%s,"compiler":%s,' \
        "$(jstr "$aienos")" "$(jstr "$physics")" \
        "$(jstr "$(gcc --version | head -1)")"
    printf '"binaries":{"trial":%s,"nodigest":%s,"reducer":%s},' \
        "$(jstr "$(sha256sum "$B" | cut -d' ' -f1)")" "$(jstr "$(sha256sum "$BND" | cut -d' ' -f1)")" \
        "$(jstr "$(sha256sum "$RED" | cut -d' ' -f1)")"
    printf '"reducer_source_sha256":%s,"rounds":%s,"l1_runs":%s,"l2_runs":%s}\n' \
        "$(jstr "$(sha256sum tools/r15_reduce.c | cut -d' ' -f1)")" "$ROUNDS" "$L1RUNS" "$L2RUNS"
} > "$OUT/machine.json"
ps -eo pid,pcpu,rss,comm --sort=-pcpu > "$OUT/processes-before.txt"
[ "$PARANOID_DURING" = 1 ] || say "WARNING: could not set perf_event_paranoid to 1 (reads $PARANOID_DURING); memory-traffic counters will be missing"
[ -n "$servers" ] && say "WARNING: a model server looks resident: $servers (spec §9 says nothing heavy may run)"
[ "$DIRTY" = true ] && say "NOTE: the working tree has uncommitted changes, so this run is not candidate-bound"

# ---- instrument preflight (spec §7, §17 C2) ----------------------------------
# Package energy is a mandatory metric (17) and G9's input. If the signed SPBM
# reader is absent or returns invalid samples, the run cannot PASS, so stop
# before consuming an acceptance window. This is BLOCKED_INSTRUMENT, not a
# measured FAIL: no trial runs, no summary and no receipt is written.
if [ "$MODE" = silicon ]; then
    if ! ENERGY_MSG=$("$STATE" energy-preflight); then
        say "BLOCKED_INSTRUMENT: $ENERGY_MSG"
        printf 'BLOCKED_INSTRUMENT\n%s\n' "$ENERGY_MSG" > "$OUT/instrument-unavailable.txt"
        echo instrument-unavailable > "$OUT/done"
        exit 4
    fi
    say "$ENERGY_MSG"
fi

# ---- machine physical state (§18 C3, observability only) --------------------
# The preflight stops the run before any trial if the X925 does not sustain
# its clock (the power-limited state seen on 2026-09-28). After that nothing
# stops, drops or reruns a trial; the state is only recorded.
if [ "$MODE" = silicon ]; then
    say "machine-state preflight: 20 s sustained on one X925 core"
    if ! "$STATE" preflight "$OUT"; then
        say "MACHINE-STATE FAILURE: the X925 did not sustain its clock (preflight.txt); stopped before any trial"
        echo machine-state-failure > "$OUT/done"
        exit 3
    fi
    say "machine-state preflight ok: $(grep median "$OUT/preflight.txt")"
fi
"$STATE" start "$OUT"

say "R15 $MODE run $RUN_ID started: $ROUNDS rounds, $L1RUNS Level-1 runs, $L2RUNS Level-2 runs"
FAILED=0
one() {   # <label> <file> <binary> <args...>
    local label=$1 file=$2 bin=$3
    shift 3
    local t0=$SECONDS
    "$STATE" mark "$OUT" "begin $file"
    "$bin" "$@" "$OUT/$file" >> "$OUT/stderr.log" 2>&1
    local rc=$?
    "$STATE" mark "$OUT" "end $file exit $rc"
    echo "$file exit $rc seconds $((SECONDS - t0))" >> "$OUT/runs.txt"
    if [ $rc -ne 0 ]; then FAILED=$((FAILED + 1)); say "$label FAILED (exit $rc); recorded, not rerun"; else say "$label ok ($((SECONDS - t0)) s)"; fi
}

# ---- W-EPISODE: balanced Latin square over 4 configurations (§10) ----------
SQ=("0 1 3 2" "1 2 0 3" "2 3 1 0" "3 0 2 1")
NAMES=(RES4 RES1 SEQ ND)
for r in $(seq 1 "$ROUNDS"); do
    for i in ${SQ[$(( (r - 1) % 4 ))]}; do
        c=${NAMES[$i]}
        if [ "$c" = ND ]; then
            one "round $r RES-1-NODIGEST trial" "trial-RES1ND-$(printf %02d "$r").jsonl" "$BND" trial RES1 "$RUN_ID" "$r"
        else
            one "round $r $c trial" "trial-$c-$(printf %02d "$r").jsonl" "$B" trial "$c" "$RUN_ID" "$r"
        fi
    done
done

# ---- Level 1 ------------------------------------------------------------------
for k in $(seq 1 "$L1RUNS"); do
    for m in A C D E G W0 W1; do
        if [ $((k % 2)) = 1 ]; then order="RES1 SEQ"; else order="SEQ RES1"; fi
        for c in $order; do
            one "Level 1 $m $c run $k" "l1-$m-$c-$k.jsonl" "$B" l1 "$m" "$c" "$RUN_ID" "$k"
        done
    done
    one "Level 1 B RES1 run $k" "l1-B-RES1-$k.jsonl" "$B" l1 B RES1 "$RUN_ID" "$k"
done

# ---- Level 2 ------------------------------------------------------------------
for k in $(seq 1 "$L2RUNS"); do
    if [ $((k % 2)) = 1 ]; then order="RES1 SEQ"; else order="SEQ RES1"; fi
    for c in $order; do one "Level 2 $c run $k" "l2-$c-$k.jsonl" "$B" l2 "$c" "$RUN_ID" "$k"; done
done

ps -eo pid,pcpu,rss,comm --sort=-pcpu > "$OUT/processes-after.txt"
restore
trap - EXIT
(cd "$OUT" && sha256sum ./*.jsonl machine.json machine-state.ndjson machine-perf.csv machine-state-marks.txt $([ -f preflight.txt ] && echo preflight.txt) 2>/dev/null | sed 's| \./| |' > SHA256SUMS)
"$RED" "$OUT" "$OUT/summary.json" > "$OUT/reduce.log" 2>&1
say "finished: $FAILED process(es) failed; $(grep -c PASS "$OUT/reduce.log") of 16 gates pass (see summary.json)"
tail -1 "$OUT/reduce.log" >> "$LOG"
echo done > "$OUT/done"
