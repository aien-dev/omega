#!/bin/bash
# m19r_campaign.sh -- mechanical verdict for a multi-run M19R qualification
# campaign.
#
#   tools/m19r_campaign.sh --campaign-dir D --runs N
#
# D holds one run directory per qualification run (each written by
# tools/m19r_qualify.sh --evidence-root D). Writes D/campaign.json at first
# use (sha, rule, created_at) and prints + writes D/final-verdict.json.
#
# Rule, evaluated in this order:
#   INVALID    any run directory lacks run.json or verdict.json, lacks
#              hashes.sha256, or fails `sha256sum -c hashes.sha256`
#              (also: extra files not covered by hashes are NOT detected,
#              only altered or missing hashed files).
#   FAIL       any run whose status is not PASS (a failed mandatory gate or
#              step; QUICK_PASS is not a full PASS).
#   INCOMPLETE fewer than N usable (verified) run directories.
#   PASS       at least N usable runs, all with status PASS.
# Exit 0 only on PASS; 1 on any other verdict; 2 on bad arguments.
# Shell + coreutils + git + jq. No Python.

campaign_rule='PASS = N completed runs all status PASS; FAIL = any completed run not PASS; INCOMPLETE = fewer than N usable run dirs; INVALID = any run dir missing run.json/verdict.json/hashes.sha256 or failing sha256sum -c (checked first)'

campaign_main() {
    local dir= n= d id status invalid=() failed=() usable=0 verdict here sha
    while [ $# -gt 0 ]; do
        case $1 in
            --campaign-dir) dir=${2:-}; shift;;
            --campaign-dir=*) dir=${1#*=};;
            --runs) n=${2:-}; shift;;
            --runs=*) n=${1#*=};;
            *) echo "usage: m19r_campaign.sh --campaign-dir D --runs N" >&2; return 2;;
        esac
        shift
    done
    if [ -z "$dir" ] || [ ! -d "$dir" ] || ! [[ ${n:-} =~ ^[1-9][0-9]*$ ]]; then
        echo "usage: m19r_campaign.sh --campaign-dir D --runs N (D must exist, N >= 1)" >&2
        return 2
    fi
    here=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
    if [ ! -e "$dir/campaign.json" ]; then
        sha=$(git -C "$here" rev-parse HEAD 2> /dev/null || echo unknown)
        jq -n --arg sha "$sha" --arg rule "$campaign_rule" --argjson runs "$n" \
            --arg at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
            '{sha: $sha, rule: $rule, runs_required: $runs, created_at: $at}' > "$dir/campaign.json"
    fi
    for d in "$dir"/*/; do
        [ -d "$d" ] || continue
        id=$(basename "$d")
        if [ ! -f "$d/run.json" ] || [ ! -f "$d/verdict.json" ] || [ ! -f "$d/hashes.sha256" ]; then
            invalid+=("$id: missing run.json, verdict.json or hashes.sha256"); continue
        fi
        if ! (cd "$d" && sha256sum --quiet -c hashes.sha256 > /dev/null 2>&1); then
            invalid+=("$id: hashes.sha256 does not verify"); continue
        fi
        status=$(jq -r '.status' "$d/verdict.json" 2> /dev/null)
        if [ -z "$status" ] || [ "$status" = null ]; then
            invalid+=("$id: verdict.json unreadable"); continue
        fi
        usable=$((usable + 1))
        [ "$status" = PASS ] || failed+=("$id: $status")
    done
    if [ ${#invalid[@]} -gt 0 ]; then verdict=INVALID
    elif [ ${#failed[@]} -gt 0 ]; then verdict=FAIL
    elif [ "$usable" -lt "$n" ]; then verdict=INCOMPLETE
    else verdict=PASS; fi
    jq -n --arg v "$verdict" --argjson n "$n" --argjson usable "$usable" \
        --argjson invalid "$(printf '%s\n' "${invalid[@]}" | jq -Rn '[inputs | select(length > 0)]')" \
        --argjson failed "$(printf '%s\n' "${failed[@]}" | jq -Rn '[inputs | select(length > 0)]')" \
        '{verdict: $v, runs_required: $n, usable_runs: $usable, invalid: $invalid, failed: $failed}' > "$dir/final-verdict.json"
    cat "$dir/final-verdict.json"
    [ "$verdict" = PASS ]
}

campaign_main "$@"
