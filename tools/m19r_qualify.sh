#!/bin/bash
# m19r_qualify.sh -- M19R Gate 1/2 qualification. Permanent receipts are
# written only on success.
#
#   tools/m19r_qualify.sh --omega-candidate SHA --physics-candidate SHA
#                         [--physics-dir DIR] [--record] [--quick]
#
# Needs the GB10 (runs the NVRM/M16/M19 silicon suites and nvidia-smi) and
# takes /tmp/aien-gb10.lock for the whole build + test section.
#
# Checks, in order: both candidates are explicit full 40-hex SHAs equal to
# HEAD of a clean tree; physics.lock names the physics candidate; clean
# rebuild; physics suites (nvrm lifecycle, m16 requalify, m16 concurrent);
# omegatool suites (m15, world lifecycle, m19, and unless --quick the M19R
# soak); no libcuda linkage or CUDA symbols; hardware probe; evidence/ not
# changed by the run; every observed gate PASS; exactly 18 passing
# OMEGA_ACCEL_RESIDENT_ gates and a complete M19 observation; soak criteria
# (passed, >= 100000 cycles, bytes churned > 2x physical memory, snapshots
# start/end/post_destroy); candidates re-checked at the end.
#
# Output: <evidence-root>/<run-id>/run.json always (plus per-command logs,
# command.txt, environment.json, stdout.log, stderr.log, verdict.json and a
# final hashes.sha256); evidence root defaults to $OMEGA/qual-runs, outside
# build/ which `make clean` removes (--evidence-root DIR to change); the run
# directory must not exist yet (exit 2) and is never deleted by this script;
# receipt-preview.json on a full run; with --record the permanent receipt
# evidence/M19R/<receipt_digest>.json (created exclusively, mode 0444).
# Exit 0 on PASS or QUICK_PASS, 1 on failure, 2 on bad arguments.
#
# The receipt digest is SHA-256 over the canonical JSON of the receipt body
# (sorted keys, compact, Python json.dumps number/string forms), computed by
# tools/json_canon.c. Shell + coreutils + git + jq (field reads, string
# escaping) + that C helper. No Python.
#
# The functions below can be sourced (tools/test_m19r_qualify.sh does);
# main only runs when the file is executed.

M19R_OMEGA=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
M19R_PROG=$(basename "${BASH_SOURCE[0]}")
M19R_CUDA_RE='libcuda(rt)?\.so|\b(cuInit|cuCtx|cuMem|cuStream|cudaMalloc)\b'
M19R_ERR=

# Records the failure reason; also in a file so failures inside $(...) survive.
m19r_fail() {
    M19R_ERR=$1
    [ -z "${M19R_TMP:-}" ] || printf '%s' "$1" > "$M19R_TMP/error"
    return 1
}

# Build tools/json_canon.c into a private directory (not build/, which the
# qualification's `make clean` removes). Sets JSON_CANON.
m19r_build_canon() {
    M19R_TMP=$(mktemp -d "${TMPDIR:-/tmp}/m19r-qualify.XXXXXX") || return 1
    JSON_CANON=$M19R_TMP/json_canon
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$M19R_OMEGA/src" -o "$JSON_CANON" \
        "$M19R_OMEGA/tools/json_canon.c" "$M19R_OMEGA/src/sha256.c" -lm
}

# JSON string literal for arbitrary text.
m19r_jstr() { jq -n --arg s "$1" '$s'; }

# Strip leading/trailing whitespace (Python str.strip()).
m19r_strip() {
    local s=$1
    s=${s#"${s%%[![:space:]]*}"}
    s=${s%"${s##*[![:space:]]}"}
    printf '%s' "$s"
}

# m19r_cmd CWD LOG ARGS... -- run ARGS in CWD, stdout+stderr to LOG (or to a
# scratch file when LOG is "-"), newlines normalised as Python text mode does.
# With M19R_USE_ENV=1 the qualification environment is added. The captured
# output file is left in M19R_OUT.
m19r_cmd() {
    local cwd=$1 log=$2 rc shown
    shift 2
    if [ "$log" = - ]; then
        M19R_OUT=$M19R_TMP/cmd.out; shown=None
    else
        M19R_OUT=$log; shown=$log
    fi
    if [ "${M19R_USE_ENV:-0}" = 1 ]; then
        (cd "$cwd" && exec env OMEGA_M19R_CANDIDATE="$M19R_OMEGA_CAND" OMEGA_QUAL_RECORD=1 \
            PHYSICS_DIR="$M19R_PHYSICS" "$@" 9>&-) 2>&1 | sed -e 's/\r$//' | tr '\r' '\n' > "$M19R_OUT"
    else
        (cd "$cwd" && exec "$@" 9>&-) 2>&1 | sed -e 's/\r$//' | tr '\r' '\n' > "$M19R_OUT"
    fi
    rc=${PIPESTATUS[0]}
    [ "$rc" -eq 0 ] || m19r_fail "$1 exited $rc; see $shown"
}

# m19r_git REPO ARGS... -- git output with surrounding whitespace stripped.
m19r_git() {
    local repo=$1
    shift
    m19r_cmd "$repo" - git "$@" || return 1
    M19R_GIT=$(m19r_strip "$(cat "$M19R_OUT")")
}

m19r_sha_file() { sha256sum < "$1" | cut -d' ' -f1; }

# m19r_must_candidate REPO SHA
m19r_must_candidate() {
    local repo=$1 supplied=$2 name
    name=$(basename "$repo")
    [[ $supplied =~ ^[0-9a-fA-F]{40}$ ]] || m19r_fail "candidate must be an explicit full 40-hex SHA" || return 1
    m19r_git "$repo" rev-parse HEAD || return 1
    [ "${M19R_GIT,,}" = "${supplied,,}" ] || m19r_fail "$name HEAD differs from supplied candidate" || return 1
    m19r_git "$repo" status --porcelain --untracked-files=normal || return 1
    [ -z "$M19R_GIT" ] || m19r_fail "$name candidate tree is dirty"
}

# m19r_write_immutable_receipt TARGET < receipt-json
# Pretty form (indent 2, ASCII) written O_EXCL, mode 0444, fsync'd.
m19r_write_immutable_receipt() {
    local target=$1 pretty
    mkdir -p "$(dirname "$target")" || m19r_fail "cannot create $(dirname "$target")" || return 1
    pretty=$(mktemp "${M19R_TMP}/receipt.XXXXXX") || return 1
    "$JSON_CANON" --pretty > "$pretty" || m19r_fail "receipt is not valid JSON" || return 1
    "$JSON_CANON" --write-exclusive "$target" < "$pretty" 2> "$pretty.err" ||
        m19r_fail "$(sed 's/^json_canon: //' "$pretty.err")"
}

# m19r_events FILE SUITE -- one "suite<TAB>id<TAB>status" line per observed
# gate: first every "[PASS|FAIL] id" line (the id may follow on a later line
# after only whitespace), then every "NAME_PASS:" line, FAIL when the word
# FAIL appears on it.
m19r_events() {
    LC_ALL=C awk -v suite="$2" '
        function gate(line,    m, st, id) {
            if (match(line, /^[ \t\v\f\r]*\[(PASS|FAIL)\][ \t\v\f\r]+[A-Za-z0-9_:-]+/)) {
                m = substr(line, 1, RLENGTH)
                sub(/^[ \t\v\f\r]+/, "", m)
                st = substr(m, 2, 4)
                id = substr(m, 7)
                sub(/^[ \t\v\f\r]+/, "", id)
                print suite "\t" id "\t" st
                return 1
            }
            return 0
        }
        { lines[NR] = $0 }
        END {
            pending = ""
            for (i = 1; i <= NR; i++) {
                line = lines[i]
                if (pending != "") {
                    if (line ~ /^[ \t\v\f\r]*$/) continue
                    if (match(line, /^[ \t\v\f\r]*[A-Za-z0-9_:-]+/)) {
                        id = substr(line, 1, RLENGTH)
                        sub(/^[ \t\v\f\r]+/, "", id)
                        print suite "\t" id "\t" pending
                        pending = ""
                        continue
                    }
                    pending = ""
                }
                if (gate(line)) continue
                if (match(line, /^[ \t\v\f\r]*\[(PASS|FAIL)\][ \t\v\f\r]*$/))
                    pending = (index(line, "[PASS]") ? "PASS" : "FAIL")
            }
            for (i = 1; i <= NR; i++) {
                line = lines[i]
                if (line ~ /^[A-Z][A-Z0-9_]+_PASS:/) {
                    name = substr(line, 1, index(line, ":") - 1)
                    st = (line ~ /(^|[^A-Za-z0-9_])FAIL([^A-Za-z0-9_]|$)/) ? "FAIL" : "PASS"
                    print suite "\t" name "\t" st
                }
            }
        }' "$1"
}

# JSON views of an events TSV (stdin).
m19r_events_json() {
    awk -F'\t' 'BEGIN { printf "[" } NR > 1 { printf "," }
        { printf "{\"suite\":\"%s\",\"id\":\"%s\",\"status\":\"%s\"}", $1, $2, $3 }
        END { printf "]" }'
}
m19r_event_ids_json() {
    awk -F'\t' 'BEGIN { printf "[" } NR > 1 { printf "," }
        { printf "\"%s:%s\"", $1, $2 } END { printf "]" }'
}

# m19r_observed_counts < events-tsv -> {"completed":N,"failed":N,"passed":N}
m19r_observed_counts() {
    awk -F'\t' '{ c++ } $3 == "PASS" { p++ } $3 == "FAIL" { f++ }
        END { printf "{\"completed\":%d,\"failed\":%d,\"passed\":%d}", c, f, p }'
}

# m19r_require_passed < events-tsv
m19r_require_passed() {
    awk -F'\t' '{ n++ } $3 != "PASS" { bad = 1 } END { exit (n == 0 || bad) }' ||
        m19r_fail "at least one observed gate failed"
}

# m19r_tagged_json FILE TAG -- the JSON after "TAG:" on every line that starts
# with it, one per line; each must parse. Fails when there is none.
m19r_tagged_json() {
    local file=$1 tag=$2 lines line
    lines=$(LC_ALL=C awk -v t="$tag:" 'index($0, t) == 1 { print substr($0, length(t) + 1) }' "$file")
    [ -n "$lines" ] || m19r_fail "missing $tag observation" || return 1
    while IFS= read -r line; do
        printf '%s' "$line" | "$JSON_CANON" > /dev/null 2>&1 ||
            m19r_fail "$tag observation is not valid JSON" || return 1
    done <<< "$lines"
    printf '%s\n' "$lines"
}

m19r_physics_binary() {
    local physics=$1 output=$2 source=$3 headers
    headers=$physics/third_party/nvidia-open-580.173.02
    m19r_cmd "$physics" - gcc -std=gnu11 -O2 -Wall -Wextra -Werror \
        -I "$physics/nvrm" -I "$physics/m16" \
        -I "$headers/src/common/sdk/nvidia/inc" \
        -I "$headers/kernel-open/common/inc" \
        -I "$headers/kernel-open/nvidia-uvm" \
        -I "$headers/src/nvidia/arch/nvalloc/unix/include" \
        "$physics/nvrm/nvrm.c" "$physics/m16/m16_native.c" \
        "$physics/$source" -o "$output"
}

m19r_historical() {
    local list
    m19r_git "$M19R_OMEGA" ls-files evidence || return 1
    list=$M19R_GIT
    if [ -z "$list" ]; then return 0; fi
    (cd "$M19R_OMEGA" && printf '%s\n' "$list" | xargs -d '\n' sha256sum --) ||
        m19r_fail "cannot hash historical evidence"
}

# Everything inside the Python original's try block. Sets R_* for run.json.
m19r_qualify() {
    local omega=$M19R_OMEGA physics=$M19R_PHYSICS run_dir=$M19R_RUN_DIR
    local historical after binary binary_sha name source flag target rc
    local hardware_raw driver_version hardware hardware_digest all_tsv m19_tsv
    local m19 soak samples msg manifest manifest_digest counts body receipt_digest

    m19r_must_candidate "$omega" "$M19R_OMEGA_CAND" || return 1
    m19r_must_candidate "$physics" "$M19R_PHYSICS_CAND" || return 1
    [ -r "$omega/physics.lock" ] ||
        m19r_fail "cannot read $omega/physics.lock" || return 1
    locked=$(m19r_strip "$(cat "$omega/physics.lock")")
    [ "${locked,,}" = "${M19R_PHYSICS_CAND,,}" ] ||
        m19r_fail "physics.lock differs from supplied physics candidate" || return 1
    historical=$(m19r_historical) || return 1

    exec 9> /tmp/aien-gb10.lock || m19r_fail "cannot open /tmp/aien-gb10.lock" || return 1
    flock -x 9
    m19r_locked_section
    rc=$?
    exec 9>&-
    [ "$rc" -eq 0 ] || return 1

    after=$(m19r_historical) || return 1
    [ "$historical" = "$after" ] ||
        m19r_fail "historical evidence changed during qualification" || return 1

    all_tsv=$M19R_TMP/events.tsv
    : > "$all_tsv"
    for name in "${M19R_SUITES[@]}"; do
        m19r_events "$run_dir/$name.log" "$name" >> "$all_tsv"
    done
    m19r_require_passed < "$all_tsv" || return 1
    manifest="{\"commands\":[\"nvrm/lifecycle_gates\",\"m16/m16_requalify\",\"m16/m16_concurrent\",\"omega/m15\",\"omega/world_lifecycle\",\"omega/m19\",\"omega/m19r_soak\"],\"observed_test_ids\":$(m19r_event_ids_json < "$all_tsv")}"
    manifest_digest=$(printf '%s' "$manifest" | "$JSON_CANON" --sha256) || return 1
    m19=$(m19r_tagged_json "$run_dir/m19.log" M19_OBSERVED_JSON) || return 1
    m19=$(printf '%s\n' "$m19" | tail -n 1)
    m19_tsv=$(m19r_events "$run_dir/m19.log" m19 | awk -F'\t' 'index($2, "OMEGA_ACCEL_RESIDENT_") == 1')
    if [ "$(printf '%s' "$m19_tsv" | grep -c .)" -ne 18 ] || printf '%s\n' "$m19_tsv" | awk -F'\t' '$3 != "PASS" { f = 1 } END { exit !f }'; then
        m19r_fail "M19 printed gate results do not contain exactly 18 passing gates"; return 1
    fi
    msg=$(printf '%s' "$m19" | jq -r '
        def k($n): if has($n) then .[$n] else error("'"'"'\($n)'"'"'") end;
        try (if (k("m19_gates_completed") != 18)
                or (k("m19_gates_passed") != k("m19_gates_completed"))
                or (k("regression_gates_passed") != k("regression_gates_completed"))
             then "M19 did not complete all 18 gates and regressions" else "" end)
        catch (if type == "string" then . else "invalid M19 observation" end)') ||
        m19r_fail "invalid M19 observation" || return 1
    [ -z "$msg" ] || m19r_fail "$msg" || return 1
    R_EVENTS=$(m19r_events_json < "$all_tsv")
    R_M19=$m19
    if [ "$M19R_QUICK" = 1 ]; then
        R_STATUS=QUICK_PASS
        return 0
    fi

    soak=$(m19r_tagged_json "$run_dir/m19r_soak.log" M19R_SOAK_JSON) || return 1
    soak=$(printf '%s\n' "$soak" | tail -n 1)
    samples=$(m19r_tagged_json "$run_dir/m19r_soak.log" M19R_RESOURCE_JSON) || return 1
    samples="[$(printf '%s\n' "$samples" | paste -sd, -)]"
    msg=$(jq -rn --argjson soak "$soak" --argjson samples "$samples" '
        def k($o; $n): if ($o | has($n)) then $o[$n] else error("'"'"'\($n)'"'"'") end;
        def falsy: . == null or . == false or . == 0 or . == "" or . == [] or . == {};
        try (if (k($soak; "passed") | falsy) or (k($soak; "cycles") < 100000)
                or (k($soak; "bytes_churned") <= 2 * k($soak; "physical_memory_bytes"))
                or ($samples | length) != 3
             then "long soak criterion failed"
             elif [$samples[] | k(.; "phase")] != ["start", "end", "post_destroy"]
             then "soak resource snapshots are incomplete"
             else "" end)
        catch (if type == "string" then . else "invalid soak observation" end)') ||
        m19r_fail "invalid soak observation" || return 1
    [ -z "$msg" ] || m19r_fail "$msg" || return 1

    m19r_must_candidate "$omega" "$M19R_OMEGA_CAND" || return 1
    m19r_must_candidate "$physics" "$M19R_PHYSICS_CAND" || return 1
    [ -r "$omega/evidence/omega_accelerator_world_qualification_receipt.json" ] ||
        m19r_fail "missing evidence/omega_accelerator_world_qualification_receipt.json" || return 1
    counts=$(m19r_observed_counts < "$all_tsv")
    local ts
    ts=$(date -u +%Y-%m-%dT%H:%M:%S.%6N+00:00)
    ts=${ts/.000000+/+}
    body="{\"schema\":\"AIEN_M19R_QUALIFICATION_V1\",\"lineage\":\"M19_REPAIR_BASELINE\""
    body+=",\"run_id\":$(m19r_jstr "$M19R_RUN_ID"),\"timestamp_utc\":\"$ts\""
    body+=",\"candidate_git_commit\":\"${M19R_OMEGA_CAND,,}\""
    body+=",\"physics_candidate_git_commit\":\"${M19R_PHYSICS_CAND,,}\""
    body+=",\"candidate_trees_clean\":{\"omega\":true,\"physics\":true}"
    body+=",\"candidate_binary_sha256\":\"$M19R_BINARY_SHA\""
    body+=",\"test_manifest_sha256\":\"$manifest_digest\",\"test_manifest\":$manifest"
    body+=",\"test_results\":$R_EVENTS"
    body+=",\"observed_gate_results_count\":$(jq -n --argjson c "$counts" '$c.completed')"
    body+=",\"observed_pass_count\":$(jq -n --argjson c "$counts" '$c.passed')"
    body+=",\"observed_fail_count\":$(jq -n --argjson c "$counts" '$c.failed')"
    body+=",\"m19_observations\":$m19,\"soak\":$soak,\"resource_samples\":$samples"
    body+=",\"hardware_probe\":$M19R_HARDWARE,\"hardware_probe_sha256\":\"$M19R_HARDWARE_SHA\""
    body+=",\"predecessor_qualification_sha256\":\"$(m19r_sha_file "$omega/evidence/omega_accelerator_world_qualification_receipt.json")\""
    body+=",\"historical_evidence_sha256\":$(printf '%s\n' "$historical" | jq -Rn '[inputs | select(length > 0) | {key: .[66:], value: .[0:64]}] | from_entries')"
    body+=",\"zero_libcuda_linkage\":true"
    body+=",\"driver_handle_count_limitation\":\"No authoritative live-handle query is exposed by the current RM interface; driver-acknowledged RM alloc/free balance and process mappings are the strongest available observables.\"}"
    receipt_digest=$(printf '%s' "$body" | "$JSON_CANON" --sha256) ||
        m19r_fail "receipt body is not valid JSON" || return 1
    local receipt="{\"receipt_digest\":\"$receipt_digest\",${body#\{}"
    if [ "$M19R_RECORD" = 1 ]; then
        target=$omega/evidence/M19R/$receipt_digest.json
        printf '%s' "$receipt" | m19r_write_immutable_receipt "$target" || return 1
        R_PERMANENT=$target
    else
        printf '%s' "$receipt" | "$JSON_CANON" --pretty > "$run_dir/receipt-preview.json" ||
            m19r_fail "cannot write receipt preview" || return 1
    fi
    R_STATUS=PASS
    R_DIGEST=$receipt_digest
    R_SOAK=$soak
    R_SAMPLES=$samples
    return 0
}

# The build + test section, run while holding /tmp/aien-gb10.lock (fd 9).
m19r_locked_section() {
    local omega=$M19R_OMEGA physics=$M19R_PHYSICS run_dir=$M19R_RUN_DIR name source flag
    M19R_SUITES=()
    m19r_cmd "$omega" - make clean || return 1
    mkdir -p "$run_dir" || m19r_fail "cannot create $run_dir" || return 1
    M19R_USE_ENV=1 m19r_cmd "$omega" "$run_dir/build.log" make -j4 "PHYSICS_DIR=$physics" || return 1
    [ -r "$omega/build/omegatool" ] || m19r_fail "missing $omega/build/omegatool" || return 1
    M19R_BINARY_SHA=$(m19r_sha_file "$omega/build/omegatool")
    for pair in nvrm_lifecycle:nvrm/lifecycle_gates.c m16_requalify:m16/m16_requalify.c \
                m16_concurrent:m16/m16_concurrent.c; do
        name=${pair%%:*}; source=${pair#*:}
        m19r_physics_binary "$physics" "$run_dir/$name" "$source" || return 1
        M19R_USE_ENV=1 m19r_cmd "$physics" "$run_dir/$name.log" "$run_dir/$name" || return 1
        M19R_SUITES+=("$name")
    done
    for pair in m15:--run-m15-gates world_lifecycle:--run-world-lifecycle-gates m19:--run-m19-gates; do
        name=${pair%%:*}; flag=${pair#*:}
        M19R_USE_ENV=1 m19r_cmd "$omega" "$run_dir/$name.log" "$omega/build/omegatool" "$flag" || return 1
        M19R_SUITES+=("$name")
    done
    if [ "$M19R_QUICK" != 1 ]; then
        M19R_USE_ENV=1 m19r_cmd "$omega" "$run_dir/m19r_soak.log" "$omega/build/omegatool" --run-m19r-soak || return 1
        M19R_SUITES+=(m19r_soak)
    fi
    m19r_cmd "$omega" "$run_dir/readelf.log" readelf -d "$omega/build/omegatool" || return 1
    m19r_cmd "$omega" "$run_dir/nm.log" nm -u "$omega/build/omegatool" || return 1
    if cat "$run_dir/readelf.log" "$run_dir/nm.log" | LC_ALL=C grep -qiE "$M19R_CUDA_RE"; then
        m19r_fail "CUDA linkage or undefined symbol found"; return 1
    fi
    m19r_cmd "$omega" - nvidia-smi --query-gpu=pci.bus_id,pci.device_id,driver_version,name,uuid \
        --format=csv,noheader || return 1
    hardware_raw=$(m19r_strip "$(cat "$M19R_OUT")")
    driver_version=$(cat /proc/driver/nvidia/version) ||
        m19r_fail "cannot read /proc/driver/nvidia/version" || return 1
    driver_version=$(m19r_strip "$driver_version")
    M19R_HARDWARE="{\"nvidia_smi_raw\":$(m19r_jstr "$hardware_raw"),\"driver_proc_version\":$(m19r_jstr "$driver_version")}"
    M19R_HARDWARE_SHA=$(printf '%s' "$M19R_HARDWARE" | "$JSON_CANON" --sha256) || return 1
}

m19r_usage() {
    echo "usage: $M19R_PROG [-h] --omega-candidate OMEGA_CANDIDATE --physics-candidate PHYSICS_CANDIDATE [--physics-dir PHYSICS_DIR] [--evidence-root DIR] [--run-id ID] [--record] [--quick]"
}

m19r_argerror() {
    m19r_usage >&2
    echo "$M19R_PROG: error: $1" >&2
    exit 2
}

# m19r_write_environment FILE -- who/where/what ran (environment.json).
m19r_write_environment() {
    local pshal
    pshal=$(git -C "$M19R_PHYSICS" rev-parse HEAD 2> /dev/null) || pshal=unknown
    jq -n --arg host "$(hostname)" --arg date "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        --arg omega "$(git -C "$M19R_OMEGA" rev-parse HEAD 2> /dev/null || echo unknown)" \
        --arg physics "$pshal" --arg kernel "$(uname -r)" --arg user "$(id -un)" \
        '{hostname: $host, date_utc: $date, omega_sha: $omega, physics_sha: $physics, uname_r: $kernel, user: $user}' > "$1"
}

# m19r_write_verdict RUN_DIR EXIT_CODE -- verdict.json from run.json and the
# suite logs. failing_step is run.json's error text; failing_gate_or_exit is
# the first observed FAIL gate (suite:id) in the logs, else "exit:<code>" for
# a failed run, else null.
m19r_write_verdict() {
    local dir=$1 code=$2 status step= gate= log name
    status=$(jq -r '.status' "$dir/run.json" 2> /dev/null) || status=
    [ -n "$status" ] || status=UNKNOWN
    step=$(jq -r '.error // empty' "$dir/run.json" 2> /dev/null)
    for log in "$dir"/*.log; do
        name=$(basename "$log" .log)
        case $name in stdout|stderr|build) continue;; esac
        gate=$(m19r_events "$log" "$name" | awk -F'\t' '$3 == "FAIL" { print $1 ":" $2; exit }')
        [ -z "$gate" ] || break
    done
    [ -n "$gate" ] || { [ "$code" -eq 0 ] || gate="exit:$code"; }
    jq -n --arg id "$(basename "$dir")" --arg status "$status" --arg step "$step" \
        --arg gate "$gate" --argjson code "$code" \
        '{run_id: $id, status: $status, failing_step: (if $step == "" then null else $step end),
          failing_gate_or_exit: (if $gate == "" then null else $gate end), exit_code: $code}' > "$dir/verdict.json"
}

# m19r_seal RUN_DIR -- hashes.sha256 over every file except itself; files made
# read-only.
m19r_seal() {
    (cd "$1" && find . -type f ! -name hashes.sha256 -print0 | LC_ALL=C sort -z |
        xargs -0 sha256sum -- > hashes.sha256) || return 1
    find "$1" -type f -exec chmod a-w {} +
}

m19r_main() {
    local physics_dir= missing=() run_json evidence_root= run_id= argv
    printf -v argv '%q ' "$M19R_PROG" "$@"
    M19R_OMEGA_CAND= M19R_PHYSICS_CAND= M19R_RECORD=0 M19R_QUICK=0
    local have_o=0 have_p=0
    while [ $# -gt 0 ]; do
        case $1 in
            -h|--help) m19r_usage; exit 0;;
            --omega-candidate=*) M19R_OMEGA_CAND=${1#*=}; have_o=1;;
            --physics-candidate=*) M19R_PHYSICS_CAND=${1#*=}; have_p=1;;
            --physics-dir=*) physics_dir=${1#*=};;
            --evidence-root=*) evidence_root=${1#*=};;
            --run-id=*) run_id=${1#*=};;
            --omega-candidate|--physics-candidate|--physics-dir|--evidence-root|--run-id)
                [ $# -ge 2 ] || m19r_argerror "argument $1: expected one argument"
                case $1 in
                    --omega-candidate) M19R_OMEGA_CAND=$2; have_o=1;;
                    --physics-candidate) M19R_PHYSICS_CAND=$2; have_p=1;;
                    --physics-dir) physics_dir=$2;;
                    --evidence-root) evidence_root=$2;;
                    --run-id) run_id=$2;;
                esac
                shift;;
            --record) M19R_RECORD=1;;
            --quick) M19R_QUICK=1;;
            *) m19r_argerror "unrecognized arguments: $1";;
        esac
        shift
    done
    [ "$have_o" = 1 ] || missing+=(--omega-candidate)
    [ "$have_p" = 1 ] || missing+=(--physics-candidate)
    if [ ${#missing[@]} -gt 0 ]; then
        local IFS=,
        m19r_argerror "the following arguments are required: $(printf '%s' "${missing[*]}" | sed 's/,/, /g')"
    fi
    if [ "$M19R_RECORD" = 1 ] && [ "$M19R_QUICK" = 1 ]; then
        m19r_argerror "--quick cannot emit a permanent receipt"
    fi
    M19R_PHYSICS=$(realpath -m "${physics_dir:-$(dirname "$M19R_OMEGA")/physics}")

    # Evidence lives outside what `make clean` removes ($OUT_DIR, default build/).
    evidence_root=$(realpath -m "${evidence_root:-$M19R_OMEGA/qual-runs}")
    local out_dir
    out_dir=$(realpath -m "$M19R_OMEGA/${OUT_DIR:-build}")
    case $evidence_root/ in
        "$out_dir"/*|"$M19R_OMEGA"/build/*) m19r_argerror "evidence root $evidence_root is inside the cleaned build directory";;
    esac
    case $out_dir/ in
        "$evidence_root"/*) m19r_argerror "evidence root $evidence_root would be removed by make clean";;
    esac
    case $run_id in */*|.|..) m19r_argerror "invalid --run-id";; esac

    m19r_build_canon || { echo "M19R qualification failed: cannot build tools/json_canon.c" >&2; exit 1; }
    trap 'rm -rf "$M19R_TMP"' EXIT

    M19R_RUN_ID=${run_id:-$(date -u +%Y%m%dT%H%M%SZ)-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')}
    M19R_RUN_DIR=$evidence_root/$M19R_RUN_ID
    mkdir -p "$evidence_root" || exit 1
    if ! mkdir "$M19R_RUN_DIR" 2> /dev/null; then
        echo "$M19R_PROG: error: run directory $M19R_RUN_DIR already exists or cannot be created; run directories are never reused" >&2
        exit 2
    fi
    printf '%s\n' "$argv" > "$M19R_RUN_DIR/command.txt"
    m19r_write_environment "$M19R_RUN_DIR/environment.json" || exit 1

    # Whole-run stdout/stderr go to the console and to the run directory.
    local p1 p2
    exec 3>&1 4>&2
    exec > >(tee "$M19R_RUN_DIR/stdout.log" >&3)
    p1=$!
    exec 2> >(tee "$M19R_RUN_DIR/stderr.log" >&4)
    p2=$!

    R_STATUS=FAILED R_ERROR= R_PERMANENT= R_DIGEST= R_EVENTS= R_M19= R_SOAK= R_SAMPLES=
    local rc=0
    rm -f "$M19R_TMP/error"
    if ! m19r_qualify; then
        rc=1
        R_STATUS=FAILED
        [ ! -s "$M19R_TMP/error" ] || M19R_ERR=$(cat "$M19R_TMP/error")
        R_ERROR=$M19R_ERR
        echo "M19R qualification failed: $M19R_ERR" >&2
    fi
    run_json="{\"run_id\":$(m19r_jstr "$M19R_RUN_ID"),\"commands\":[],\"status\":\"$R_STATUS\""
    [ "$rc" = 0 ] || run_json+=",\"error\":$(m19r_jstr "$R_ERROR")"
    [ -z "$R_PERMANENT" ] || run_json+=",\"permanent_receipt\":$(m19r_jstr "$R_PERMANENT")"
    [ -z "$R_DIGEST" ] || run_json+=",\"receipt_digest\":\"$R_DIGEST\""
    if [ "$rc" = 0 ]; then
        run_json+=",\"events\":$R_EVENTS,\"m19\":$R_M19"
        [ -z "$R_SOAK" ] || run_json+=",\"soak\":$R_SOAK,\"resource_samples\":$R_SAMPLES"
    fi
    run_json+="}"
    printf '%s' "$run_json" | "$JSON_CANON" --pretty > "$M19R_RUN_DIR/run.json" || rc=1
    echo "M19R run evidence: $M19R_RUN_DIR/run.json"
    exec 1>&3 2>&4 3>&- 4>&-
    wait "$p1" "$p2" 2> /dev/null
    m19r_write_verdict "$M19R_RUN_DIR" "$rc" || rc=1
    m19r_seal "$M19R_RUN_DIR" || rc=1
    exit "$rc"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -u
    m19r_main "$@"
fi
