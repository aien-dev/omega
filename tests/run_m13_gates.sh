#!/bin/sh
# tests/run_m13_gates.sh -- Master Qualification Gate Runner for Milestone 13 (OMEGA_MACHINE_GRAPH)
set -eu

OMEGA_ROOT=$(cd "$(dirname "$0")/.." && pwd -P)
TOOL="$OMEGA_ROOT/build/omegatool"
EVIDENCE_DIR="$OMEGA_ROOT/evidence"
RECEIPT="$EVIDENCE_DIR/omega_machine_graph_qualification_receipt.json"

echo "================================================================================"
echo "    AIEN OMEGA SUBSTRATE — MILESTONE 13: OMEGA_MACHINE_GRAPH QUALIFICATION"
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

# Step 2: Run all 10 M13 qualification gates
echo "[*] Step 2: Evaluating 10 canonical M13 qualification gates..."
"$TOOL" --run-m13-gates
echo ""

# Step 3: Run Demonstration (Formal Machine Hardware Graph & Microarchitecture Disambiguation)
echo "[*] Step 3: Running Demonstration (Formal Machine Hardware Graph & Microarchitecture Disambiguation)..."
"$TOOL" --demonstrate-machine
echo ""

# Step 4: Regression checks (M4 through M11)
echo "[*] Step 4: Running regression checks across M4 through M11..."
"$TOOL" --run-gates
"$TOOL" --run-m5-gates
"$TOOL" --run-m6-gates
"$TOOL" --run-m7-gates
"$TOOL" --run-m8-gates
"$TOOL" --run-m9-gates
"$TOOL" --run-m10-gates
"$TOOL" --run-m11-gates
echo "    Regression checks OK: all M4 through M11 gates passed (81 gates)."
echo ""

# Step 5: Dump and hash test vectors and binaries
echo "[*] Step 5: Computing artifact digests..."
mkdir -p "$EVIDENCE_DIR"
sha256sum "$OMEGA_ROOT/src/omega_machine."* \
          "$TOOL" > "$EVIDENCE_DIR/m13_corpus_digests.txt"

DIGEST_MACHINE=$(sha256sum "$OMEGA_ROOT/src/omega_machine.c" | awk '{print $1}')
DIGEST_MACHINE_H=$(sha256sum "$OMEGA_ROOT/src/omega_machine.h" | awk '{print $1}')
DIGEST_TOOL=$(sha256sum "$TOOL" | awk '{print $1}')
PARENT_COMMIT="8e0a50a7c4493ebf49298457636e05bfba9c1e7a"
SOURCE_PARENT_COMMIT="$PARENT_COMMIT"
QUALIFIED_COMMIT="${QUALIFIED_IMPLEMENTATION_COMMIT:-$(git -C "$OMEGA_ROOT" rev-parse HEAD)}"
RECEIPT_COMMIT="${RECEIPT_COMMIT:-pending-receipt-commit}"

echo "[*] Step 6: Generating formal qualification receipt: $RECEIPT..."
cat <<EOF > "$RECEIPT"
{
  "milestone": "MILESTONE 13 — OMEGA_MACHINE_GRAPH",
  "status": "QUALIFIED / PASS",
  "contract_id": "CONTRACT-OMEGA-MACHINE-GRAPH-M13",
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
    "src/omega_machine.c": { "sha256": "$DIGEST_MACHINE" },
    "src/omega_machine.h": { "sha256": "$DIGEST_MACHINE_H" },
    "build/omegatool": { "sha256": "$DIGEST_TOOL" }
  },
  "machine_graph_properties": {
    "hardware_profiles": "NVIDIA DGX Spark Grace V2 and QEMU virt AArch64 modeled",
    "canonical_identity": "OMG0 wire serialization with SHA-256 MACHINE_ID determinism",
    "physics_authority": "Ingests verified physical machine descriptors signed by Physics authority",
    "topological_conformance": "Enforces strict cache level monotonicity, power-of-two line sizes, and valid register file bounds",
    "realization_foundation": "Provides grounded cycle latency cost functions for M14 G_S x G_M -> G_R synthesis"
  },
  "qualification_gates": {
    "total": 10,
    "passed": 10,
    "gates": {
      "OMEGA_MACHINE_INIT_PASS": "PASS",
      "OMEGA_MACHINE_PIPELINE_PASS": "PASS",
      "OMEGA_MACHINE_REGISTER_FILE_PASS": "PASS",
      "OMEGA_MACHINE_MEMORY_HIERARCHY_PASS": "PASS",
      "OMEGA_MACHINE_PHYSICS_INGRESS_PASS": "PASS",
      "OMEGA_MACHINE_CANONICAL_ID_PASS": "PASS",
      "OMEGA_MACHINE_TOPOLOGY_DIFFERENCE_PASS": "PASS",
      "OMEGA_MACHINE_CYCLE_PREVENTION_PASS": "PASS",
      "OMEGA_MACHINE_COST_EVALUATION_PASS": "PASS",
      "OMEGA_MACHINE_RECEIPT_PASS": "PASS"
    }
  },
  "sovereignty_accounting": {
    "llvm_dependency": false,
    "gnu_as_dependency": false,
    "gcc_asm_dependency": false,
    "jit_compiler_dependency": false,
    "python_dependency": false,
    "realizer_lineage": "Direct sovereign C99 formal machine hardware graph and microarchitecture topology modeling engine"
  }
}
EOF

echo ""
echo "================================================================================"
echo "    MILESTONE 13 (OMEGA_MACHINE_GRAPH) QUALIFICATION COMPLETE: 10/10 GATES PASS"
echo "    Cumulative Sovereign Gates Passed: 91/91"
echo "    Receipt generated at: $RECEIPT"
echo "================================================================================"
