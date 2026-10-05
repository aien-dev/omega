#!/bin/bash
# e1_combine.sh -- E1 numerical closure receipt (one deterministic, content-addressed aggregate).
#
#   tools/e1_combine.sh --candidate SHA --main SHA --physics SHA --manifest FILE \
#       --gate5 FILE --reduce FILE --transc FILE --mutant-log FILE --campaign-log FILE \
#       --ldst-chip-log FILE --host-rerun FILE --transc-prior FILE [--evidence-dir DIR]
#
# Read-only aggregation, in the spirit of tools/gate14_combine.sh. The E1 chip
# campaign ran on --candidate; the squash merge is --main. This receipt never
# says the chip ran on main. It binds, and refuses unless every one holds:
#
#   manifest   AIEN_E1_EQUIVALENCE_MANIFEST_V1 from tools/e1_manifest.sh for exactly
#              these two commits; every file's digests are recomputed here from git;
#              every chip-class file equal; a differing host-class file is allowed
#              only because --host-rerun proves the host tier PASSED on main; the
#              rebuilt chip binaries (from main) equal the binary digests the Gate 5,
#              reduce and transc receipts carry.
#   gate5      AIEN_OMEGA_NUMERIC_0_V1: receipt_digest = SHA-256 of the canonical body
#              without it (tools/json_canon.c), named for it, PASS, exit 0, no failures,
#              candidate = run commit = --candidate, physics = lock = --physics, clean trees.
#   reduce     E1_REDUCE_GB10_PARITY: file SHA-256 = file name, PASS, exit "0",
#              ops exactly SUM MAX MIN MEAN, 380 cases, 0 mismatches overall and per op,
#              commits and clean flags as above.
#   transc     E1-TRANSC-GB10: file SHA-256 = file name, PASS, chip exit 0, ops exactly
#              SIN COS ERF GELU RSQRT, each checked 4294967296 exhaustive, 0 mismatches,
#              0 unwritten, commits, pins and clean flags as above.
#   prior      the earlier E1-TRANSC-GB10 receipt (EXP2 LOG2 SIGMOID TANH, each 4294967296
#              exhaustive, 0 mismatches, PASS, same Physics): its four kernel digests must
#              equal the same four kernels listed in --transc, so the words that ran on the
#              chip then are the words the candidate carries. Its omega commit is recorded,
#              not required to equal --candidate.
#   mutant     the MEAN mutant chip log, SHA-256 = file name: MEAN FAIL with mismatches > 0,
#              SUM MAX MIN PASS with 0 mismatches (the chip caught the mutant).
#   campaign   the five-step campaign log: names candidate and physics, LDST host and
#              chip PASS, names the Gate 5, reduce, transc and mutant digests above,
#              "E1 MEAN MUTANT: KILLED", final VERDICT PASS, no GB10_COMPLETION_UNCERTAIN.
#   ldst log   the LDST chip log: as many "RESULT chip ... verdict=PASS" lines as the
#              campaign's "specs passed", no OMEGA_DEVERR, last VERDICT PASS.
#   host rerun AIEN_E1_HOST_RERUN_V1, SHA-256 = file name, omega_commit = --main, clean
#              tracked tree before and after, every one of the six host lines PASS.
#
# On accept prints the receipt digest and, with --evidence-dir, writes
# <dir>/<digest>.json (pretty, exclusive, 0444; the location is evidence/E1-CLOSURE/).
# On refuse prints "REFUSED: <reason>" on stderr, writes nothing, exits 1. Exit 2 = usage.
# Digests make alteration detectable; they are not signatures. Shell + coreutils +
# git + jq + tools/json_canon.c. No Python.

E1_OMEGA=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
E1_GIT=${E1_GIT_DIR:-$E1_OMEGA}   # tests point this at a scratch repository
E1_SCHEMA=AIEN_E1_CLOSURE_V1
E1_ERR=
E1_HOST_LINES="GB10-COMPILE NUMERIC-HOST PROGRAM TRANSC MANIFEST HOSTALL"

e1_refuse() { E1_ERR=$1; return 1; }

e1_build_canon() {
    E1_TMP=$(mktemp -d "${TMPDIR:-/tmp}/e1combine.XXXXXX") || return 1
    JSON_CANON=$E1_TMP/json_canon
    gcc -std=gnu11 -O2 -Wall -Wextra -Werror -I"$E1_OMEGA/src" -o "$JSON_CANON" \
        "$E1_OMEGA/tools/json_canon.c" "$E1_OMEGA/src/sha256.c" -lm
}

e1_sha() { sha256sum -- "$1" | cut -c1-64; }

# e1_copy FILE VAR -- private copy of FILE (so it cannot change between checks).
e1_copy() {
    local f=$1 s
    [ -n "$f" ] || e1_refuse "a required input was not given" || return 1
    [ -f "$f" ] && [ -r "$f" ] || e1_refuse "cannot read $f" || return 1
    s=$(mktemp "$E1_TMP/in.XXXXXX") && cp -- "$f" "$s" || e1_refuse "cannot copy $f" || return 1
    printf -v "$2" '%s' "$s"
}

e1_json() { # FILE -- valid JSON object
    "$JSON_CANON" --check < "$1" || e1_refuse "$2 is not valid JSON" || return 1
    jq -e 'type == "object"' "$1" > /dev/null || e1_refuse "$2 is not a JSON object" || return 1
}

e1_named_for_sha() { # FILE ORIG LABEL -- file name is its own SHA-256
    local name; name=$(basename "$2")
    [ "$name" = "$(e1_sha "$1").${name##*.}" ] || e1_refuse "$3 $2 is not named for its SHA-256" || return 1
}

# ---- children ----

e1_check_gate5() { # FILE ORIG
    local s=$1 f=$2 d rc
    e1_json "$s" "$f" || return 1
    [ "$(jq -r '.schema // ""' "$s")" = AIEN_OMEGA_NUMERIC_0_V1 ] || e1_refuse "gate5 $f has the wrong schema" || return 1
    d=$(jq -r '.receipt_digest // ""' "$s")
    [[ $d =~ ^[0-9a-f]{64}$ ]] || e1_refuse "gate5 $f has no 64-hex receipt_digest" || return 1
    rc=$(jq -c 'del(.receipt_digest)' "$s" | "$JSON_CANON" --sha256) || e1_refuse "cannot digest gate5 $f" || return 1
    [ "$rc" = "$d" ] || e1_refuse "gate5 $f receipt_digest does not match its content" || return 1
    [ "$(basename "$f" .json)" = "$d" ] || e1_refuse "gate5 $f is named for a different digest" || return 1
    jq -e --arg c "$E1_CAND" --arg p "$E1_PHYS" '
        .status == "PASS" and .gate_binary_exit_status == 0 and .observed_fail_count == 0
        and .candidate_git_commit == $c and .run_git_commit == $c
        and .physics_candidate_git_commit == $p and .physics_lock == $p
        and .candidate_trees_clean.omega == true and .candidate_trees_clean.physics == true
        and (.test_results | type) == "array" and (.test_results | length) > 0
        and (.test_results | length) == .observed_test_count
        and .observed_pass_count == .observed_test_count
        and all(.test_results[]; .status == "PASS")
        and (.hardware_descriptor_digest | type) == "string" and (.hardware_descriptor_digest | test("^[0-9a-f]{64}$"))
        and (.candidate_binary_sha256 | type) == "string" and (.candidate_binary_sha256 | test("^[0-9a-f]{64}$"))' \
        "$s" > /dev/null 2>&1 || e1_refuse "gate5 $f does not record a PASS on candidate $E1_CAND / physics $E1_PHYS" || return 1
    E1_G5_DIGEST=$d
    E1_G5_BIN=$(jq -r .candidate_binary_sha256 "$s")
    E1_G5_HW=$(jq -r .hardware_descriptor_digest "$s")
    E1_G5_TESTS=$(jq -r .observed_test_count "$s")
}

e1_check_reduce() { # FILE ORIG
    local s=$1 f=$2
    e1_json "$s" "$f" || return 1
    e1_named_for_sha "$s" "$f" reduce || return 1
    jq -e --arg c "$E1_CAND" --arg p "$E1_PHYS" '
        .suite == "E1_REDUCE_GB10_PARITY" and .status == "PASS" and .exit_status == "0"
        and .omega_commit == $c and .physics_commit == $p
        and .omega_clean_before == true and .omega_clean_after == true
        and .ops == ["SUM","MAX","MIN","MEAN"]
        and (.parity_line | test("^RED_GB10_PARITY: PASS ops=SUM,MAX,MIN,MEAN cases=380 mismatches=0$"))
        and ([.ops[] as $o | .parity_by_op[$o]] | all(type == "string" and test("^RED_GB10_PARITY_[A-Z]+: PASS op=") and test(" mismatches=0( |$)")))
        and ([.ops[] as $o | .parity_by_op[$o] | capture("cases=(?<n>[0-9]+)") | .n | tonumber] | add) == 380
        and (.binary_sha256 | type) == "string" and (.binary_sha256 | test("^[0-9a-f]{64}$"))
        and (.log_sha256 | type) == "string" and (.log_sha256 | test("^[0-9a-f]{64}$"))' \
        "$s" > /dev/null 2>&1 || e1_refuse "reduce $f does not record SUM/MAX/MIN/MEAN 380 cases 0 mismatches PASS on candidate $E1_CAND / physics $E1_PHYS" || return 1
    E1_RED_DIGEST=$(e1_sha "$s")
    E1_RED_BIN=$(jq -r .binary_sha256 "$s")
}

e1_check_transc() { # FILE ORIG
    local s=$1 f=$2
    e1_json "$s" "$f" || return 1
    e1_named_for_sha "$s" "$f" transc || return 1
    jq -e --arg c "$E1_CAND" --arg p "$E1_PHYS" '
        .gate == "E1-TRANSC-GB10" and .verdict == "PASS" and .chip_exit_status == 0
        and .omega_commit == $c and .physics_commit == $p and .physics_lock_pin == $p
        and .omega_tree_clean_before == true and .omega_tree_clean_after == true and .omega_commit_unchanged_after == true
        and .physics_tree_clean_before == true and .physics_tree_clean_after == true and .physics_commit_unchanged_after == true
        and .ops_requested == ["SIN","COS","ERF","GELU","RSQRT"]
        and ([.ops[].op] | sort) == (["SIN","COS","ERF","GELU","RSQRT"] | sort) and (.ops | length) == 5
        and all(.ops[]; .checked == 4294967296 and .exhaustive == true and .mismatches == 0 and .unwritten == 0 and .verdict == "PASS")
        and (.binary_sha256 | type) == "string" and (.binary_sha256 | test("^[0-9a-f]{64}$"))
        and (.chip_log_sha256 | type) == "string" and (.chip_log_sha256 | test("^[0-9a-f]{64}$"))' \
        "$s" > /dev/null 2>&1 || e1_refuse "transc $f does not record five exhaustive 2^32 PASS ops on candidate $E1_CAND / physics $E1_PHYS" || return 1
    E1_TR_DIGEST=$(e1_sha "$s")
    E1_TR_BIN=$(jq -r .binary_sha256 "$s")
    E1_TR_INPUTS=$(jq -r '[.ops[].checked] | add' "$s")
}

e1_check_transc_prior() { # FILE ORIG  (needs e1_check_transc first)
    local s=$1 f=$2 op a b
    e1_json "$s" "$f" || return 1
    e1_named_for_sha "$s" "$f" "transc prior" || return 1
    jq -e --arg p "$E1_PHYS" '
        .gate == "E1-TRANSC-GB10" and .verdict == "PASS" and .chip_exit_status == 0
        and .physics_commit == $p and .physics_lock_pin == $p
        and .omega_tree_clean_before == true and .omega_tree_clean_after == true
        and (.omega_commit | test("^[0-9a-f]{40}$"))
        and ([.ops[].op] | sort) == (["EXP2","LOG2","SIGMOID","TANH"] | sort) and (.ops | length) == 4
        and all(.ops[]; .checked == 4294967296 and .exhaustive == true and .mismatches == 0 and .unwritten == 0 and .verdict == "PASS")
        and (.kernels | type) == "array"' "$s" > /dev/null 2>&1 ||
        e1_refuse "transc prior $f does not record EXP2/LOG2/SIGMOID/TANH exhaustive PASS on physics $E1_PHYS" || return 1
    for op in EXP2 LOG2 SIGMOID TANH; do
        a=$(jq -r --arg o "$op" '[.kernels[] | select(.op == $o) | .sha256] | if length == 1 then .[0] else "" end' "$s")
        b=$(jq -r --arg o "$op" '[.kernels[] | select(.op == $o) | .sha256] | if length == 1 then .[0] else "" end' "$E1_S_TR")
        [[ $a =~ ^[0-9a-f]{64}$ ]] && [ "$a" = "$b" ] || e1_refuse "transc prior $f: $op kernel digest ($a) is not the candidate kernel ($b)" || return 1
    done
    E1_TRP_DIGEST=$(e1_sha "$s")
    E1_TRP_COMMIT=$(jq -r .omega_commit "$s")
}

e1_check_mutant() { # FILE ORIG
    local s=$1 f=$2 m
    e1_named_for_sha "$s" "$f" mutant || return 1
    m=$(grep -E '^RED_GB10_PARITY_MEAN: FAIL ' "$s" | grep -oE 'mismatches=[0-9]+' | head -n1 | cut -d= -f2)
    [ -n "$m" ] && [ "$m" -gt 0 ] || e1_refuse "mutant log $f does not show MEAN failing with mismatches > 0" || return 1
    for op in SUM MAX MIN; do
        grep -Eq "^RED_GB10_PARITY_$op: PASS .* mismatches=0( |$)" "$s" || e1_refuse "mutant log $f does not show $op PASS with 0 mismatches" || return 1
    done
    grep -q '^E1 Reduce Verdict: FAIL$' "$s" || e1_refuse "mutant log $f lacks the FAIL verdict" || return 1
    E1_MUT_DIGEST=$(e1_sha "$s")
    E1_MUT_MISMATCHES=$m
}

e1_check_campaign() { # FILE ORIG
    local s=$1 f=$2
    grep -Eq "^omega $E1_CAND physics $E1_PHYS\$" "$s" || e1_refuse "campaign log $f does not name candidate $E1_CAND and physics $E1_PHYS" || return 1
    ! grep -q GB10_COMPLETION_UNCERTAIN "$s" || e1_refuse "campaign log $f records an uncertain completion" || return 1
    grep -Eq '^Gate LDST host: PASSED=[1-9][0-9]* FAILED=0$' "$s" || e1_refuse "campaign log $f lacks LDST host PASS with 0 failures" || return 1
    grep -Eq '^\[PASS\] ldst: ([0-9]+) of \1 kernels decode' "$s" || e1_refuse "campaign log $f lacks the full ldst nvdisasm decode" || return 1
    E1_LDST_SPECS=$(grep -E '^specs passed: [0-9]+$' "$s" | head -n1 | cut -d' ' -f3)
    [ -n "$E1_LDST_SPECS" ] && [ "$E1_LDST_SPECS" -gt 0 ] || e1_refuse "campaign log $f lacks 'specs passed: N'" || return 1
    grep -q "^Gate 5 PASS: receipt digest $E1_G5_DIGEST\$" "$s" || e1_refuse "campaign log $f does not name the Gate 5 receipt $E1_G5_DIGEST" || return 1
    grep -Eq "^receipt: .*/$E1_RED_DIGEST\.json status=PASS" "$s" || e1_refuse "campaign log $f does not name the reduce receipt $E1_RED_DIGEST as PASS" || return 1
    grep -Eq "^RECEIPT .*/$E1_TR_DIGEST\.json\$" "$s" || e1_refuse "campaign log $f does not name the transc receipt $E1_TR_DIGEST" || return 1
    grep -q "^log sha256: $E1_MUT_DIGEST\$" "$s" || e1_refuse "campaign log $f does not name the mutant log $E1_MUT_DIGEST" || return 1
    grep -q '^E1 MEAN MUTANT: KILLED$' "$s" || e1_refuse "campaign log $f does not record the MEAN mutant as KILLED" || return 1
    [ "$(grep -E '^VERDICT ' "$s" | tail -n1)" = "VERDICT PASS" ] || e1_refuse "campaign log $f does not end on VERDICT PASS" || return 1
    E1_CAMP_DIGEST=$(e1_sha "$s")
}

e1_check_ldst_log() { # FILE ORIG
    local s=$1 f=$2 n
    n=$(grep -cE '^RESULT chip .* verdict=PASS' "$s")
    [ "$n" = "$E1_LDST_SPECS" ] || e1_refuse "ldst chip log $f has $n PASS results, campaign says $E1_LDST_SPECS" || return 1
    ! grep -Eq 'verdict=(FAIL|NOT_RUN)' "$s" || e1_refuse "ldst chip log $f has a failed or unrun spec" || return 1
    ! grep -q OMEGA_DEVERR "$s" || e1_refuse "ldst chip log $f records a device error" || return 1
    [ "$(grep -E '^VERDICT ' "$s" | tail -n1)" = "VERDICT PASS" ] || e1_refuse "ldst chip log $f does not end on VERDICT PASS" || return 1
    E1_LDST_DIGEST=$(e1_sha "$s")
}

e1_check_host_rerun() { # FILE ORIG
    local s=$1 f=$2 l
    e1_json "$s" "$f" || return 1
    e1_named_for_sha "$s" "$f" "host rerun" || return 1
    jq -e --arg m "$E1_MAIN" '
        .schema == "AIEN_E1_HOST_RERUN_V1" and .omega_commit == $m
        and .tracked_tree_clean_before == true and .tracked_tree_clean_after == true
        and (.lines | type) == "object"' "$s" > /dev/null 2>&1 ||
        e1_refuse "host rerun $f is not a clean-tree E1 host rerun on main $E1_MAIN" || return 1
    for l in $E1_HOST_LINES; do
        [ "$(jq -r --arg l "$l" '.lines[$l] // ""' "$s")" = PASS ] || e1_refuse "host rerun $f: line $l is not PASS" || return 1
    done
    E1_HOST_DIGEST=$(e1_sha "$s")
}

e1_check_manifest() { # FILE ORIG
    local s=$1 f=$2 n i path cls a b eq ga gb nd
    e1_json "$s" "$f" || return 1
    jq -e --arg c "$E1_CAND" --arg m "$E1_MAIN" '
        .schema == "AIEN_E1_EQUIVALENCE_MANIFEST_V1" and .candidate_git_commit == $c and .main_git_commit == $m
        and (.files | type) == "array" and (.files | length) > 0
        and (.rebuilt_chip_binaries | type) == "array"' "$s" > /dev/null 2>&1 ||
        e1_refuse "manifest $f is not an equivalence manifest for $E1_CAND -> $E1_MAIN with rebuilt binaries" || return 1
    git -C "$E1_GIT" cat-file -e "$E1_CAND^{commit}" 2>/dev/null || e1_refuse "candidate $E1_CAND is not in this repository" || return 1
    git -C "$E1_GIT" cat-file -e "$E1_MAIN^{commit}" 2>/dev/null || e1_refuse "main $E1_MAIN is not in this repository" || return 1
    n=$(jq -r '.files | length' "$s"); nd=0
    for ((i = 0; i < n; i++)); do
        path=$(jq -r ".files[$i].path" "$s"); cls=$(jq -r ".files[$i].class" "$s")
        a=$(jq -r ".files[$i].sha256_candidate" "$s"); b=$(jq -r ".files[$i].sha256_main" "$s"); eq=$(jq -r ".files[$i].equal" "$s")
        [[ $path =~ ^[A-Za-z0-9._/-]+$ ]] || e1_refuse "manifest $f has an odd path" || return 1
        ga=$(git -C "$E1_GIT" cat-file -e "$E1_CAND:$path" 2>/dev/null && git -C "$E1_GIT" show "$E1_CAND:$path" | sha256sum | cut -c1-64 || echo ABSENT)
        gb=$(git -C "$E1_GIT" cat-file -e "$E1_MAIN:$path" 2>/dev/null && git -C "$E1_GIT" show "$E1_MAIN:$path" | sha256sum | cut -c1-64 || echo ABSENT)
        [ "$a" = "$ga" ] && [ "$b" = "$gb" ] || e1_refuse "manifest $f: digests of $path do not match git" || return 1
        if [ "$a" = "$b" ]; then [ "$eq" = true ] || e1_refuse "manifest $f: $path equal flag is wrong" || return 1
        else
            [ "$eq" = false ] || e1_refuse "manifest $f: $path equal flag is wrong" || return 1
            [ "$cls" = host ] || e1_refuse "manifest $f: chip artifact $path differs between candidate and main" || return 1
            nd=$((nd + 1))
        fi
    done
    [ "$(jq -r .chip_files_differ "$s")" = 0 ] || e1_refuse "manifest $f declares differing chip files" || return 1
    [ "$(jq -r .files_differ "$s")" = "$nd" ] || e1_refuse "manifest $f files_differ count is wrong" || return 1
    E1_HOST_DIFFER=$nd
    for pair in "GATE5:$E1_G5_BIN" "REDUCE:$E1_RED_BIN" "TRANSC:$E1_TR_BIN"; do
        a=$(jq -r --arg g "${pair%%:*}" '[.rebuilt_chip_binaries[] | select(.gate == $g) | .sha256_main] | if length == 1 then .[0] else "" end' "$s")
        [ "$a" = "${pair#*:}" ] || e1_refuse "manifest $f: rebuilt ${pair%%:*} binary from main ($a) is not the chip-run binary (${pair#*:})" || return 1
    done
    E1_LDST_BIN=$(jq -r '[.rebuilt_chip_binaries[] | select(.gate == "LDST") | .sha256_main] | if length == 1 then .[0] else "" end' "$s")
    [[ $E1_LDST_BIN =~ ^[0-9a-f]{64}$ ]] || e1_refuse "manifest $f: no rebuilt LDST binary digest" || return 1
    E1_MAN_DIGEST=$(e1_sha "$s")
    E1_WHOLE_TREE=$(jq -r .whole_tree_identical "$s")
}

# ---- combine ----

e1_combine() { # TS
    local ts=$1 body
    e1_copy "$E1_IN_MANIFEST" S_MAN && e1_copy "$E1_IN_G5" S_G5 && e1_copy "$E1_IN_RED" S_RED &&
    e1_copy "$E1_IN_TR" S_TR && e1_copy "$E1_IN_MUT" S_MUT && e1_copy "$E1_IN_CAMP" S_CAMP &&
    e1_copy "$E1_IN_LDST" S_LDST && e1_copy "$E1_IN_HOST" S_HOST && e1_copy "$E1_IN_TRP" S_TRP || return 1
    E1_S_TR=$S_TR
    e1_check_gate5 "$S_G5" "$E1_IN_G5" || return 1
    e1_check_reduce "$S_RED" "$E1_IN_RED" || return 1
    e1_check_transc "$S_TR" "$E1_IN_TR" || return 1
    e1_check_transc_prior "$S_TRP" "$E1_IN_TRP" || return 1
    e1_check_mutant "$S_MUT" "$E1_IN_MUT" || return 1
    e1_check_campaign "$S_CAMP" "$E1_IN_CAMP" || return 1
    e1_check_ldst_log "$S_LDST" "$E1_IN_LDST" || return 1
    e1_check_host_rerun "$S_HOST" "$E1_IN_HOST" || return 1
    e1_check_manifest "$S_MAN" "$E1_IN_MANIFEST" || return 1
    body="{\"schema\":\"$E1_SCHEMA\",\"gate\":\"E1_NUMERICAL_CLOSURE\",\"status\":\"PASS\",\"timestamp_utc\":\"$ts\""
    body+=",\"chip_qualified_candidate_git_commit\":\"$E1_CAND\",\"merged_main_git_commit\":\"$E1_MAIN\""
    body+=",\"physics_candidate_git_commit\":\"$E1_PHYS\",\"candidate_trees_clean\":{\"omega\":true,\"physics\":true}"
    body+=",\"chip_ran_on\":\"candidate\",\"whole_repository_trees_identical\":$E1_WHOLE_TREE"
    body+=",\"e1_artifact_set\":{\"chip_class_byte_identical\":true,\"host_class_files_differ\":$E1_HOST_DIFFER,\"host_tier_rerun_on_main\":\"PASS\",\"manifest_sha256\":\"$E1_MAN_DIGEST\"}"
    body+=",\"rebuilt_on_main_equals_chip_binary\":{\"GATE5\":\"$E1_G5_BIN\",\"REDUCE\":\"$E1_RED_BIN\",\"TRANSC\":\"$E1_TR_BIN\",\"LDST_main_rebuild\":\"$E1_LDST_BIN\"}"
    body+=",\"constituents\":{"
    body+="\"gate5\":{\"schema\":\"AIEN_OMEGA_NUMERIC_0_V1\",\"receipt_digest\":\"$E1_G5_DIGEST\",\"status\":\"PASS\",\"tests\":$E1_G5_TESTS,\"hardware_descriptor_digest\":\"$E1_G5_HW\"}"
    body+=",\"reduce\":{\"suite\":\"E1_REDUCE_GB10_PARITY\",\"sha256\":\"$E1_RED_DIGEST\",\"status\":\"PASS\",\"ops\":[\"SUM\",\"MAX\",\"MIN\",\"MEAN\"],\"cases\":380,\"mismatches\":0}"
    body+=",\"mean_mutant\":{\"log_sha256\":\"$E1_MUT_DIGEST\",\"result\":\"KILLED\",\"mean_mismatches\":$E1_MUT_MISMATCHES}"
    body+=",\"transc\":{\"gate\":\"E1-TRANSC-GB10\",\"sha256\":\"$E1_TR_DIGEST\",\"status\":\"PASS\",\"ops\":[\"SIN\",\"COS\",\"ERF\",\"GELU\",\"RSQRT\"],\"inputs_checked\":$E1_TR_INPUTS,\"per_op\":4294967296,\"exhaustive\":true,\"mismatches\":0}"
    body+=",\"transc_prior\":{\"gate\":\"E1-TRANSC-GB10\",\"sha256\":\"$E1_TRP_DIGEST\",\"omega_commit\":\"$E1_TRP_COMMIT\",\"status\":\"PASS\",\"ops\":[\"EXP2\",\"LOG2\",\"SIGMOID\",\"TANH\"],\"per_op\":4294967296,\"exhaustive\":true,\"mismatches\":0,\"kernel_digests_equal_candidate\":true}"
    body+=",\"ldst\":{\"chip_log_sha256\":\"$E1_LDST_DIGEST\",\"status\":\"PASS\",\"specs_passed\":$E1_LDST_SPECS,\"device_errors\":0}"
    body+=",\"campaign_log_sha256\":\"$E1_CAMP_DIGEST\",\"uncertain_completions\":0"
    body+=",\"host_rerun\":{\"schema\":\"AIEN_E1_HOST_RERUN_V1\",\"sha256\":\"$E1_HOST_DIGEST\",\"omega_commit\":\"$E1_MAIN\",\"lines\":[$(for l in $E1_HOST_LINES; do printf '"%s",' "$l"; done | sed 's/,$//')],\"status\":\"PASS\"}}"
    body+=",\"digest_meaning\":\"integrity only, not authenticity: re-hash the constituents named here\"}"
    E1_DIGEST=$(printf '%s' "$body" | "$JSON_CANON" --sha256) || e1_refuse "combined body is not valid JSON" || return 1
    E1_RECEIPT="{\"receipt_digest\":\"$E1_DIGEST\",${body#\{}"
}

e1_write() { # DIR
    local dir=$1 pretty
    mkdir -p "$dir" || e1_refuse "cannot create $dir" || return 1
    pretty=$E1_TMP/combined.pretty
    printf '%s' "$E1_RECEIPT" | "$JSON_CANON" --pretty > "$pretty" || e1_refuse "cannot format receipt" || return 1
    "$JSON_CANON" --write-exclusive "$dir/$E1_DIGEST.json" < "$pretty" 2> "$pretty.err" ||
        e1_refuse "cannot write $dir/$E1_DIGEST.json: $(cat "$pretty.err")" || return 1
    chmod 0444 "$dir/$E1_DIGEST.json" || e1_refuse "cannot chmod $dir/$E1_DIGEST.json"
}

e1_dup() { echo "REFUSED: duplicate input $1" >&2; exit 1; }
e1_usage() { sed -n '4,6p' "${BASH_SOURCE[0]}" | sed 's/^# *//' >&2; exit 2; }

e1_main() {
    local ev= ts
    E1_CAND= E1_MAIN= E1_PHYS= E1_IN_MANIFEST= E1_IN_G5= E1_IN_RED= E1_IN_TR= E1_IN_MUT= E1_IN_CAMP= E1_IN_LDST= E1_IN_HOST= E1_IN_TRP=
    while [ $# -gt 0 ]; do
        [ $# -ge 2 ] || e1_usage
        case $1 in
            --candidate) E1_CAND=${2,,};; --main) E1_MAIN=${2,,};; --physics) E1_PHYS=${2,,};;
            --manifest) [ -z "$E1_IN_MANIFEST" ] || e1_dup "$1"; E1_IN_MANIFEST=$2;;
            --gate5) [ -z "$E1_IN_G5" ] || e1_dup "$1"; E1_IN_G5=$2;;
            --reduce) [ -z "$E1_IN_RED" ] || e1_dup "$1"; E1_IN_RED=$2;;
            --transc) [ -z "$E1_IN_TR" ] || e1_dup "$1"; E1_IN_TR=$2;;
            --mutant-log) [ -z "$E1_IN_MUT" ] || e1_dup "$1"; E1_IN_MUT=$2;;
            --campaign-log) [ -z "$E1_IN_CAMP" ] || e1_dup "$1"; E1_IN_CAMP=$2;;
            --ldst-chip-log) [ -z "$E1_IN_LDST" ] || e1_dup "$1"; E1_IN_LDST=$2;;
            --host-rerun) [ -z "$E1_IN_HOST" ] || e1_dup "$1"; E1_IN_HOST=$2;;
            --transc-prior) [ -z "$E1_IN_TRP" ] || e1_dup "$1"; E1_IN_TRP=$2;;
            --evidence-dir) ev=$2;;
            *) e1_usage;;
        esac
        shift 2
    done
    for c in "$E1_CAND" "$E1_MAIN" "$E1_PHYS"; do
        [[ $c =~ ^[0-9a-f]{40}$ ]] || { echo "--candidate, --main and --physics must be full 40-hex SHAs" >&2; exit 2; }
    done
    e1_build_canon || { echo "REFUSED: cannot build tools/json_canon.c" >&2; exit 1; }
    trap 'rm -rf "$E1_TMP"' EXIT
    ts=${E1_TIMESTAMP_UTC:-$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ)}   # override: tests only
    e1_combine "$ts" || { echo "REFUSED: $E1_ERR" >&2; exit 1; }
    if [ -n "$ev" ]; then
        e1_write "$ev" || { echo "REFUSED: $E1_ERR" >&2; exit 1; }
        echo "E1 CLOSURE PASS: receipt $ev/$E1_DIGEST.json"
    else
        echo "E1 CLOSURE PASS: receipt digest $E1_DIGEST (not written; no --evidence-dir)"
    fi
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    e1_main "$@"
fi
