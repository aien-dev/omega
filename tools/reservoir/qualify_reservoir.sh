#!/bin/bash
# qualify_reservoir.sh -- omega#440 Gate 2 receipt for the ionic-droplet reservoir
# (docs/reservoir/RESERVOIR_REFERENCE.md sections 7, 7.1, 8, 10, 10.1).
#
#   RES_ARCHIVE=<dir> [RES_V2_RESULTS=<file>] tools/reservoir/qualify_reservoir.sh [out-dir]
#   make qualify-reservoir RES_ARCHIVE=<dir> [RES_V2_RESULTS=<file>]
#
# RES_ARCHIVE is the unpacked project-zenodo-ready_with-paper/ directory of Zenodo
# 10.5281/zenodo.17296788 (every file used is sha256-pinned by the tool); RES_V2_RESULTS
# the version 2 HyperParameterOptimizationVariablesToPlot.mat (sha256-pinned; without it
# case N1b-v2 is NOT_RUN). The archive is data: read and hashed, never executed.
#
# On the committed tree only: make test-reservoir (wrapped in `flock $BENCH_LOCK`), then
# res_qualify: pins; G2-N1 cases N1a, N1b, N1b-v2 and control NC-N; G2-L1 on 13, 25 and
# 43 nodes and control NC-L; the NARMA and Lorenz benchmarks; and, when the quiet flag
# is clear, the timing run (under the lock). The receipt is a NEW file named by the
# SHA-256 of its own bytes; nothing under evidence/ is rewritten. Its status is FAIL
# (with reasons) when the tree is dirty or changes during the run, a pin mismatches,
# the build or tests fail, or a command errors. A comparison case that FAILs is a
# recorded result, not a receipt failure. Shell and coreutils only (git, sha256sum,
# grep, sed, awk, make, flock; jq only to check the JSON when present). No Python.
set -u

HERE=$(cd "$(dirname "$0")/../.." && pwd)
cd "$HERE" || exit 2
ARCHIVE=${RES_ARCHIVE:-}
V2=${RES_V2_RESULTS:-}
OUTDIR=${1:-evidence/RESERVOIR}
BENCH_LOCK=${BENCH_LOCK:-$HOME/workspace/.argus-bench.lock}
LANES=${LANES:-$HOME/.claude/skills/orchestrate-lanes/lanes.sh}
Q=build/resq
REASONS=()
if [ -z "$ARCHIVE" ] || [ ! -d "$ARCHIVE" ]; then
    echo "qualify_reservoir: RES_ARCHIVE must name the unpacked archive directory" >&2
    exit 2
fi
mkdir -p "$(dirname "$BENCH_LOCK")" || exit 2

head0=$(git rev-parse HEAD)
tree0=$(git rev-parse 'HEAD^{tree}')
dirty0=$(git status --porcelain)
[ -z "$dirty0" ] || REASONS+=("tree dirty at start")
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
rm -rf "$Q" && mkdir -p "$Q/r"

json_str() { printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/ /g' | awk 'BEGIN{ORS=""} {if (NR>1) print "\\n"; print}'; }

# ---- build and unit tests ------------------------------------------------------
build_rc=0
flock "$BENCH_LOCK" make OUT_DIR="$Q/out" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-reservoir \
    > "$Q/test-reservoir.log" 2>&1 || build_rc=$?
[ "$build_rc" -eq 0 ] || REASONS+=("make test-reservoir failed (rc $build_rc)")
T=$Q/out/tests-reservoir/res_qualify
tests_summary=$(grep -E '^test_res(ervoir|_mat): [0-9]+ checks' "$Q/test-reservoir.log" | tr '\n' ';')

run() { # run <name> <res_qualify args...>: stdout JSON, stderr and exit code kept
    local name=$1
    shift
    "$T" "$@" > "$Q/r/$name.json" 2> "$Q/r/$name.err"
    echo $? > "$Q/r/$name.rc"
}

CASES="g2-narma-N1a g2-narma-N1b g2-narma-N1b-v2 g2-narma-NC-N g2-lorenz-13 g2-lorenz-25 g2-lorenz-43 g2-lorenz-13-NC-L"
if [ "$build_rc" -eq 0 ]; then
    run pins pins "$ARCHIVE"
    [ "$(cat "$Q/r/pins.rc")" = 0 ] || REASONS+=("archive pin mismatch (see results.pins)")
    # the comparisons are independent processes and untimed: run them side by side
    run g2-narma-N1a g2-narma "$ARCHIVE" N1a &
    run g2-narma-N1b g2-narma "$ARCHIVE" N1b &
    run g2-narma-NC-N g2-narma "$ARCHIVE" NC-N &
    if [ -n "$V2" ]; then run g2-narma-N1b-v2 g2-narma "$ARCHIVE" N1b-v2 "$V2" & fi
    run g2-lorenz-13 g2-lorenz "$ARCHIVE" 13 &
    run g2-lorenz-25 g2-lorenz "$ARCHIVE" 25 &
    run g2-lorenz-43 g2-lorenz "$ARCHIVE" 43 &
    run g2-lorenz-13-NC-L g2-lorenz "$ARCHIVE" 13 NC-L &
    wait
    run bench-narma bench-narma
    run bench-lorenz bench-lorenz
fi

# ---- timing (task e): only on a quiet machine ------------------------------------
quiet="not checked (no lanes.sh on this host)"
perf_state=NOT_RUN
perf_reason="build failed"
if [ "$build_rc" -eq 0 ]; then
    idle=1
    if [ -x "$LANES" ]; then
        nurse=$("$LANES" nurse 2>&1)
        quiet=$(printf '%s\n' "$nurse" | grep -m1 -i 'quiet flag' | sed 's/^ *//')
        quiet="$quiet; $(printf '%s\n' "$nurse" | grep -m1 -i 'forge:' | sed 's/^ *//')"
        printf '%s\n' "$nurse" | grep -qi 'quiet flag: *clear' || idle=0
        printf '%s\n' "$nurse" | grep -i 'forge:' | grep -qi 'not running' || idle=0
    fi
    if [ "$idle" -eq 0 ]; then
        perf_reason="machine not quiet (a GB10 job or the quiet flag): $quiet"
    else
        loadavg_before=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null)
        flock "$BENCH_LOCK" "$T" perf > "$Q/r/perf.json" 2> "$Q/r/perf.err"
        echo $? > "$Q/r/perf.rc"
        perf_state=RUN
        perf_reason="load average before: ${loadavg_before:-unknown}"
    fi
fi

# ---- collect --------------------------------------------------------------------
verdict_of() { grep -o '"verdict": "[A-Z_]*"' "$1" 2>/dev/null | tail -1 | sed 's/.*: "\(.*\)"/\1/'; }
results=""
summary=""
for name in pins $CASES bench-narma bench-lorenz perf; do
    f=$Q/r/$name.json
    if [ ! -f "$Q/r/$name.rc" ]; then
        why="not run"
        [ "$name" = g2-narma-N1b-v2 ] && [ -z "$V2" ] && why="NOT_RUN: RES_V2_RESULTS not given"
        [ "$name" = perf ] && why="NOT_RUN: $perf_reason"
        results+="${results:+,}
    \"$name\": {\"status\": \"$(json_str "$why")\"}"
        summary+="${summary:+, }\"$name\": \"$(json_str "$why")\""
        continue
    fi
    rc=$(cat "$Q/r/$name.rc")
    [ "$rc" -le 1 ] || REASONS+=("$name exited $rc: $(head -c 300 "$Q/r/$name.err")")
    if [ "$rc" -le 1 ] && [ -s "$f" ]; then
        results+="${results:+,}
    \"$name\": $(cat "$f")"
    else
        results+="${results:+,}
    \"$name\": {\"status\": \"ERROR\", \"rc\": $rc, \"stderr\": \"$(json_str "$(head -c 2000 "$Q/r/$name.err")")\"}"
    fi
    v=$(verdict_of "$f")
    [ -n "$v" ] || { [ "$rc" = 0 ] && v=RAN || v="ERROR rc $rc"; }
    summary+="${summary:+, }\"$name\": \"$v\""
done
# negative controls must fail; a passing control means the contract cannot see that error
for c in g2-narma-NC-N g2-lorenz-13-NC-L; do
    v=$(verdict_of "$Q/r/$c.json")
    [ -z "$v" ] || [ "$v" = FAIL ] || REASONS+=("negative control $c did not fail ($v)")
done

head1=$(git rev-parse HEAD)
dirty1=$(git status --porcelain)
stable=true
{ [ "$head1" = "$head0" ] && [ "$dirty1" = "$dirty0" ]; } || { stable=false; REASONS+=("tree changed during the run"); }
tree_dirty=false
[ -z "$dirty0" ] || tree_dirty=true
status=RECORDED
[ "${#REASONS[@]}" -eq 0 ] || status=FAIL
reasons_json=""
for r in "${REASONS[@]}"; do reasons_json+="${reasons_json:+, }\"$(json_str "$r")\""; done

body=$(cat <<EOF
{
  "schema": "omega-reservoir-qualification/1",
  "issue": "aien-spark/omega#440, Gate 2",
  "doc": "docs/reservoir/RESERVOIR_REFERENCE.md sections 7, 7.1, 8, 10, 10.1",
  "config": "docs/reservoir/gate2_config.json (test-reservoir checks the tool still emits it)",
  "started_utc": "$started",
  "finished_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "candidate_commit": "$head0",
  "candidate_tree": "$tree0",
  "tree_dirty": $tree_dirty,
  "tree_stable_during_run": $stable,
  "host": {"uname": "$(json_str "$(uname -srm)")", "nproc": $(nproc), "product_name": "$(json_str "$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)")"},
  "compiler": "$(json_str "$(${CC:-cc} --version 2>/dev/null | head -1)")",
  "unit_tests": "$(json_str "$tests_summary")",
  "unit_tests_log_sha256": "$(sha256sum "$Q/test-reservoir.log" | awk '{print $1}')",
  "archive": {"record": "10.5281/zenodo.17296788", "license": "CC-BY-4.0", "pins": "results.pins", "v2_results_file_given": $([ -n "$V2" ] && echo true || echo false)},
  "quiet_flag": "$(json_str "$quiet")",
  "timing": {"state": "$perf_state", "note": "$(json_str "$perf_reason")"},
  "hardware_scope": "CPU only; no GPU opened, no silicon, no energy or power measurement",
  "summary": {$summary},
  "blocked": [
    "63 of the 64 archived NARMA grid cells: their input masks come from MATLAB's rng/randi stream, which is not reimplemented (no MATLAB or Octave here)",
    "state-trace comparison against the authors' code: the version 1 archive holds no state trajectories",
    "whether the archived outputs came from the archived code (discrepancy 15): needs MATLAB",
    "the paper's parallel reservoir (S5): a different, voltage-clamped model, not implemented"
  ],
  "not_run": [
    "energy and power: not instrumented, no efficiency claim",
    "AIEN telemetry workload: no real telemetry with a time-ordered train/validation/test split exists (section 11)",
    "RC with squared states on the 50-node NARMA chain: 295 features, above the readout bound of 256"
  ],
  "proves": [
    "For the cases whose verdict is PASS: numerical agreement, under the tolerances declared before the runs, between Omega's binary32 simulator with its binary64 readout and the outputs the authors archived from their MATLAB code (targets, predictions and NRMSE), on the archived inputs.",
    "For the cases whose verdict is FAIL: that Omega does not meet the declared criteria there, with the diagnostics recorded in the case.",
    "The benchmark and timing numbers below, for this commit, compiler and host."
  ],
  "does_not_prove": [
    "physical validity of the droplet model, or anything about real droplets or devices",
    "agreement on the BLOCKED cases, or with the authors' code beyond its archived outputs",
    "GPU qualification of any kind (CPU only)",
    "energy, power or efficiency (NOT_RUN)",
    "production readiness"
  ],
  "status": "$status",
  "status_reasons": [$reasons_json],
  "results": {$results
  }
}
EOF
)
mkdir -p "$OUTDIR"
tmp=$(mktemp "$OUTDIR/.tmp-XXXXXX")
printf '%s\n' "$body" > "$tmp"
if command -v jq > /dev/null 2>&1 && ! jq -e . "$tmp" > /dev/null 2>&1; then
    echo "qualify_reservoir: receipt is not valid JSON; kept at $tmp" >&2
    exit 2
fi
digest=$(sha256sum "$tmp" | awk '{print $1}')
final=$OUTDIR/$digest.json
if [ -e "$final" ]; then rm -f "$tmp"; else mv "$tmp" "$final"; fi
chmod 0644 "$final"
echo "SUMMARY {$summary}"
echo "RECEIPT $final"
echo "STATUS $status"
[ "$status" = RECORDED ]
