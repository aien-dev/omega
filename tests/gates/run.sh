#!/usr/bin/env bash
# tests/gates/run.sh: the one entry point for the local verification gates of omega.
# Every step runs the SAME command as the hosted job it mirrors (.github/workflows/pr-smoke.yml,
# host-suites.yml, host-suites-2.yml, host-suites-3.yml, rx-host.yml, turing-yield.yml,
# evidence-immutable.yml, crumb.yml). Manifests under tests/gates/ name this script plus one
# step word, so no manifest needs shell expansion (ADR 0028 P6).
#
# Pinned sibling sources. The hosted jobs check out aien-spark/physics at physics.lock (rx-host
# at its own hard-coded commit), aien-spark/aienos at aienos.lock and aien-spark/aienos at
# argus.lock, as physics-src/, aienos-src/ and aienos-argus/ next to the Makefile, and exclude
# them from git status. Here the same commits are materialised under build/pins/ (build/ is
# gitignored) from the sibling local clones ../physics and ../aienos, offline.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
step="${1:?usage: tests/gates/run.sh <step>}"

# Every step builds in its own directory. The Spark judge (aien-test) runs a repository's host gates
# at the same moment in one worktree; with one shared build/ a gate links an object another gate's
# compiler is still writing (omega#511: OMEGA-M12 "undefined reference to omega_tern_*" with the
# object on the link line). OUT_DIR is read by the Makefile (?=) and by every make it spawns.
# The pins stay at build/pins, shared read-only under their own lock. The directory name keeps
# only [A-Za-z0-9_.-] from the step word: "spark:test-r13-host" would put a colon in a make
# target ("multiple target patterns").
export OUT_DIR="build/step-$(printf '%s' "$step" | tr -c 'A-Za-z0-9_.-' '-')"
mkdir -p "$OUT_DIR"
obs() { printf 'AIEN-OBS %s %s\n' "$1" "$2"; }

PINS="$ROOT/build/pins"
NOPHYS="PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0"
lock() { awk 'NF && $1 !~ /^#/ {print $1; exit}' "$1"; }

# pin <dir-name> <sibling repo> <sha40>: build/pins/<dir-name> is a detached checkout of <sha40>
# taken from the object store of ../<sibling repo>. Reused when already at that commit.
pin() {
  local name="$1" repo="$2" sha="$3" dir="$PINS/$1"
  if [ ! -d "../$repo/.git" ] && [ ! -f "../$repo/.git" ]; then
    echo "run.sh: ../$repo is missing; the hosted job checks out aien-dev/$repo at $sha" >&2
    exit 3
  fi
  mkdir -p "$PINS"
  (
    flock -x 9
    if [ ! -d "$dir" ] || [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" != "$sha" ]; then
      rm -rf "$dir"
      git clone --quiet --shared --no-checkout "../$repo" "$dir"
      git -C "$dir" checkout --quiet --detach "$sha"
    fi
  ) 9>"$PINS/.$name.lock"
  obs "pin_${name//-/_}" "\"$sha\""
}
physics_pin() { pin physics-src physics "$(lock physics.lock)"; export PHYSICS_DIR="$PINS/physics-src"; }
aienos_pin()  { pin aienos-src aienos "$(lock aienos.lock)"; export AIENOS_R7_DIR="$PINS/aienos-src"; export AIENOS_COMMIT="$(lock aienos.lock)"; }
argus_pin()   { pin aienos-argus aienos "$(lock argus.lock)"; export ARGUS_REPO="$PINS/aienos-argus"; }
export OMEGA_CANDIDATE_COMMIT="$(git rev-parse HEAD)"

# receipt <file name or glob> <grep pattern>...: the hosted "Show ... receipt" steps.
# Only receipts written during this step count: a hosted job starts from a fresh checkout, so it
# can only see its own run, while a reused local tree keeps build/qual-runs from earlier runs.
STEP_MARK="$(mktemp /tmp/omega-gate-step.XXXXXX)"
trap 'rm -f "$STEP_MARK"' EXIT
receipt() {
  local r; r="$(find build/qual-runs -name "$1" -type f -newer "$STEP_MARK" | head -n 1)"; shift
  test -n "$r"
  local p; for p in "$@"; do grep -q "$p" "$r"; done
  sha256sum "$r"
}
gates_pass() { grep -E 'TOTAL GATES: [0-9]+ \| PASSED: [0-9]+ \| FAILED: 0' "$1" && ! grep -E 'FAILED: [1-9]' "$1"; }

case "$step" in
  crumb-verify)
    # Hosted crumb.yml builds the crumb tool from the aien-architecture commit in crumb.lock.
    # Same here, from the sibling checkout ../aien-architecture into a temp dir outside the
    # tree, so a newer crumb on PATH can never change the verdict.
    if [ ! -d ../aien-architecture ]; then
      echo "run.sh: ../aien-architecture is missing; the hosted job checks out aien-dev/aien-architecture at crumb.lock" >&2
      exit 3
    fi
    tool="$(mktemp -d /tmp/omega-crumb-tool.XXXXXX)"
    trap 'rm -rf "$tool" "$STEP_MARK"' EXIT
    git -C ../aien-architecture archive "$(lock crumb.lock)" tools/crumb | tar -x -C "$tool"
    make -s -C "$tool/tools/crumb" crumb
    obs crumb_tool_commit "\"$(lock crumb.lock)\""
    "$tool/tools/crumb/crumb" verify .
    ;;
  # pr-smoke.yml
  base)
    physics_pin; set -o pipefail
    make test | tee build/base.log
    grep -E 'TOTAL GATES: ([0-9]+) \| PASSED: \1 \| FAILED: 0' build/base.log
    ! grep -qE 'FAILED: [1-9]|\[FAIL\]|\(FAIL\)' build/base.log
    ;;
  language)           # pr-smoke.yml and host-suites-3.yml (same command)
    set -o pipefail; mkdir -p build
    make $NOPHYS test-language | tee build/lang.log
    grep -E '^PASS ([0-9]+)/\1$' build/lang.log
    ! grep -q '^FAIL' build/lang.log
    ;;
  numeric-transc)     # pr-smoke.yml and host-suites-3.yml (same command and checks)
    set -o pipefail; mkdir -p build
    make $NOPHYS test-numeric-transc | tee build/transc.log
    test "$(grep -cE '^mutant [0-9]+ killed' build/transc.log)" -eq 10
    grep -qE '^test-numeric-transc: PASS \(all 10 mutants killed\)' build/transc.log
    ! grep -q '^FAIL' build/transc.log
    obs mutants_killed 10
    ;;
  # host-suites.yml
  searchtrace)        make $NOPHYS test-searchtrace ;;
  estimation)         for t in test-estimation test-est-tools test-estimation-v3 test-est3c-tools; do make $NOPHYS $t; done ;;
  ans)                make $NOPHYS test-inertial-alignment ;;
  numeric-cpu)        for t in test-numeric-cpu test-numeric-qualify test-e1-combine test-chipwait-campaign test-gpu-wait test-gpu-wait-mutants test-gpu-code-alloc; do make $NOPHYS $t; done ;;
  train)              make $NOPHYS test-train ;;
  compiler)           make $NOPHYS test-compiler-quick; make $NOPHYS test-compiler-full ;;
  one-layer-acceptance) make $NOPHYS test-one-layer-acceptance ;;
  physics0)           make $NOPHYS physics0-test ;;
  chip-tool-selftests) for t in test-m19r-qualify test-gate14-combine test-window-lane-guard test-unwritten-trap-host test-ldst-host; do make $NOPHYS $t; done ;;
  # host-suites-2.yml
  visor)              aienos_pin; make $NOPHYS test-visor ;;
  turing)             make $NOPHYS test-turing; make $NOPHYS test-turing-exp001-a test-turing-exp001-coders; make $NOPHYS test-turing-verify-indep; make $NOPHYS test-turing-qcont test-turing-qrecord ;;
  brownian)           make $NOPHYS test-brownian-tps; make $NOPHYS test-brownian-active ;;
  polyglot)           make $NOPHYS test-polyglot-asm; make $NOPHYS test-polyglot-encoder ;;
  algebra)            make $NOPHYS test-algebra test-algebra-asan; make $NOPHYS test-realize; make $NOPHYS test-phase-twin; make $NOPHYS test-ma-digital-bottom ;;
  path)               make $NOPHYS test-path ;;
  crumbline)          make $NOPHYS test-crumbline test-library test-vcstore test-resolve test-vcstore-genesis test-resolve-genesis test-genesis test-genesis-real test-program-ir test-vc-bridge ;;
  runtime)
    physics_pin; aienos_pin
    # host-suites-2.yml sets only PHYSICS_DIR and AIENOS_R7_DIR, and only on the steps that list
    # them; the qualify self-test and the aienos.lock source proof run with neither (the latter
    # checks the default cache, which an inherited AIENOS_R7_DIR override would bypass).
    unset AIENOS_COMMIT OMEGA_CANDIDATE_COMMIT
    make test-r15-instr
    make test-r15-receipt
    make test-r15-preflight test-r15-g15-diag
    make test-r16-inventory
    make test-r16-negative
    make test-r16-surface
    make test-r16-negative-mutants
    env -u PHYSICS_DIR -u AIENOS_R7_DIR make test-r16-qualify-selftest
    env -u PHYSICS_DIR -u AIENOS_R7_DIR make test-aienos-lock-source
    make test-allen
    ;;
  argus)
    physics_pin; aienos_pin; argus_pin
    ARGUS_STREAMS="$(mktemp -d /tmp/argus-streams.XXXXXX)"; export ARGUS_STREAMS
    make test-argus-runtime test-machine-identity-argus
    ;;
  numeric2)           make $NOPHYS test-numeric-reduce-cpu test-divsqrt-host ;;
  tensor)             make $NOPHYS test-tensor; make $NOPHYS test-tensor-store; make $NOPHYS test-m20-receipt; make $NOPHYS test-tensor-mutations ;;
  # host-suites-3.yml
  numeric-lifecycle)  make $NOPHYS test-numeric-lifecycle ;;
  numeric-transc-digest)
    set -o pipefail; mkdir -p build
    make $NOPHYS test-numeric-transc-digest | tee build/digest.log
    n=$(grep -c 'matches frozen digest' build/digest.log); obs frozen_digests_matched "$n"
    test "$n" -ge 1
    ;;
  numeric-transc-full) make $NOPHYS test-numeric-transc-full ;;
  m6|m7|m8|m9|m10|m11|m12|m13|m14|m15|m17|m5|m19|m19r-qualify|gate14-combine)
    physics_pin; set -o pipefail; mkdir -p build
    make "test-$step" | tee "build/gate-$step.log"
    gates_pass "build/gate-$step.log"
    ;;
  # rx-host.yml
  rx-host-reference)
    pin physics-src-6d7cf0d physics 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
    export PHYSICS_COMMIT=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf PHYSICS_DIR="$PINS/physics-src-6d7cf0d"
    aienos_pin; set -o pipefail; mkdir -p build
    make test-r3 test-r7 test-r16-operator-authority-misuse test-r8 test-r9 test-r12 test-rx-emergency test-action-graph test-state-projection test-capability-query test-capability-graph test-skillroute-compose test-semantic-comm test-cognitive-routing test-sem-incremental test-cortex test-jspace-prod test-composition test-composition-attach-asan test-composition-gate test-fabric test-fabric-living
    make test-machine-identity
    make test-effect-cap64 | tee build/effect-cap64.log
    grep -q '^OMEGA_EFFECT_CAP64_PASS$' build/effect-cap64.log
    receipt omega_state_projection_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_STATE_PROJECTION_PASS": "PASS"'
    receipt rx_action_graph_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_ACTION_GRAPH_IR_PASS": "PASS"'
    # omega#404 ruling: PASS (energy meter present, e.g. aien_spbm hwmon on the Spark) and HOST_PASS_ENERGY_UNMEASURED
    # (no meter, the hosted runner) both pass; which one is recorded as an observation. PASS_UNBOUND and FAIL do not.
    receipt omega_incremental_semantics_receipt.json '"OMEGA_INCREMENTAL_SEMANTICS_PASS": "\(PASS\|HOST_PASS_ENERGY_UNMEASURED\)"' '"false_hits": 0,'
    obs incremental_semantics_verdict "$(grep -o '"OMEGA_INCREMENTAL_SEMANTICS_PASS": "[A-Z_]*"' "$(find build/qual-runs -name omega_incremental_semantics_receipt.json -type f -newer "$STEP_MARK" | head -n 1)" | sed 's/.*: //')"
    receipt rx_capability_query_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_CAPABILITY_QUERY_PASS": "PASS"'
    receipt rx_semantic_comm_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_SEMANTIC_COMMUNICATION_PASS": "PASS"'
    receipt rx_cognitive_routing_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_COGNITIVE_ROUTING_PASS": "PASS"'
    make test-workflow-fusion
    receipt rx_workflow_fusion_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_WORKFLOW_FUSION_PASS": "PASS"'
    make test-typed-results
    receipt rx_typed_results_receipt.json '"failures": 0,' '"candidate_bound": true,' '"OMEGA_TYPED_RESULT_CONSTRAINTS_PASS": "PASS"'
    make test-costmodel
    receipt rx_aegis_resident_receipt.json '"failures": 0,' '"candidate_bound": true,' '"R8_CONSTITUTIONAL_AEGIS": "PASS"'
    r="$(find build/qual-runs -name rx_heartbeat_receipt.json -type f -newer "$STEP_MARK" | head -n 1)"
    native="$(find build/qual-runs -name rx_native_authority_receipt.json -type f -newer "$STEP_MARK" | head -n 1)"
    generation="$(find build/qual-runs -name rx_generation_barrier_receipt.json -type f -newer "$STEP_MARK" | head -n 1)"
    test -n "$r"; test -n "$native"; test -n "$generation"
    if git diff --quiet && git diff --cached --quiet; then obs tree_cleanliness '"clean"'; else obs tree_cleanliness '"dirty"'; fi
    tools/rx_host_receipts.sh heartbeat "$r"
    tools/rx_host_receipts.sh native "$native" "$OMEGA_CANDIDATE_COMMIT"
    tools/rx_host_receipts.sh generation "$generation" "$OMEGA_CANDIDATE_COMMIT"
    ;;
  faculty-arm)
    pin physics-src-6d7cf0d physics 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
    export PHYSICS_DIR="$PINS/physics-src-6d7cf0d"
    aienos_pin; argus_pin; set -o pipefail; mkdir -p build
    make OUT_DIR=build/pid $NOPHYS test-program-id
    make OUT_DIR=build/prz $NOPHYS test-program-realize
    R10_ALLOW_NO_WIN=1 make test-r10
    receipt rx_omega_faculty_receipt.json '"failures": 0,' '"candidate_bound": true,' '"tree_dirty": false,'
    make test-regulate
    make test-r11
    receipt rx_aien_faculty_receipt.json '"failures": 0,' '"candidate_bound": true,' '"tree_dirty": false,'
    make "$OUT_DIR/rx_r13_living_host" "$OUT_DIR/rx_r13_living_testbuild_host"
    make "$OUT_DIR/rx_r14_recovery_host"
    make test-r16-authpath-linkmap | tee build/r16-g3-linkmap-host.log
    grep -qx 'R16-G3 static: R16_G3_LINKMAP=STATIC_PASS mode=host (checks 1-3 of 5 on this build; not the gate)' build/r16-g3-linkmap-host.log
    grep -qx 'R16 gate: R16_G3_AUTHPATH=NOT_RUN (checks 4-5, exec trace and living run, not run in static-only mode)' build/r16-g3-linkmap-host.log
    make test-r16-authpath-linkmap-silicon | tee build/r16-g3-linkmap-silicon.log
    grep -qx 'R16-G3 static: R16_G3_LINKMAP=STATIC_PASS mode=silicon (checks 1-3 of 5 on this build; not the gate)' build/r16-g3-linkmap-silicon.log
    grep -qx 'R16 gate: R16_G3_AUTHPATH=NOT_RUN (checks 4-5, exec trace and living run, not run in static-only mode)' build/r16-g3-linkmap-silicon.log
    make test-prod-hygiene | tee build/prod-hygiene-host.log
    grep -q '^R16 prod: PROD_HYGIENE=PASS mode=host ' build/prod-hygiene-host.log
    R16_PROD_NO_RUN=1 make test-prod-hygiene-silicon | tee build/prod-hygiene-silicon.log
    grep -q '^R16 prod: PROD_HYGIENE=PASS mode=silicon ' build/prod-hygiene-silicon.log
    make test-prod-refuses-test-pieces | tee build/prod-refuses.log
    grep -q '^R16 prod: PROD_REFUSES_TEST_PIECES=PASS ' build/prod-refuses.log
    EMPIRICAL_ALLOW_NO_WIN=1 make test-empirical
    receipt rx_empirical_optimizer_receipt.json '"failures": 0,' '"candidate_bound": true,' '"tree_dirty": false,' '"mismatches": 0,' '"OMEGA_EMPIRICAL_OPTIMIZER_PASS": "\(PASS\|CHAIN_PASS_NO_WIN\)"'
    PLAN_REUSE_NO_ENERGY=1 make test-plan-reuse
    receipt 'rx_plan_reuse_receipt.*.json' '"failures": 0,' '"G3_outcome_and_evidence": "PASS"' '"G4_false_applicability_zero": "PASS"' '"G5_identity": "PASS"'
    make test-rx-deadline-cancel
    make test-i11-wake-merge
    make test-c4-requal
    ;;
  # rx-host.yml chip-only-declared and spark-only targets (SKIP on GitHub, never a pass there)
  chip:*|spark:*)
    t="${step#*:}"
    pin physics-src-6d7cf0d physics 6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf
    export PHYSICS_DIR="$PINS/physics-src-6d7cf0d"
    aienos_pin; argus_pin
    make "$t"
    ;;
  # turing-yield.yml
  turing-yield)       make test-turing-yield test-turing-energy ;;
  # evidence-immutable.yml: reviewed evidence is additions-only. Hosted compares the PR base
  # and head; locally the base is origin/main (or AIEN_BASE_REF).
  evidence-immutable)
    base="${AIEN_BASE_REF:-origin/main}"
    obs base_ref "\"$(git rev-parse "$base")\""
    changed="$(git diff --name-status --diff-filter=MDRT "$base...HEAD" -- evidence/)"
    if [ -n "$changed" ]; then
      echo "These existing evidence files would be changed:"; echo "$changed"
      echo "Add a new, content-addressed file instead of rewriting reviewed evidence."
      obs evidence_rewrites "$(printf '%s\n' "$changed" | grep -c .)"
      exit 1
    fi
    obs evidence_rewrites 0
    echo "evidence/: additions only"
    ;;
  zero-disk-secrets)
    echo "Auditing repository for prohibited plaintext secret files..."
    if find . -maxdepth 4 -name ".env*" -not -path "*/.git/*" -not -path "*/build/*" | grep -q .; then
      echo "::error::Prohibited plaintext .env file found. Use an approved AIEN SecretProvider."
      obs env_files_found true; exit 1
    fi
    obs env_files_found false
    echo "Zero plaintext .env files verified."
    ;;
  *) echo "run.sh: unknown step '$step'" >&2; exit 2 ;;
esac
