#!/bin/sh
# TY-4/TY-5 timed run driver (docs/turing/TURING_YIELD_ENERGY_PROTOCOL_V0.md).
#
#   ty_energy_run.sh all OUTDIR BINDIR      stage 1 sweep, selection, stage 2
#   ty_energy_run.sh fixture OUTDIR BINDIR  one short round (3 s windows) for test fixtures
#
# The rounds, the idle wait and the pause between windows are here, in shell,
# on purpose: the C tools hold no timed loop (R16 loop inventory scope is C and
# Rust). Waits for an idle machine, then creates its OWN quiet flag, and removes
# it only if it is still its own. Never kills anything. Run it detached.
set -u
case "${1:-}" in all|fixture) MODE=$1; OUT=$2; BIN=$3 ;; *) echo "usage: $0 all|fixture OUTDIR BINDIR" >&2; exit 64 ;; esac
# Serialize with the existing Spark hardware/qualification owner before any
# timed window. No other job is interrupted and no new lock namespace is used.
exec 9> "$HOME/workspace/.argus-bench.lock"
flock 9 || exit 1
QUIET=$HOME/workspace/.spark-quiet
TAG="ty45-energy pid $$"
WL=$BIN/ty_workload
WIN=$BIN/ty_energy_window
RED=$BIN/ty_energy_reduce
S1_SEED=0x7459510001
S2_SEED=0x7459520001

log() { echo "$(date -u +%FT%TZ) $*"; }

# Config table (protocol section 5): id A-spec B-spec
cfg_line() {
    case "$1" in
        S1) echo "R1_plain:5 R2c_crumb:6" ;;
        S2) echo "R1_plain:5 R2c_crumb:15" ;;
        S3) echo "R1_plain:5,6 R2c_crumb:7,8" ;;
        S4) echo "R1_plain:5,6 R2c_crumb:15,16" ;;
        S5) echo "R1_plain:5,6,7 R2c_crumb:8,9" ;;
        S6) echo "R1_plain:5,6,7,8,9 R2c_crumb:15,16,17,18,19" ;;
        *) return 1 ;;
    esac
}

busy_reason() {
    l1=$(cut -d' ' -f1 /proc/loadavg)
    if awk -v l="$l1" 'BEGIN{exit !(l >= 2)}'; then echo "load1=$l1"; return; fi
    if [ -e "$QUIET" ] && ! grep -q "$TAG" "$QUIET" 2>/dev/null; then echo "quiet flag held: $(head -c 120 "$QUIET")"; return; fi
    p=$(pgrep -a -x make 2>/dev/null | head -1)
    [ -n "$p" ] && { echo "make running: $p"; return; }
    p=$(pgrep -a -f 'bench_|_bench|qualify|rx_r1[0-9]_' 2>/dev/null | grep -v "ty_energy\|ty_workload\|pgrep" | head -1)
    [ -n "$p" ] && { echo "bench running: $p"; return; }
    echo ""
}

wait_idle() {
    n=0
    while :; do
        why=$(busy_reason)
        [ -z "$why" ] && return 0
        [ $((n % 15)) -eq 0 ] && log "waiting for idle: $why"
        n=$((n + 1))
        sleep 20
    done
}

take_flag() {
    wait_idle
    (set -C; printf '%s since %s: TY-5 timed energy windows, please hold heavy work\n' "$TAG" "$(date -u +%FT%TZ)" > "$QUIET") 2>/dev/null \
        || { log "quiet flag appeared while taking it; waiting again"; take_flag; return; }
    log "quiet flag taken"
    sleep 10   # let anything that just checked the flag finish its current step
}

drop_flag() {
    if [ -e "$QUIET" ] && grep -q "$TAG" "$QUIET" 2>/dev/null; then rm -f "$QUIET"; log "quiet flag removed"; fi
}
trap drop_flag EXIT
trap 'drop_flag; exit 130' INT TERM

manifest() {
    {
        echo "date_utc $(date -u +%FT%TZ)"
        echo "git_head $(git rev-parse HEAD 2>/dev/null)"
        echo "git_dirty $(git status --porcelain -- src tests tools mk 2>/dev/null | grep -c .)"
        for b in "$WL" "$WIN" "$RED"; do echo "bin $(sha256sum "$b")"; done
        echo "kernel $(uname -r)"
        echo "stage1_seed $S1_SEED (config order: --shuffle seed; round order: --plan seed+k, k = position of the config in S1..S6)"
        echo "stage2_seed $S2_SEED (contention case --plan seed, control case --plan seed+1)"
    } > "$OUT/manifest.txt"
}

# run_config STAGEDIR STAGE CFG SEED ROUNDS SECS
run_config() {
    dir=$1; st=$2; cfg=$3; seed=$4; rounds=$5; secs=$6
    set -- $(cfg_line "$cfg")
    sa=$1; sb=$2
    "$WIN" --plan "$seed" "$rounds" > "$dir/plan_$cfg.txt"
    while read -r r p c; do
        f=$(printf '%s/%s_%s_r%02d_p%d_%s.jsonl' "$dir" "$st" "$cfg" "$r" "$p" "$c")
        "$WIN" --out "$f" --run "$(basename "$OUT")-$st" --config "$cfg" --cond "$c" --round "$r" --pos "$p" \
            --secs "$secs" --workload "$WL" --a "$sa" --b "$sb" --tool-cpu 0 \
            || log "window $f returned nonzero (kept; the reducer decides)"
        grep -q "$TAG" "$QUIET" 2>/dev/null || log "WARNING: our quiet flag is gone during $f"
        sleep 3
    done < "$dir/plan_$cfg.txt"
}

sums() { (cd "$1" && sha256sum *.jsonl > SHA256SUMS); }

mkdir -p "$OUT"
manifest
if [ "$MODE" = fixture ]; then
    mkdir -p "$OUT/raw"
    take_flag
    run_config "$OUT/raw" fx S1 "$S1_SEED" 1 3
    sums "$OUT/raw"
    exit 0
fi

# ---- stage 1 ----
mkdir -p "$OUT/raw/stage1"
take_flag
log "stage 1 start"
"$WIN" --shuffle "$S1_SEED" 6 > "$OUT/raw/stage1/config_order.txt"
while read -r k; do
    cfg="S$((k + 1))"
    log "stage 1 config $cfg"
    run_config "$OUT/raw/stage1" s1 "$cfg" $((S1_SEED + k)) 3 10
done < "$OUT/raw/stage1/config_order.txt"
sums "$OUT/raw/stage1"
drop_flag
"$RED" --stage1 "$OUT/raw/stage1" "$OUT/stage1_summary.json" > "$OUT/stage1_report.txt" 2>&1
sel=$(grep '^SELECT' "$OUT/stage1_report.txt")
log "stage 1 done: $sel"
cont=$(echo "$sel" | sed -n 's/.*contention=\([^ ]*\).*/\1/p')
ctrl=$(echo "$sel" | sed -n 's/.*control=\([^ ]*\).*/\1/p')
cfg_line "$cont" > /dev/null || { log "no contention config selected; stop"; exit 1; }

# ---- stage 2 ----
mkdir -p "$OUT/raw/stage2"
take_flag
log "stage 2 start: contention=$cont control=$ctrl"
run_config "$OUT/raw/stage2" s2 "$cont" "$S2_SEED" 10 30
if [ -n "$ctrl" ] && [ "$ctrl" != "$cont" ] && cfg_line "$ctrl" > /dev/null; then
    run_config "$OUT/raw/stage2" s2 "$ctrl" $((S2_SEED + 1)) 10 30
fi
sums "$OUT/raw/stage2"
drop_flag
"$RED" --stage2 "$OUT/raw/stage2" "$OUT/stage2_summary.json" "$OUT/records.jsonl" > "$OUT/stage2_report.txt" 2>&1
log "stage 2 done"
grep '^CHECK\|^I \|^FIT' "$OUT/stage2_report.txt"
log "finished"
