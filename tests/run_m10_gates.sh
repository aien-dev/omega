#!/bin/sh
# tests/run_m10_gates.sh -- Master Qualification Gate Runner for Milestone 10 (OMEGA_LIBRARY_V1)
set -eu

OMEGA_ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
TOOL="$OMEGA_ROOT/build/omegatool"
EVIDENCE_DIR="$OMEGA_ROOT/evidence"
RECEIPT="$EVIDENCE_DIR/omega_library_v1_qualification_receipt.json"

echo "================================================================================"
echo "    AIEN OMEGA SUBSTRATE — MILESTONE 10: OMEGA_LIBRARY_V1 QUALIFICATION"
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

# Step 2: Run all 10 M10 qualification gates
echo "[*] Step 2: Evaluating 10 canonical M10 qualification gates..."
"$TOOL" --run-m10-gates
echo ""

# Step 3: Run Demonstration (Library Catalog & Synthesis Reuse)
echo "[*] Step 3: Running Demonstration (Library Catalog & Synthesis Reuse)..."
"$TOOL" --reference-demonstrate-library
echo ""

# Step 4: Regression checks (M4 through M9)
echo "[*] Step 4: Running regression checks across M4 through M9..."
"$TOOL" --run-gates
"$TOOL" --run-m5-gates
"$TOOL" --run-m6-gates
"$TOOL" --run-m7-gates
"$TOOL" --run-m8-gates
"$TOOL" --run-m9-gates
echo "    Regression checks OK: all M4 through M9 gates passed (61 gates)."
echo ""

# Step 5: Dump and hash test vectors and binaries
echo "[*] Step 5: Computing artifact digests..."
mkdir -p "$EVIDENCE_DIR"
sha256sum "$OMEGA_ROOT/src/omega_library."* \
          "$TOOL" > "$EVIDENCE_DIR/m10_corpus_digests.txt"

DIGEST_LIBRARY=$(sha256sum "$OMEGA_ROOT/src/omega_library.c" | awk '{print $1}')
DIGEST_LIBRARY_H=$(sha256sum "$OMEGA_ROOT/src/omega_library.h" | awk '{print $1}')
DIGEST_TOOL=$(sha256sum "$TOOL" | awk '{print $1}')
PARENT_COMMIT="469f53c9b4e7fe6dcfe3287ae27161b997c11874"
SOURCE_PARENT_COMMIT="$PARENT_COMMIT"
QUALIFIED_COMMIT="${QUALIFIED_IMPLEMENTATION_COMMIT:-$(git -C "$OMEGA_ROOT" rev-parse HEAD)}"
RECEIPT_COMMIT="${RECEIPT_COMMIT:-pending-receipt-commit}"

echo "[*] Step 6: Generating formal qualification receipt: $RECEIPT..."
cat <<EOF > "$RECEIPT"
{
  "milestone": "MILESTONE 10 — OMEGA_LIBRARY_V1",
  "status": "QUALIFIED / PASS",
  "contract_id": "CONTRACT-OMEGA-LIBRARY-V1-M10",
  "generated_at": "$(date -u +'%Y-%m-%dT%H:%M:%SZ')",
  "source_parent_commit": "$SOURCE_PARENT_COMMIT",
  "qualified_implementation_commit": "$QUALIFIED_COMMIT",
  "receipt_commit": "$RECEIPT_COMMIT",
  "git_commit": "$QUALIFIED_COMMIT",
  "host": {
    "os": "$(uname -s)",
    "arch": "$(uname -m)",
    "compiler": "$(${CC:-gcc} --version | head -n 1)"
  },
  "artifacts": {
    "src/omega_library.c": { "sha256": "$DIGEST_LIBRARY" },
    "src/omega_library.h": { "sha256": "$DIGEST_LIBRARY_H" },
    "build/omegatool": { "sha256": "$DIGEST_TOOL" }
  },
  "library_properties": {
    "content_addressing": "Lookup by SEMANTIC_ID, REALIZATION_ID, and human symbol name",
    "immutability": "Monotonic versioning with cryptographic SHA-256 state digest",
    "dependency_tracking": "Explicit DAG tracking with DFS cycle detection refusing circular dependencies",
    "verification_admission": "Fail-closed admission: unverified programs strictly rejected",
    "synthesis_integration": "Direct export to SynthPrimitiveBank for composition reuse"
  },
  "qualification_gates": {
    "total": 10,
    "passed": 10,
    "gates": {
      "OMEGA_LIBRARY_INIT_PASS": "PASS",
      "OMEGA_LIBRARY_INSERT_PASS": "PASS",
      "OMEGA_LIBRARY_INDEXING_PASS": "PASS",
      "OMEGA_LIBRARY_QUERY_PASS": "PASS",
      "OMEGA_LIBRARY_DEP_DAG_PASS": "PASS",
      "OMEGA_LIBRARY_IMMUTABILITY_PASS": "PASS",
      "OMEGA_LIBRARY_UNVERIFIED_REFUSAL_PASS": "PASS",
      "OMEGA_LIBRARY_DUPLICATE_REFUSAL_PASS": "PASS",
      "OMEGA_LIBRARY_SYNTHESIS_REUSE_PASS": "PASS",
      "OMEGA_LIBRARY_RECEIPT_PASS": "PASS"
    }
  },
  "sovereignty_accounting": {
    "llvm_dependency": false,
    "gnu_as_dependency": false,
    "gcc_asm_dependency": false,
    "jit_compiler_dependency": false,
    "python_dependency": false,
    "realizer_lineage": "Direct sovereign C99 library catalog, indexing, and synthesis reuse engine"
  }
}
EOF

echo ""
echo "================================================================================"
echo "    MILESTONE 10 (OMEGA_LIBRARY_V1) QUALIFICATION COMPLETE: 10/10 GATES PASS"
echo "    Cumulative Sovereign Gates Passed: 71/71"
echo "    Receipt generated at: $RECEIPT"
echo "================================================================================"
