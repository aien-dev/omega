#!/bin/bash
# chipwait_campaign.sh -- run an N-run M19R qualification campaign and print
# the mechanical verdict.
#
#   tools/chipwait_campaign.sh --omega-candidate SHA --physics-candidate SHA \
#       --physics-dir DIR --campaign-dir DIR [--runs N]      (N default 3)
#
# 1. Refuses (exit 2) if the campaign dir already holds campaign.json or any
#    run-00N directory. Nothing is ever overwritten or deleted.
# 2. Writes campaign.json FIRST (tools/m19r_campaign.sh only creates it when
#    absent, so this one is kept; its extra keys are not read by that script).
# 3. Runs tools/m19r_qualify.sh (override: env CHIPWAIT_QUALIFY) N times as
#    run-001..run-00N with --evidence-root <campaign-dir>. A failing run does
#    not stop the campaign. Each run's stdout/stderr are tee'd to
#    <campaign-dir>/runner-run-00<i>.{stdout,stderr}.log and its exit code is
#    written to runner-run-00<i>.exit (files beside, not inside, the run dir,
#    because m19r_qualify.sh refuses an existing run dir).
# 4. Calls tools/m19r_campaign.sh (override: env CHIPWAIT_CAMPAIGN). Exit 0
#    only if that verdict is PASS, else 1. Bad arguments: 2.
# Never touches the GPU itself; the qualify script does. Bash + coreutils + jq.

HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
QUALIFY=${CHIPWAIT_QUALIFY:-$HERE/m19r_qualify.sh}
CAMPAIGN=${CHIPWAIT_CAMPAIGN:-$HERE/m19r_campaign.sh}

usage() {
    echo "usage: chipwait_campaign.sh --omega-candidate SHA --physics-candidate SHA --physics-dir DIR --campaign-dir DIR [--runs N]" >&2
}
die() { echo "chipwait_campaign: $*" >&2; usage; exit 2; }

omega= physics= pdir= cdir= runs=3
while [ $# -gt 0 ]; do
    case $1 in
        --omega-candidate|--physics-candidate|--physics-dir|--campaign-dir|--runs)
            [ $# -ge 2 ] || die "missing value for $1"
            case $1 in
                --omega-candidate) omega=$2;;
                --physics-candidate) physics=$2;;
                --physics-dir) pdir=$2;;
                --campaign-dir) cdir=$2;;
                --runs) runs=$2;;
            esac
            shift 2;;
        --omega-candidate=*) omega=${1#*=}; shift;;
        --physics-candidate=*) physics=${1#*=}; shift;;
        --physics-dir=*) pdir=${1#*=}; shift;;
        --campaign-dir=*) cdir=${1#*=}; shift;;
        --runs=*) runs=${1#*=}; shift;;
        *) die "unknown argument: $1";;
    esac
done
[ -n "$omega" ] || die "--omega-candidate is required"
[ -n "$physics" ] || die "--physics-candidate is required"
[ -n "$pdir" ] || die "--physics-dir is required"
[ -n "$cdir" ] || die "--campaign-dir is required"
[[ $runs =~ ^[1-9][0-9]*$ ]] || die "--runs must be a positive integer"

if [ -e "$cdir/campaign.json" ] || compgen -G "$cdir/run-00*" > /dev/null; then
    echo "chipwait_campaign: $cdir already holds campaign.json or a run-00N entry; refusing to reuse it" >&2
    exit 2
fi
# Lane receipt guard (omega #315): m19r_qualify.sh runs `make clean`, which removes
# build/qual-runs. Refuse while an earlier window lane's receipt (WINDOW_LANE_OUT, the
# window directory a ladder keeps .lane-guard/ and R11-living/ in) or any R11 receipt in
# the build directory m19r_qualify.sh cleans is unarchived. Nothing is created first.
. "$HERE/window_lane_guard.sh" || exit 2
build_dir=${CHIPWAIT_BUILD_DIR:-$(realpath -m "$HERE/../${OUT_DIR:-build}")}
wl_guard_build "${WINDOW_LANE_OUT:-}" chipwait "$build_dir" || {
    echo "chipwait_campaign: lane receipt guard refused (see above); nothing was run" >&2; exit 2; }
mkdir -p "$cdir" || { echo "chipwait_campaign: cannot create $cdir" >&2; exit 2; }

runner_sha=$(git -C "$HERE" rev-parse HEAD 2> /dev/null || echo unknown)
jq -n --arg o "$omega" --arg p "$physics" --arg rule "$runs/$runs PASS, predeclared" \
    --argjson n "$runs" --arg at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    --arg host "$(hostname)" --arg un "$(uname -a)" --arg rs "$runner_sha" \
    '{omega_candidate_sha: $o, physics_candidate_sha: $p, rule: $rule, runs_required: $n,
      created_at: $at, host: $host, uname: $un, runner_sha: $rs}' > "$cdir/campaign.json" ||
    { echo "chipwait_campaign: cannot write campaign.json" >&2; exit 1; }

i=1
while [ "$i" -le "$runs" ]; do
    id=$(printf 'run-%03d' "$i")
    echo "== $id of $runs =="
    "$QUALIFY" --omega-candidate "$omega" --physics-candidate "$physics" --physics-dir "$pdir" \
        --evidence-root "$cdir" --run-id "$id" \
        > >(tee "$cdir/runner-$id.stdout.log") 2> >(tee "$cdir/runner-$id.stderr.log" >&2)
    rc=$?
    wait   # let both tee processes finish flushing
    echo "$rc" > "$cdir/runner-$id.exit"
    echo "== $id exit $rc =="
    i=$((i + 1))
done

echo "== mechanical verdict =="
"$CAMPAIGN" --campaign-dir "$cdir" --runs "$runs"
[ $? -eq 0 ] && exit 0
exit 1
