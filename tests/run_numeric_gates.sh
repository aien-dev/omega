#!/bin/bash
# tests/run_numeric_gates.sh -- M19R Gate 5 (OMEGA-NUMERIC-0) qualification.
#
#   tests/run_numeric_gates.sh --omega-candidate SHA --physics-candidate SHA
#                              [--physics-dir DIR] [--evidence-dir DIR] [--record]
#
# Needs the GB10: the test binary launches kernels on the chip. The build and
# the run happen under /tmp/aien-gb10.lock.
#
# Checks, in order:
#   1. both candidates are explicit full 40-hex SHAs equal to HEAD of a clean
#      tree (omega = this repo, physics = --physics-dir, default ../physics);
#   2. physics.lock names the physics candidate, and the physics checkout is at
#      that commit (the dependency comes from the pin file, never a fixed path);
#   3. the pinned physics commit has the forge hardware probe (forge/); if not
#      the run stops and says so (moving the pin is an owner decision);
#   4. clean build into build/qual-runs/<run-id>/ with -ffp-contract=off, no
#      libm and no CUDA symbols in the binary;
#   5. the binary runs with OMEGA_NUMERIC_RUN_ID set; a nonzero exit status
#      fails the run even when every printed record passes;
#   6. the log: every expected test ID printed exactly once as PASS, no SKIP;
#      one run line whose run id is this run and whose binary digest (the
#      binary hashes /proc/self/exe) is the file that was built; registry
#      lines equal to the manifest below; for every encoded op exactly the
#      expected GB10 parity lines (FFMA: one per uniform c in NUM_FFMA_C, the
#      others one) with the comparison mode taken from the manifest, never
#      from the log, full corpus size, typed integer counts and zero
#      mismatches; a hardware descriptor probed from the device (a fake one
#      is refused);
#   7. receipt time: binary and log digests unchanged since the run, both
#      candidates re-checked, run commit re-read from git, evidence/
#      unchanged.
#
# Output: build/qual-runs/<run-id>/{build.log,gate5.log,gate5.stderr,gate5.status,run.json}
# always; receipt-preview.json when every check passed. With --record a
# permanent receipt <evidence-dir>/<receipt_digest>.json is written
# for every run that got past argument parsing: "status":"PASS" when every
# check passed, "status":"FAIL" (with the reason) otherwise. Receipts are
# append-only: created exclusively, mode 0444, never overwritten. A PASS
# receipt is written only for the repository this script belongs to. The
# evidence directory defaults to ~/workspace/evidence-out/OMEGA-NUMERIC-0 and
# is refused (exit 2) when it lies inside the omega tree or the physics
# checkout, so recording a receipt never dirties a candidate tree; run.json
# records whether both trees were still clean afterwards. Each receipt binds
# the run id, the omega and physics commits with their clean flags, and the
# gate binary digest.
# The receipt digest shows the receipt was not altered after it was written;
# it is not a signature and does not prove who wrote it. With --record the
# gate log, stderr and binary it names are copied beside it as
# <evidence-dir>/blobs/<sha256>.{log,stderr,bin} (mode 0444) so anyone can
# re-hash them and re-run the log checks.
# The historical file evidence/m19r_gate5_omega_numeric_evidence.json is not
# touched; its hash is recorded as the predecessor.
# All 40 ops in the manifest (the 15 original ops, the 23 E1 scalar ops, and
# DIV and SQRT, which run the E1 row 7 whole-program kernels),
# LDS_STS and REDUCE_SUM included, need one GB10
# parity line each (FFMA one per c) at n=4096 with zero mismatches against the
# CPU reference. REDUCE_SUM checks lane 0 of each warp (n/32 sums) and must
# name the declared summation order. A PASS needs a real chip run; nothing
# here can produce one without the GB10.
# Exit 0 on PASS, 1 on failure, 2 on bad arguments.
#
# Functions can be sourced (tools/test_numeric_qualify.sh does); main only
# runs when the file is executed. Shell + coreutils + git + jq + gcc. No Python.

NUM_SELF=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")
# shellcheck source=../tools/m19r_qualify.sh
. "$(dirname "$NUM_SELF")/../tools/m19r_qualify.sh"
M19R_PROG=run_numeric_gates.sh

NUM_SUITE=gate5
NUM_HISTORICAL=evidence/m19r_gate5_omega_numeric_evidence.json
# Permanent receipts go here, outside every candidate tree (--evidence-dir).
NUM_EVIDENCE_DEFAULT=$HOME/workspace/evidence-out/OMEGA-NUMERIC-0
# Test IDs the chip build must print, each exactly once, each PASS.
NUM_EXPECTED_IDS="FPCR_RNE_NO_FTZ_REQUIRED PROVENANCE_MATCHES_EXECUTOR
NOT_ENCODED_OPS_REFUSED_BEFORE_SUBMISSION FP32_SIMT_OPCODES_ENCODED
OMEGA_MATH_SEQUENCES_QUALIFIED WARP_REDUCTION_ORDER_DECLARED
CPU_TIER_EQUALS_REFERENCE CPU_TIER_SUBNORMALS_PRESERVED CPU_TIER_INDEPENDENT_ORACLE
CPU_GB10_BIT_PARITY
SUBNORMALS_PRESERVED_NO_FTZ MUFU_SEED_ONLY_NOT_COMPARED
EDGE_CLASS_BEHAVIOR_VERIFIED HARDWARE_DESCRIPTOR_PROBED
NEG_FTZ_DETECTED_AND_REJECTED NEG_UNORDERED_REDUCTION_DIVERGENCE_CAUGHT
NEG_RAW_MUFU_APPROX_REJECTED_WITHOUT_REFINEMENT NEG_UNKNOWN_OPCODE_FAILS_CLOSED
NEG_OPCODE_PROVENANCE_INTEGRITY_VERIFIED NEG_NONDEFAULT_FPCR_REFUSED
NEG_COMPARATOR_CATCHES_ONE_BIT NEG_BAD_SHARED_AND_WARP_SHAPES_REFUSED
NEG_PATCH_STRUCTURE_CHECKED_BEFORE_SUBMISSION
E1_SCALAR_BOUNDARY_VALUES E1_SCALAR_CPU_EQUALS_REFERENCE E1_SCALAR_INDEPENDENT_ORACLE"
# The operation manifest: every op the executor encodes for GB10 and how its
# result is compared. It mirrors OP_TABLE in src/omega_numeric.c (the host
# test checks the two agree). The log never chooses its own comparison.
NUM_OP_MANIFEST="FADD:BIT_EXACT FSUB:BIT_EXACT FMUL:BIT_EXACT FFMA:BIT_EXACT
FSETP_SEL:INT_EXACT FSEL:INT_EXACT FMNMX_MIN:BIT_EXACT FMNMX_MAX:BIT_EXACT
I2FP:BIT_EXACT F2I:INT_EXACT MUFU_RCP:SEED_BOUND MUFU_RSQ:SEED_BOUND
SHFL_DOWN:INT_EXACT LDS_STS:INT_EXACT REDUCE_SUM:BIT_EXACT
FSETP_LT_SEL:INT_EXACT FSETP_LE_SEL:INT_EXACT FSETP_GT_SEL:INT_EXACT FSETP_EQ_SEL:INT_EXACT
FSETP_NE_SEL:INT_EXACT FSETP_NUM_SEL:INT_EXACT FSETP_NAN_SEL:INT_EXACT FSETP_LTU_SEL:INT_EXACT
FSETP_LEU_SEL:INT_EXACT FSETP_GTU_SEL:INT_EXACT FSETP_GEU_SEL:INT_EXACT FSETP_EQU_SEL:INT_EXACT
FSETP_NEU_SEL:INT_EXACT F2I_FLOOR:INT_EXACT F2I_CEIL:INT_EXACT F2I_RNI:INT_EXACT F2U:INT_EXACT
I2FP_U32:BIT_EXACT F32_TO_F16:F16_BITS F32_TO_BF16:BF16_BITS F16_TO_F32:BIT_EXACT
BF16_TO_F32:BIT_EXACT FFMA_V:BIT_EXACT DIV:BIT_EXACT SQRT:BIT_EXACT"
NUM_ENCODED_OPS=$(printf '%s\n' $NUM_OP_MANIFEST | cut -d: -f1 | tr '\n' ' ')
# Elements per launch (N in tests/test_omega_numeric.c).
NUM_CORPUS_N=4096
# FFMA runs once per uniform c (FFMA_C in tests/test_omega_numeric.c).
NUM_FFMA_C="0x3f800000 0x80000000 0x00800000 0xbf800000 0x7fc00000 0x7f800000
0x00000003 0x80800000"
# REDUCE_SUM: only lane 0 of each 32-lane warp holds a sum, so it checks N/32
# elements, and its parity line must name this summation order (the order
# the chip kernel and the CPU reference both use: SHFL.DOWN deltas 16, 8, 4,
# 2, 1, one FADD after each; OMEGA_WARP_REDUCTION_DECLARED_ORDER).
NUM_REDUCE_ORDER=PAIRWISE_TREE_LANE_DELTA_16_8_4_2_1

# num_manifest_json -- {"ops":{op:mode},"n":N,"ffma_c":[...],
#   "checked":{op:count} (ops that do not check all N), "reduction_order":{op:order}}
num_manifest_json() {
    jq -n --arg m "$NUM_OP_MANIFEST" --arg c "$NUM_FFMA_C" --argjson n "$NUM_CORPUS_N" --arg ro "$NUM_REDUCE_ORDER" '
        {ops: ([$m | splits("\\s+") | select(length > 0) | split(":") | {key: .[0], value: .[1]}]
               | from_entries),
         n: $n, ffma_c: [$c | splits("\\s+") | select(length > 0)],
         checked: {REDUCE_SUM: ($n / 32 | floor)}, reduction_order: {REDUCE_SUM: $ro}}'
}

# num_check_physics OMEGA PHYSICS_DIR PHYSICS_CAND -- the physics dependency
# is the commit in physics.lock; the checkout must be at it and carry forge/.
num_check_physics() {
    local omega=$1 physics=$2 cand=$3 locked head f
    [ -r "$omega/physics.lock" ] || m19r_fail "cannot read $omega/physics.lock" || return 1
    locked=$(m19r_strip "$(cat "$omega/physics.lock")")
    [[ $locked =~ ^[0-9a-fA-F]{40}$ ]] ||
        m19r_fail "physics.lock does not hold a full 40-hex commit" || return 1
    [ "${locked,,}" = "${cand,,}" ] ||
        m19r_fail "physics.lock pins $locked but --physics-candidate is $cand" || return 1
    [ -d "$physics" ] ||
        m19r_fail "physics checkout $physics does not exist (pinned commit $locked)" || return 1
    m19r_git "$physics" rev-parse HEAD ||
        m19r_fail "physics checkout $physics is not a git repository" || return 1
    head=$M19R_GIT
    [ "${head,,}" = "${locked,,}" ] ||
        m19r_fail "physics checkout $physics is at $head but physics.lock pins $locked" || return 1
    m19r_must_candidate "$physics" "$cand" || return 1
    for f in forge/forge_descriptor.h forge/forge_descriptor.c forge/forge_realize.c \
             sha256_clean.c nvrm/nvrm.c m16/m16_native.c; do
        [ -f "$physics/$f" ] || m19r_fail "pinned physics commit $locked has no $f; Gate 5 needs the forge hardware probe, so physics.lock must move to a commit that has it (owner decision, not done by this script)" || return 1
    done
}

# num_build OMEGA PHYSICS OUT LOG -- build the chip test binary.
num_build() {
    local omega=$1 p=$2 out=$3 log=$4 nv
    nv=$p/third_party/nvidia-open-580.173.02
    (cd "$omega" && gcc -std=gnu11 -O2 -Wall -Wextra -Werror -ffp-contract=off \
        -Isrc -I"$p/forge" -I"$p/nvrm" -I"$p/m16" \
        -I"$nv/src/common/sdk/nvidia/inc" -I"$nv/kernel-open/common/inc" \
        -I"$nv/kernel-open/nvidia-uvm" -I"$nv/src/nvidia/arch/nvalloc/unix/include" \
        -o "$out" tests/test_omega_numeric.c src/omega_numeric.c src/omega_numeric_gb10.c \
        src/omega_numeric_divsqrt_gb10.c \
        src/omega_numeric_provenance.c src/omega_blackwell_codegen.c \
        src/omega_blackwell_encoder.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c \
        src/forge_realization.c src/aegis_verification.c src/sha256.c \
        "$p/forge/forge_descriptor.c" "$p/forge/forge_realize.c" "$p/sha256_clean.c" \
        "$p/nvrm/nvrm.c" "$p/m16/m16_native.c") > "$log" 2>&1 ||
        m19r_fail "build failed; see $log" || return 1
    if nm -u "$out" | grep -Eq '\b(sqrtf?|expf?|logf?|powf?|fmaf?|sinf?|cosf?|roundf?|fabsf?)\b'; then
        m19r_fail "libm math symbols found in $out"; return 1
    fi
    if nm -u "$out" | grep -Eq "$M19R_CUDA_RE"; then
        m19r_fail "CUDA symbols found in $out"; return 1
    fi
}

# num_execute OMEGA BIN -- run the gate binary once for this run. Writes
# $NUM_RUN_DIR/gate5.log and gate5.status, sets NUM_EXEC_RC and NUM_LOG_SHA.
# A nonzero exit status fails, whatever the log says.
num_execute() {
    local omega=$1 bin=$2 rc
    NUM_EXEC_RC= NUM_LOG_SHA= NUM_STDERR_SHA=
    # stdout and stderr go to separate files: the binary's stderr (refusal
    # messages) is unbuffered and would otherwise land inside half-written
    # stdout JSON lines (seen on the chip run 20260930T164139Z-890338e1a62e).
    (cd "$omega" && exec env OMEGA_NUMERIC_RUN_ID="$NUM_RUN_ID" "$bin" 9>&-) \
        > "$NUM_RUN_DIR/gate5.log" 2> "$NUM_RUN_DIR/gate5.stderr"
    rc=$?
    NUM_EXEC_RC=$rc
    printf '%s\n' "$rc" > "$NUM_RUN_DIR/gate5.status"
    NUM_LOG_SHA=$(m19r_sha_file "$NUM_RUN_DIR/gate5.log")
    NUM_STDERR_SHA=$(m19r_sha_file "$NUM_RUN_DIR/gate5.stderr")
    [ "$rc" -eq 0 ] ||
        m19r_fail "gate binary exited with status $rc; a crash or late failure disqualifies the run" || return 1
}

# jq: the first problem with the GB10 parity lines against the manifest, or "".
# Input: array of GB10 parity lines. $man: num_manifest_json.
NUM_JQ_PARITY='
def isint: type == "number" and . == floor and . >= 0;
def problem($op; $m):
  . as $l
  | ($man.checked[$op] // $man.n) as $need
  | ($man.reduction_order[$op] // null) as $order
  | if ($l | has("error")) then "a launch error (\($l.error | tojson))"
    elif $l.reduction_order != $order then "reduction_order \($l.reduction_order | tojson), the manifest says \($order | tojson)"
    elif ($l.compare != $m) then "comparison \($l.compare | tojson) but the manifest says \($m)"
    elif (($l.n | isint) | not) or $l.n != $man.n then "n \($l.n | tojson), need \($man.n)"
    elif (($l.checked | isint) | not) then "checked is not an integer"
    elif $m == "SEED_BOUND" then
      if (($l.out_of_bound | isint) | not) or (($l.skipped | isint) | not) then "seed-bound counts are not integers"
      elif $l.checked == 0 then "no element checked"
      elif $l.checked + $l.skipped != $l.n then "checked + skipped != n"
      elif $l.out_of_bound != 0 then "\($l.out_of_bound) results out of bound"
      else "" end
    else
      if $l.checked != $need then "checked \($l.checked), need \($need)"
      elif (($l.mismatches | isint) | not) then "mismatches is not an integer"
      elif $l.mismatches != 0 then "\($l.mismatches) mismatches"
      else "" end
    end;
. as $all
| ([$all[] | select(.tier != "gb10") | .op] | first) as $badtier
| ([$all[] | .op as $o | select(($man.ops | has($o)) | not) | $o] | first) as $stray
| if $badtier != null then "a parity line for \($badtier | tojson) is not tier gb10"
  elif $stray != null then "a GB10 parity line names \($stray | tojson), outside the encoded set"
  else
    [ $man.ops | to_entries[] | .key as $op | .value as $mode
      | [$all[] | select(.op == $op)] as $ls
      | if ($ls | length) == 0 then "no GB10 parity line for \($op)"
        elif $op == "FFMA" then
          if ([$ls[] | .c_bits] | sort) != ($man.ffma_c | sort)
          then "FFMA launches \([$ls[] | .c_bits] | tojson), need exactly one per c in \($man.ffma_c | tojson)"
          else ([$ls[] | problem($op; $mode) | select(. != "") | "GB10 parity for FFMA: " + .] | first // "") end
        elif ($ls | length) != 1 then "GB10 parity for \($op): \($ls | length) lines, need exactly 1"
        elif ($ls[0] | has("c_bits")) then "GB10 parity for \($op) carries c_bits"
        else ($ls[0] | problem($op; $mode) | if . == "" then "" else "GB10 parity for \($op): " + . end) end
      | select(. != "") ] | first // ""
  end'

# jq: the first problem with the registry lines against the manifest, or "".
NUM_JQ_REGISTRY='
([.[] | select(.encoded == true)]) as $enc
| if ([.[] | .op] | length) != ([.[] | .op] | unique | length) then "an op appears twice in the registry lines"
  elif ([$enc[] | .op] | sort) != ($man.ops | keys | sort)
  then "registry encodes \([$enc[] | .op] | sort | tojson), manifest lists \($man.ops | keys | sort | tojson)"
  else ([$enc[] | select(.compare != $man.ops[.op]) | "registry compares \(.op) as \(.compare | tojson), manifest says \($man.ops[.op])"]
        + [$enc[] | select(.launches != (if .op == "FFMA" then ($man.ffma_c | length) else 1 end))
           | "registry has \(.launches | tojson) launches for \(.op)"]) | first // ""
  end'

# num_check_log LOG -- validate the gate log. Sets NUM_TSV (events file),
# NUM_HWDESC, NUM_HWDIGEST, NUM_PARITY (JSON array of GB10 parity lines),
# NUM_LOG_RUN_ID, NUM_LOG_BINARY_SHA (from the run line).
num_check_log() {
    local log=$1 id n hw man reg run problem
    [ -r "$log" ] || m19r_fail "cannot read gate log $log" || return 1
    if grep -Eq '^[[:space:]]*\[SKIP\]' "$log"; then
        m19r_fail "gate log has SKIP results; a qualifying run executes every test"; return 1
    fi
    NUM_TSV=$M19R_TMP/gate5.tsv
    m19r_events "$log" "$NUM_SUITE" > "$NUM_TSV"
    for id in $NUM_EXPECTED_IDS; do
        n=$(awk -F'\t' -v i="$id" '$2 == i' "$NUM_TSV" | grep -c .)
        [ "$n" -eq 1 ] || m19r_fail "expected test $id printed $n times (need exactly 1)" || return 1
    done
    n=$(grep -c . "$NUM_TSV")
    [ "$n" -eq "$(printf '%s\n' $NUM_EXPECTED_IDS | grep -c .)" ] ||
        m19r_fail "gate log has $n results; the manifest lists $(printf '%s\n' $NUM_EXPECTED_IDS | grep -c .)" || return 1
    m19r_require_passed < "$NUM_TSV" || return 1

    run=$(m19r_tagged_json "$log" OMEGA_NUMERIC_RUN_JSON) || return 1
    [ "$(printf '%s\n' "$run" | grep -c .)" -eq 1 ] ||
        m19r_fail "expected exactly one run line (OMEGA_NUMERIC_RUN_JSON)" || return 1
    NUM_LOG_RUN_ID=$(printf '%s' "$run" | jq -r 'if (.run_id | type) == "string" then .run_id else "" end')
    NUM_LOG_BINARY_SHA=$(printf '%s' "$run" | jq -r 'if (.binary_sha256 | type) == "string" then .binary_sha256 else "" end')
    [ -n "$NUM_LOG_RUN_ID" ] || m19r_fail "run line has no run id" || return 1
    [[ $NUM_LOG_BINARY_SHA =~ ^[0-9a-f]{64}$ ]] ||
        m19r_fail "run line has no binary digest" || return 1

    hw=$(m19r_tagged_json "$log" OMEGA_NUMERIC_HWDESC_JSON) || return 1
    [ "$(printf '%s\n' "$hw" | grep -c .)" -eq 1 ] ||
        m19r_fail "expected exactly one hardware descriptor line" || return 1
    [ "$(printf '%s' "$hw" | jq -r '.source')" = FORGE_PROBE ] ||
        m19r_fail "hardware descriptor is not a device probe (source $(printf '%s' "$hw" | jq -c '.source')); refusing" || return 1
    [ "$(printf '%s' "$hw" | jq -r '.fake // false')" = false ] ||
        m19r_fail "hardware descriptor is marked fake; refusing" || return 1
    NUM_HWDIGEST=$(printf '%s' "$hw" | jq -r '.descriptor_digest')
    [[ $NUM_HWDIGEST =~ ^[0-9a-f]{64}$ ]] && [ "$NUM_HWDIGEST" != "$(printf '0%.0s' {1..64})" ] ||
        m19r_fail "hardware descriptor digest is missing or all zero" || return 1
    NUM_HWDESC=$hw

    man=$(num_manifest_json) || m19r_fail "cannot build the operation manifest" || return 1
    reg=$(m19r_tagged_json "$log" OMEGA_NUMERIC_REGISTRY_JSON | jq -sc .) ||
        m19r_fail "cannot read registry lines" || return 1
    problem=$(printf '%s' "$reg" | jq -r --argjson man "$man" "$NUM_JQ_REGISTRY") ||
        m19r_fail "cannot check registry lines" || return 1
    [ -z "$problem" ] || m19r_fail "$problem" || return 1

    NUM_PARITY=$(m19r_tagged_json "$log" OMEGA_NUMERIC_PARITY_JSON | jq -sc '[.[] | select(.tier == "gb10")]') ||
        m19r_fail "cannot read GB10 parity lines" || return 1
    problem=$(printf '%s' "$NUM_PARITY" | jq -r --argjson man "$man" "$NUM_JQ_PARITY") ||
        m19r_fail "cannot check GB10 parity lines" || return 1
    [ -z "$problem" ] || m19r_fail "$problem" || return 1
}

# num_under PATH DIR -- true when PATH is DIR or inside it (both resolved).
num_under() {
    local p d
    p=$(realpath -m "$1") && d=$(realpath -m "$2") || return 1
    [ "$p" = "$d" ] || [[ $p == "$d"/* ]]
}

# num_check_evidence_dir OMEGA -- resolve NUM_EVIDENCE_DIR (default
# $NUM_EVIDENCE_DEFAULT) and refuse it when it lies inside the omega tree,
# this script's repository or the physics checkout: a receipt written there
# would dirty the very tree it certifies as clean.
num_check_evidence_dir() {
    local omega=$1 d
    d=$(realpath -m "${NUM_EVIDENCE_DIR:-$NUM_EVIDENCE_DEFAULT}") ||
        m19r_fail "cannot resolve evidence directory ${NUM_EVIDENCE_DIR:-$NUM_EVIDENCE_DEFAULT}" || return 1
    num_outside_trees "$d" "$omega" "evidence directory" || return 1
    NUM_EVIDENCE_DIR=$d
}

# num_outside_trees PATH OMEGA WHAT -- refuse PATH (already resolved) when it
# lies inside the omega tree, this script's repository or the physics checkout.
num_outside_trees() {
    local d=$1 omega=$2 what=$3 t
    for t in "$omega" "${M19R_OMEGA:-}" "${M19R_PHYSICS:-}"; do
        [ -n "$t" ] || continue
        if num_under "$d" "$t"; then
            m19r_fail "$what $d is inside $(realpath -m "$t"); receipts must live outside the candidate trees" || return 1
        fi
    done
}


# num_tree_clean REPO -- "true" or "false" from git status right now.
num_tree_clean() {
    if m19r_git "$1" status --porcelain --untracked-files=normal && [ -z "$M19R_GIT" ]; then
        echo true
    else
        echo false
    fi
}

# num_keep_blob FILE EXT OMEGA -- copy FILE into NUM_EVIDENCE_DIR/blobs/<sha256>.EXT
# (mode 0444). An existing blob must already hash to that name. Receipts name
# the gate log, stderr and binary by digest; the digest shows the receipt was
# not altered, it does not prove who wrote it, so the named bytes are kept
# beside it for anyone to re-hash and re-check.
num_keep_blob() {
    local src=$1 ext=$2 omega=$3 sha dst tmp bdir
    [ -f "$src" ] || return 0
    sha=$(m19r_sha_file "$src") && dst=$NUM_EVIDENCE_DIR/blobs/$sha.$ext
    mkdir -p "$NUM_EVIDENCE_DIR/blobs" || m19r_fail "cannot create $NUM_EVIDENCE_DIR/blobs" || return 1
    # blobs/ may be a symlink planted into a candidate tree: resolve it and
    # check where it really points before anything is written through it.
    bdir=$(realpath -e "$NUM_EVIDENCE_DIR/blobs") && [ -d "$bdir" ] ||
        m19r_fail "cannot resolve $NUM_EVIDENCE_DIR/blobs" || return 1
    num_outside_trees "$bdir" "$omega" "blob directory" || return 1
    dst=$bdir/$sha.$ext
    if [ -e "$dst" ]; then
        [ "$(m19r_sha_file "$dst")" = "$sha" ] || m19r_fail "blob $dst does not hash to its name" || return 1
        return 0
    fi
    tmp=$(mktemp "$bdir/.tmp.XXXXXX") || return 1
    cp "$src" "$tmp" && chmod 0444 "$tmp" && [ "$(m19r_sha_file "$tmp")" = "$sha" ] &&
        mv -n "$tmp" "$dst" && [ ! -e "$tmp" ] ||
        { rm -f "$tmp"; m19r_fail "cannot keep blob $dst"; return 1; }
}

# num_write_receipt OMEGA BODY [PREVIEW=1] -- digest the body, write the
# preview (PASS only), and with NUM_RECORD=1 the permanent append-only
# receipt NUM_EVIDENCE_DIR/<digest>.json (never inside a candidate tree).
# Sets NUM_DIGEST and NUM_PERMANENT.
num_write_receipt() {
    local omega=$1 body=$2 preview=${3:-1} digest receipt
    # The digest makes alteration detectable; it is not a signature and does
    # not prove who wrote the receipt. The bytes it names are kept in blobs/.
    body="${body%\}},\"digest_meaning\":\"integrity only, not authenticity: re-hash blobs/<sha256>.{log,stderr,bin} named here\"}"
    digest=$(printf '%s' "$body" | "$JSON_CANON" --sha256) ||
        m19r_fail "receipt body is not valid JSON" || return 1
    receipt="{\"receipt_digest\":\"$digest\",${body#\{}"
    if [ "$preview" = 1 ] && [ -n "${NUM_RUN_DIR:-}" ] && [ -d "$NUM_RUN_DIR" ]; then
        printf '%s' "$receipt" | "$JSON_CANON" --pretty > "$NUM_RUN_DIR/receipt-preview.json" ||
            m19r_fail "cannot write receipt preview" || return 1
    fi
    NUM_DIGEST=$digest
    if [ "${NUM_RECORD:-0}" = 1 ]; then
        num_check_evidence_dir "$omega" || return 1
        if [ -n "${NUM_RUN_DIR:-}" ]; then
            num_keep_blob "$NUM_RUN_DIR/gate5.log" log "$omega" || return 1
            num_keep_blob "$NUM_RUN_DIR/gate5.stderr" stderr "$omega" || return 1
            num_keep_blob "$NUM_RUN_DIR/test_omega_numeric" bin "$omega" || return 1
        fi
        NUM_PERMANENT=$NUM_EVIDENCE_DIR/$digest.json
        m19r_write_immutable_receipt "$NUM_PERMANENT" <<< "$receipt" || { NUM_PERMANENT=; return 1; }
    fi
}

# num_receipt OMEGA LOG -- revalidate everything this run produced and write
# the PASS receipt. Nothing supplied by the caller is taken on trust: the log
# must be this run's gate5.log with the digest recorded when the binary
# exited 0, the binary must still hash to NUM_BINARY_SHA and match the run
# line, both candidates are re-checked and the run commit is re-read from git.
# Needs NUM_OMEGA_CAND NUM_PHYSICS_CAND NUM_RUN_COMMIT NUM_BINARY_SHA
# NUM_EXEC_RC NUM_LOG_SHA NUM_RUN_ID NUM_TS NUM_RUN_DIR M19R_PHYSICS.
num_receipt() {
    local omega=$1 log=$2 bin bin_sha head status manifest manifest_digest counts body pred
    local omega_clean physics_clean
    [ -n "${NUM_RUN_DIR:-}" ] && [ "$log" = "$NUM_RUN_DIR/gate5.log" ] ||
        m19r_fail "log $log is not this run's gate5.log" || return 1
    [ "${NUM_EXEC_RC:-}" = 0 ] ||
        m19r_fail "gate binary exit status is ${NUM_EXEC_RC:-unknown}, need 0" || return 1
    status=$(cat "$NUM_RUN_DIR/gate5.status" 2>/dev/null)
    [ "$status" = 0 ] || m19r_fail "recorded gate binary status is '${status}', need 0" || return 1
    [ -f "$log" ] && [ "$(m19r_sha_file "$log")" = "${NUM_LOG_SHA:-}" ] ||
        m19r_fail "gate log changed since the run (or no run recorded its digest)" || return 1
    [ -f "$NUM_RUN_DIR/gate5.stderr" ] && [ "$(m19r_sha_file "$NUM_RUN_DIR/gate5.stderr")" = "${NUM_STDERR_SHA:-}" ] ||
        m19r_fail "gate stderr changed since the run (or no run recorded its digest)" || return 1
    bin=$NUM_RUN_DIR/test_omega_numeric
    [ -f "$bin" ] || m19r_fail "gate binary $bin is missing" || return 1
    bin_sha=$(m19r_sha_file "$bin")
    [ "$bin_sha" = "${NUM_BINARY_SHA:-}" ] ||
        m19r_fail "gate binary digest $bin_sha differs from the built binary ${NUM_BINARY_SHA:-(none)}" || return 1
    num_check_log "$log" || return 1
    [ "$NUM_LOG_RUN_ID" = "$NUM_RUN_ID" ] ||
        m19r_fail "log run id $NUM_LOG_RUN_ID is not this run ($NUM_RUN_ID)" || return 1
    [ "$NUM_LOG_BINARY_SHA" = "$bin_sha" ] ||
        m19r_fail "log was produced by binary $NUM_LOG_BINARY_SHA, not $bin_sha" || return 1
    m19r_must_candidate "$omega" "$NUM_OMEGA_CAND" || return 1
    num_check_physics "$omega" "$M19R_PHYSICS" "$NUM_PHYSICS_CAND" || return 1
    m19r_git "$omega" rev-parse HEAD || return 1
    head=$M19R_GIT
    [ "${head,,}" = "${NUM_RUN_COMMIT,,}" ] && [ "${head,,}" = "${NUM_OMEGA_CAND,,}" ] ||
        m19r_fail "run commit ${NUM_RUN_COMMIT} / HEAD $head differ from candidate $NUM_OMEGA_CAND" || return 1
    omega_clean=$(num_tree_clean "$omega")
    physics_clean=$(num_tree_clean "$M19R_PHYSICS")
    [ "$omega_clean" = true ] && [ "$physics_clean" = true ] ||
        m19r_fail "candidate trees not clean at receipt time" || return 1
    [[ $NUM_TS =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:.]+Z$ ]] || m19r_fail "missing UTC timestamp" || return 1
    if [ "${NUM_RECORD:-0}" = 1 ] && [ "$(cd -P "$omega" && pwd)" != "$M19R_OMEGA" ]; then
        m19r_fail "a recorded receipt certifies only the checkout this script runs from ($M19R_OMEGA), not $omega" || return 1
    fi
    pred=null
    [ ! -f "$omega/$NUM_HISTORICAL" ] || pred="\"$(m19r_sha_file "$omega/$NUM_HISTORICAL")\""
    manifest="{\"command\":\"tests/test_omega_numeric\",\"expected_test_ids\":$(printf '%s\n' $NUM_EXPECTED_IDS | jq -Rn '[inputs]'),\"encoded_ops\":$(printf '%s\n' $NUM_ENCODED_OPS | jq -Rn '[inputs]'),\"operation_manifest\":$(num_manifest_json)}"
    manifest_digest=$(printf '%s' "$manifest" | "$JSON_CANON" --sha256) || return 1
    counts=$(m19r_observed_counts < "$NUM_TSV")
    body="{\"schema\":\"AIEN_OMEGA_NUMERIC_0_V1\",\"gate\":\"M19R_GATE5_OMEGA_NUMERIC_0\",\"status\":\"PASS\""
    body+=",\"run_id\":$(m19r_jstr "$NUM_RUN_ID"),\"timestamp_utc\":\"$NUM_TS\""
    body+=",\"candidate_git_commit\":\"${NUM_OMEGA_CAND,,}\",\"run_git_commit\":\"${head,,}\""
    body+=",\"physics_candidate_git_commit\":\"${NUM_PHYSICS_CAND,,}\""
    body+=",\"physics_lock\":\"$(m19r_strip "$(cat "$omega/physics.lock")" | tr 'A-F' 'a-f')\""
    body+=",\"candidate_trees_clean\":{\"omega\":$omega_clean,\"physics\":$physics_clean}"
    body+=",\"candidate_binary_sha256\":\"$bin_sha\",\"gate_binary_exit_status\":0"
    body+=",\"gate_log_sha256\":\"$NUM_LOG_SHA\""
    body+=",\"gate_stderr_sha256\":\"$NUM_STDERR_SHA\""
    body+=",\"test_manifest_sha256\":\"$manifest_digest\",\"test_manifest\":$manifest"
    body+=",\"test_results\":$(m19r_events_json < "$NUM_TSV")"
    body+=",\"observed_test_count\":$(jq -n --argjson c "$counts" '$c.completed')"
    body+=",\"observed_pass_count\":$(jq -n --argjson c "$counts" '$c.passed')"
    body+=",\"observed_fail_count\":$(jq -n --argjson c "$counts" '$c.failed')"
    body+=",\"hardware_descriptor\":$NUM_HWDESC,\"hardware_descriptor_digest\":\"$NUM_HWDIGEST\""
    body+=",\"gb10_parity\":$NUM_PARITY"
    body+=",\"predecessor_historical_gate5_sha256\":$pred,\"zero_libm_zero_libcuda\":true}"
    num_write_receipt "$omega" "$body"
}

# num_fail_receipt OMEGA REASON -- the FAIL receipt for a run that did not
# qualify: what was observed, nothing claimed. Only with NUM_RECORD=1.
num_fail_receipt() {
    local omega=$1 reason=$2 body f v
    body="{\"schema\":\"AIEN_OMEGA_NUMERIC_0_V1\",\"gate\":\"M19R_GATE5_OMEGA_NUMERIC_0\",\"status\":\"FAIL\""
    body+=",\"error\":$(m19r_jstr "$reason"),\"run_id\":$(m19r_jstr "$NUM_RUN_ID")"
    body+=",\"timestamp_utc\":$(m19r_jstr "${NUM_TS:-$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ)}")"
    body+=",\"supplied_omega_candidate\":$(m19r_jstr "$NUM_OMEGA_CAND")"
    body+=",\"supplied_physics_candidate\":$(m19r_jstr "$NUM_PHYSICS_CAND")"
    v=null; m19r_git "$omega" rev-parse HEAD && v=$(m19r_jstr "$M19R_GIT")
    body+=",\"omega_head\":$v"
    v=null; [ -d "${M19R_PHYSICS:-/nonexistent}" ] && m19r_git "$M19R_PHYSICS" rev-parse HEAD && v=$(m19r_jstr "$M19R_GIT")
    body+=",\"physics_head\":$v"
    body+=",\"candidate_trees_clean\":{\"omega\":$(num_tree_clean "$omega"),\"physics\":$([ -d "${M19R_PHYSICS:-/nonexistent}" ] && num_tree_clean "$M19R_PHYSICS" || echo null)}"
    v=null; [ -z "${NUM_RUN_DIR:-}" ] || [ ! -f "$NUM_RUN_DIR/test_omega_numeric" ] ||
        v="\"$(m19r_sha_file "$NUM_RUN_DIR/test_omega_numeric")\""
    body+=",\"candidate_binary_sha256\":$v"
    v=null; [[ ${NUM_EXEC_RC:-} =~ ^[0-9]+$ ]] && v=$NUM_EXEC_RC
    body+=",\"gate_binary_exit_status\":$v"
    v=null; [ -z "${NUM_RUN_DIR:-}" ] || [ ! -f "$NUM_RUN_DIR/gate5.log" ] ||
        v="\"$(m19r_sha_file "$NUM_RUN_DIR/gate5.log")\""
    body+=",\"gate_log_sha256\":$v"
    v=null; [ -z "${NUM_RUN_DIR:-}" ] || [ ! -f "$NUM_RUN_DIR/gate5.stderr" ] ||
        v="\"$(m19r_sha_file "$NUM_RUN_DIR/gate5.stderr")\""
    body+=",\"gate_stderr_sha256\":$v}"
    num_write_receipt "$omega" "$body" 0
}

num_qualify() {
    local omega=$M19R_OMEGA bin before after rc
    m19r_must_candidate "$omega" "$NUM_OMEGA_CAND" || return 1
    num_check_physics "$omega" "$M19R_PHYSICS" "$NUM_PHYSICS_CAND" || return 1
    before=$(m19r_historical) || return 1
    bin=$NUM_RUN_DIR/test_omega_numeric

    exec 9> /tmp/aien-gb10.lock || m19r_fail "cannot open /tmp/aien-gb10.lock" || return 1
    flock -x 9 || { exec 9>&-; m19r_fail "cannot take the GPU lock /tmp/aien-gb10.lock; no build and no device access"; return 1; }
    rc=0
    num_build "$omega" "$M19R_PHYSICS" "$bin" "$NUM_RUN_DIR/build.log" || rc=1
    if [ "$rc" = 0 ]; then
        NUM_BINARY_SHA=$(m19r_sha_file "$bin")
        if m19r_git "$omega" rev-parse HEAD; then NUM_RUN_COMMIT=$M19R_GIT; else rc=1; m19r_fail "cannot read the omega HEAD commit"; fi
        NUM_TS=$(date -u +%Y-%m-%dT%H:%M:%S.%6NZ)
        if [ "$rc" = 0 ]; then
            num_execute "$omega" "$bin" || rc=1
            echo "gate binary exit status $NUM_EXEC_RC" >> "$NUM_RUN_DIR/build.log"
        fi
    fi
    exec 9>&-
    [ "$rc" = 0 ] || return 1

    after=$(m19r_historical) || return 1
    [ "$before" = "$after" ] || m19r_fail "evidence/ changed during the run" || return 1
    num_receipt "$omega" "$NUM_RUN_DIR/gate5.log"
}

# num_on_failure OMEGA REASON -- a failed run: forget any digest, remove the
# PASS preview so nothing left behind claims a pass, and with NUM_RECORD=1
# write the FAIL receipt.
num_on_failure() {
    NUM_DIGEST= NUM_PERMANENT=
    rm -f "$NUM_RUN_DIR/receipt-preview.json"
    if [ "${NUM_RECORD:-0}" = 1 ]; then
        if num_fail_receipt "$1" "$2"; then
            echo "Permanent FAIL receipt: $NUM_PERMANENT" >&2
        else
            echo "Could not write the FAIL receipt: $M19R_ERR" >&2
        fi
    else
        echo "No receipt written (no --record)." >&2
    fi
}

num_usage() {
    echo "usage: $M19R_PROG --omega-candidate SHA --physics-candidate SHA [--physics-dir DIR] [--evidence-dir DIR] [--record]"
}

num_main() {
    local physics_dir= rc=0 run_json
    NUM_OMEGA_CAND= NUM_PHYSICS_CAND= NUM_RECORD=0 NUM_PERMANENT= NUM_DIGEST= NUM_EVIDENCE_DIR=
    NUM_EXEC_RC= NUM_LOG_SHA= NUM_BINARY_SHA= NUM_RUN_COMMIT= NUM_TS=
    while [ $# -gt 0 ]; do
        case $1 in
            -h|--help) num_usage; exit 0;;
            --omega-candidate|--physics-candidate|--physics-dir|--evidence-dir)
                [ $# -ge 2 ] || { num_usage >&2; echo "$M19R_PROG: $1 needs a value" >&2; exit 2; }
                case $1 in
                    --omega-candidate) NUM_OMEGA_CAND=$2;;
                    --physics-candidate) NUM_PHYSICS_CAND=$2;;
                    --physics-dir) physics_dir=$2;;
                    --evidence-dir) NUM_EVIDENCE_DIR=$2;;
                esac
                shift;;
            --omega-candidate=*) NUM_OMEGA_CAND=${1#*=};;
            --physics-candidate=*) NUM_PHYSICS_CAND=${1#*=};;
            --physics-dir=*) physics_dir=${1#*=};;
            --evidence-dir=*) NUM_EVIDENCE_DIR=${1#*=};;
            --record) NUM_RECORD=1;;
            *) num_usage >&2; echo "$M19R_PROG: unrecognized argument: $1" >&2; exit 2;;
        esac
        shift
    done
    if [ -z "$NUM_OMEGA_CAND" ] || [ -z "$NUM_PHYSICS_CAND" ]; then
        num_usage >&2; echo "$M19R_PROG: --omega-candidate and --physics-candidate are required" >&2; exit 2
    fi
    M19R_PHYSICS=$(realpath -m "${physics_dir:-$(dirname "$M19R_OMEGA")/physics}")
    if ! num_check_evidence_dir "$M19R_OMEGA"; then
        num_usage >&2; echo "$M19R_PROG: $M19R_ERR" >&2; exit 2
    fi
    m19r_build_canon || { echo "Gate 5 failed: cannot build tools/json_canon.c" >&2; exit 1; }
    trap 'rm -rf "$M19R_TMP"' EXIT
    NUM_RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)-$(od -An -N6 -tx1 /dev/urandom | tr -d ' \n')
    NUM_RUN_DIR=$M19R_OMEGA/build/qual-runs/gate5-$NUM_RUN_ID
    mkdir -p "$(dirname "$NUM_RUN_DIR")" && mkdir "$NUM_RUN_DIR" || exit 1
    rm -f "$M19R_TMP/error"
    if num_qualify; then
        echo "Gate 5 PASS: receipt digest $NUM_DIGEST"
        [ -z "$NUM_PERMANENT" ] || echo "Permanent PASS receipt: $NUM_PERMANENT"
    else
        rc=1
        [ ! -s "$M19R_TMP/error" ] || M19R_ERR=$(cat "$M19R_TMP/error")
        echo "Gate 5 FAILED: $M19R_ERR" >&2
        num_on_failure "$M19R_OMEGA" "$M19R_ERR"
    fi
    run_json="{\"run_id\":$(m19r_jstr "$NUM_RUN_ID"),\"status\":\"$([ "$rc" = 0 ] && echo PASS || echo FAILED)\""
    run_json+=",\"physics_dir\":$(m19r_jstr "$M19R_PHYSICS")"
    run_json+=",\"evidence_dir\":$(m19r_jstr "$NUM_EVIDENCE_DIR")"
    run_json+=",\"trees_clean_after\":{\"omega\":$(num_tree_clean "$M19R_OMEGA"),\"physics\":$([ -d "$M19R_PHYSICS" ] && num_tree_clean "$M19R_PHYSICS" || echo null)}"
    [ "$rc" = 0 ] || run_json+=",\"error\":$(m19r_jstr "$M19R_ERR")"
    [ -z "$NUM_DIGEST" ] || run_json+=",\"receipt_digest\":\"$NUM_DIGEST\""
    [ -z "$NUM_PERMANENT" ] || run_json+=",\"permanent_receipt\":$(m19r_jstr "$NUM_PERMANENT")"
    run_json+="}"
    printf '%s' "$run_json" | "$JSON_CANON" --pretty > "$NUM_RUN_DIR/run.json" || rc=1
    echo "Gate 5 run evidence: $NUM_RUN_DIR/run.json"
    exit "$rc"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    set -u
    num_main "$@"
fi
