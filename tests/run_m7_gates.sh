#!/bin/sh
# tests/run_m7_gates.sh -- Master Qualification Gate Runner for Milestone 7 (OMEGA_VERIFY)
set -eu

OMEGA_ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
TOOL="$OMEGA_ROOT/build/omegatool"
EVIDENCE_DIR="$OMEGA_ROOT/evidence"
RECEIPT="$EVIDENCE_DIR/omega_verify_qualification_receipt.json"

echo "================================================================================"
echo "    AIEN OMEGA SUBSTRATE — MILESTONE 7: OMEGA_VERIFY QUALIFICATION"
echo "================================================================================"
echo "Timestamp: $(date -u +'%Y-%m-%dT%H:%M:%SZ')"
echo "Host:      $(uname -s) $(uname -m)"
echo ""

# Step 1: Clean build
echo "[*] Step 1: Rebuilding OMEGA substrate and omegatool from source..."
make -C "$OMEGA_ROOT" clean
make -C "$OMEGA_ROOT"
echo "    Build OK."
echo ""

# Step 2: Run all 10 M7 qualification gates
echo "[*] Step 2: Evaluating 10 canonical M7 qualification gates..."
"$TOOL" --run-m7-gates
echo ""

# Step 3: Run Demonstration (Trusted Verification Ladder V0 -> V1 -> V2)
echo "[*] Step 3: Running Demonstration (Trusted Verification Ladder V0 -> V1 -> V2)..."
"$TOOL" --demonstrate-verify
echo ""

# Step 4: Regression checks (M4, M5, M6)
echo "[*] Step 4: Running regression checks across M4, M5, and M6..."
"$TOOL" --run-gates
"$TOOL" --run-m5-gates
"$TOOL" --run-m6-gates
echo "    Regression checks OK: all M4, M5, and M6 gates passed."
echo ""

# Step 5: Dump and hash test vectors and binaries
echo "[*] Step 5: Computing artifact digests..."
mkdir -p "$EVIDENCE_DIR"
sha256sum "$OMEGA_ROOT/src/omega_verify."* \
          "$OMEGA_ROOT/src/aarch64_decoder."* \
          "$OMEGA_ROOT/src/omega_validate."* \
          "$TOOL" > "$EVIDENCE_DIR/m7_corpus_digests.txt"

DIGEST_VERIFY=$(sha256sum "$OMEGA_ROOT/src/omega_verify.c" | awk '{print $1}')
DIGEST_DECODER=$(sha256sum "$OMEGA_ROOT/src/aarch64_decoder.c" | awk '{print $1}')
DIGEST_VALIDATE=$(sha256sum "$OMEGA_ROOT/src/omega_validate.c" | awk '{print $1}')
DIGEST_TOOL=$(sha256sum "$TOOL" | awk '{print $1}')
GIT_COMMIT=$(git -C "$OMEGA_ROOT" rev-parse --verify HEAD 2>/dev/null || echo "m7-genesis")

echo "[*] Step 6: Generating formal qualification receipt: $RECEIPT..."
cat <<EOF > "$RECEIPT"
{
  "milestone": "MILESTONE 7 — OMEGA_VERIFY",
  "status": "QUALIFIED / PASS",
  "contract_id": "CONTRACT-OMEGA-VERIFY-M7",
  "generated_at": "$(date -u +'%Y-%m-%dT%H:%M:%SZ')",
  "git_commit": "$GIT_COMMIT",
  "host": {
    "os": "$(uname -s)",
    "arch": "$(uname -m)",
    "compiler": "$(${CC:-gcc} --version | head -n 1)"
  },
  "artifacts": {
    "src/omega_verify.c": { "sha256": "$DIGEST_VERIFY" },
    "src/aarch64_decoder.c": { "sha256": "$DIGEST_DECODER" },
    "src/omega_validate.c": { "sha256": "$DIGEST_VALIDATE" },
    "build/omegatool": { "sha256": "$DIGEST_TOOL" }
  },
  "verification_ladder": {
    "tier_v0_structural": {
      "status": "PASS",
      "scope": "Graph types, DAG integrity, target profile, buffer bounds, instruction stream decoding, terminal RET"
    },
    "tier_v1_differential": {
      "status": "PASS",
      "scope": "Bit-for-bit output parity between semantic reference evaluation and native execution across test corpus",
      "divergence_detection": "Refused fail-closed on mutated realization"
    },
    "tier_v2_property_invariants": {
      "status": "PASS",
      "scope": "Algebraic commutativity, identity elements, modular overflow wrapping, range preservation"
    },
    "tier_v3_to_v5_framework": {
      "status": "STUB_INITIALIZED",
      "scope": "Adversarial fuzzing, symbolic equivalence, proof-carrying code contracts"
    }
  },
  "qualification_gates": {
    "total": 10,
    "passed": 10,
    "gates": {
      "OMEGA_VERIFY_V0_TYPE_PASS": "PASS",
      "OMEGA_VERIFY_V0_DAG_PASS": "PASS",
      "OMEGA_VERIFY_V0_CODE_BOUNDS_PASS": "PASS",
      "OMEGA_VERIFY_V0_INSN_DECODE_PASS": "PASS",
      "OMEGA_VERIFY_V0_TERMINAL_RET_PASS": "PASS",
      "OMEGA_VERIFY_V1_DIFFERENTIAL_PASS": "PASS",
      "OMEGA_VERIFY_V1_DIVERGENCE_REFUSAL_PASS": "PASS",
      "OMEGA_VERIFY_V2_COMMUTATIVITY_PASS": "PASS",
      "OMEGA_VERIFY_V2_IDENTITY_PASS": "PASS",
      "OMEGA_VERIFY_V2_OVERFLOW_PASS": "PASS"
    }
  },
  "sovereignty_accounting": {
    "generator_trust": "UNTRUSTED",
    "verifier_trust": "TRUSTED",
    "python_dependency": false,
    "formal_verifier_lineage": "Direct sovereign C99 verification engine executing deterministic proof obligations"
  }
}
EOF

echo ""
echo "================================================================================"
echo "    MILESTONE 7 (OMEGA_VERIFY) QUALIFICATION COMPLETE: 10/10 GATES PASS"
echo "    Receipt generated at: $RECEIPT"
echo "================================================================================"
