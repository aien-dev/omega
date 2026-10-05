#!/bin/sh
# M20 receipt writer mutation sweep (sibling of tools/tensor_mutations.sh,
# which calls it). For each rule marked MUT:<name> at the end of a line in
# tools/m20_receipt.sh, copy the writer to a scratch directory, break that one
# rule, run tests/test_m20_receipt.sh against the copy and require it to FAIL.
# A mutation that does not apply, or that the tests do not catch, fails the
# sweep. Host only, no device. Shell only (no Python).
#
# Thin adapter over tools/mutation_runner.sh (marker reader). The rows below
# are the mutant set; the runner copies the tree, applies each edit and runs
# the test. The sed address stays /MUT:<name>$/ (end of line): the runner adds
# its own unanchored /MUT:<name>/ address, and each row nests the anchored one
# inside a { } block so only the line ENDING in the marker is edited. This
# script maps the runner's per-mutant stderr lines back to the legacy stdout
# lines and keeps the legacy exit codes.
set -u
cd "$(dirname "$0")/.." || exit 2
SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/m20-receipt-mut.XXXXXX") || exit 2
trap 'rm -rf "$SCRATCH"' EXIT INT TERM

# Rows: name|file|sed expression (anchored to the end-of-line marker).
cat > "$SCRATCH/rows" <<'ROWS'
DIRTY_REFUSED|tools/m20_receipt.sh|{/MUT:DIRTY_REFUSED$/ s/|| refuse/|| :/}
EVID_OUTSIDE|tools/m20_receipt.sh|{/MUT:EVID_OUTSIDE$/ s/) refuse/) :/}
UNKNOWN_LABEL|tools/m20_receipt.sh|{/MUT:UNKNOWN_LABEL$/ s/refuse "row/: "row/}
NOTRUN_FORCES_NQ|tools/m20_receipt.sh|{/MUT:NOTRUN_FORCES_NQ$/ s/add_nq/:/}
GB10_NEEDS_EVIDENCE|tools/m20_receipt.sh|{/MUT:GB10_NEEDS_EVIDENCE$/ s/|| refuse/|| :/}
GB10_HASH_MATCH|tools/m20_receipt.sh|{/MUT:GB10_HASH_MATCH$/ s/|| refuse/|| :/}
HOST_LOG_PASS|tools/m20_receipt.sh|{/MUT:HOST_LOG_PASS$/ s/|| refuse/|| :/}
GB10_PARITY_REQUIRED|tools/m20_receipt.sh|{/MUT:GB10_PARITY_REQUIRED$/ s/|| add_nq/|| :/}
CLEAN_AFTER|tools/m20_receipt.sh|{/MUT:CLEAN_AFTER$/ s/|| add_nq/|| :/}
COMMIT_UNCHANGED|tools/m20_receipt.sh|{/MUT:COMMIT_UNCHANGED$/ s/|| add_nq/|| :/}
NO_OVERWRITE|tools/m20_receipt.sh|{/MUT:NO_OVERWRITE$/ s/set -o noclobber; //}
ROWS

# Baseline: the unmutated writer must pass, otherwise every mutant would
# look "caught".
if ! tests/test_m20_receipt.sh > "$SCRATCH/base.log" 2>&1; then
    grep '^FAIL' "$SCRATCH/base.log" | head -5
    echo "m20 receipt mutation sweep: FAIL (unmutated baseline does not pass)"; exit 1
fi

# The test command runs inside the runner's scratch tree and points the test
# at that tree's (mutated) writer through M20_RECEIPT_WRITER.
tools/mutation_runner.sh -k marker -m "$SCRATCH/rows" \
    -t 'M20_RECEIPT_WRITER=$PWD/tools/m20_receipt.sh tests/test_m20_receipt.sh' \
    -c "tools/m20_receipt.sh tests/test_m20_receipt.sh tests/fixtures" \
    -f FAIL -o "$SCRATCH/out.json" > "$SCRATCH/runner.out" 2> "$SCRATCH/runner.err"
rrc=$?

fail=0
total=0
while IFS= read -r line; do
    name=${line%% *}; rest=${line#* }; status=${rest%% *}
    total=$((total + 1))
    case $status in
        KILLED)
            n=$(echo "$line" | sed -n 's/.*(\([0-9][0-9]*\) failing case(s).*/\1/p'); : "${n:=0}"
            echo "MUTATION $name: caught ($n failing checks)" ;;
        ERROR) echo "MUTATION $name: NOT APPLIED (marker or pattern missing)"; fail=1 ;;
        *) echo "MUTATION $name: NOT CAUGHT (tests still pass)"; fail=1 ;;
    esac
done < "$SCRATCH/runner.err"
# Runner usage error or a row it never reported must not pass silently.
[ "$total" -eq "$(grep -c '|' "$SCRATCH/rows")" ] || fail=1
[ "$rrc" -eq 0 ] || fail=1
if [ "$fail" -ne 0 ]; then echo "m20 receipt mutation sweep: FAIL"; exit 1; fi
echo "m20 receipt mutation sweep: PASS ($total of $total mutations caught)"
