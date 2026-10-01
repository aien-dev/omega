#!/bin/sh
# M20 receipt writer mutation sweep (sibling of tools/tensor_mutations.sh,
# which calls it). For each rule marked MUT:<name> at the end of a line in
# tools/m20_receipt.sh, copy the writer to a scratch directory, break that one
# rule, run tests/test_m20_receipt.sh against the copy and require it to FAIL.
# A mutation that does not apply, or that the tests do not catch, fails the
# sweep. Host only, no device. Shell only (no Python).
set -u
cd "$(dirname "$0")/.." || exit 2
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/m20-receipt-mut.XXXXXX") || exit 2
trap 'rm -rf "$SCRATCH"' EXIT INT TERM

# name@sed expression (applied only to the line ending in MUT:<name>)
MUTATIONS='DIRTY_REFUSED@s/|| refuse/|| :/
EVID_OUTSIDE@s/) refuse/) :/
UNKNOWN_LABEL@s/refuse "row/: "row/
NOTRUN_FORCES_NQ@s/add_nq/:/
GB10_NEEDS_EVIDENCE@s/|| refuse/|| :/
GB10_HASH_MATCH@s/|| refuse/|| :/
HOST_LOG_PASS@s/|| refuse/|| :/
GB10_PARITY_REQUIRED@s/|| add_nq/|| :/
CLEAN_AFTER@s/|| add_nq/|| :/
COMMIT_UNCHANGED@s/|| add_nq/|| :/
NO_OVERWRITE@s/set -o noclobber; //'

# Baseline: the unmutated writer must pass, otherwise every mutant would
# look "caught".
if ! tests/test_m20_receipt.sh > "$SCRATCH/base.log" 2>&1; then
    grep '^FAIL' "$SCRATCH/base.log" | head -5
    echo "m20 receipt mutation sweep: FAIL (unmutated baseline does not pass)"; exit 1
fi
fail=0
total=0
old_ifs=$IFS
IFS='
'
for m in $MUTATIONS; do
    IFS=$old_ifs
    name=${m%%@*}; expr=${m#*@}
    total=$((total + 1))
    cp tools/m20_receipt.sh "$SCRATCH/m20_receipt.sh"
    sed -i "/MUT:$name\$/ $expr" "$SCRATCH/m20_receipt.sh"
    if cmp -s tools/m20_receipt.sh "$SCRATCH/m20_receipt.sh"; then
        echo "MUTATION $name: NOT APPLIED (marker or pattern missing)"; fail=1; continue
    fi
    if M20_RECEIPT_WRITER="$SCRATCH/m20_receipt.sh" tests/test_m20_receipt.sh > "$SCRATCH/run.log" 2>&1; then
        echo "MUTATION $name: NOT CAUGHT (tests still pass)"; fail=1
    else
        echo "MUTATION $name: caught ($(grep -c '^FAIL' "$SCRATCH/run.log") failing checks)"
    fi
done
IFS=$old_ifs
if [ "$fail" -ne 0 ]; then echo "m20 receipt mutation sweep: FAIL"; exit 1; fi
echo "m20 receipt mutation sweep: PASS ($total of $total mutations caught)"
