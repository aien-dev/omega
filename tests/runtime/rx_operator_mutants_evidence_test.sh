#!/bin/bash
# Host self-test for the evidence rules of tests/runtime/rx_operator_mutants.sh (omega #313 item 2, #314).
# Runs the script inside a fake tree with a stub compiler and a stub operator test; no real build, no chip.
#   1. a failing run keeps its logs outside the work folder (RX_OP_MUT_KEEP);
#   2. mutant programs run in their own folder: their build/qual-runs is not the fake tree's;
#   3. a mutant program that does write under the real build/qual-runs makes the script FAIL.
set -u
HERE=$(cd "$(dirname "$0")/../.." && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/rxopmut.XXXXXX"); trap 'rm -rf "$T"' EXIT
FAILS=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; FAILS=$((FAILS + 1)); fi; }
F=$T/tree
mkdir -p "$F/tests/runtime" "$F/src" "$F/tests/fabric" "$F/tools" "$F/build/qual-runs/existing"
cp "$HERE/tests/runtime/rx_operator_mutants.sh" "$F/tests/runtime/"
# Stub of the operator test: runs the built program once (as the real test does) and reports one failed check.
printf '#!/bin/bash\n"$2" > /dev/null 2>&1 < /dev/null\necho "[-] stub check failed"\nexit 1\n' > "$F/tests/runtime/rx_operator_host.sh"
echo keep > "$F/build/qual-runs/existing/sentinel"
printf '#!/bin/sh\n:\n' > "$T/cli"; chmod +x "$T/cli"
# Stub compiler: the "program" it builds writes a receipt relative to its working folder, and, when
# STUB_REAL is set, also straight into the real build/qual-runs.
cat > "$T/cc" <<'STUB'
#!/bin/bash
while [ $# -gt 0 ]; do [ "$1" = -o ] && { out=$2; break; }; shift; done
cat > "$out" <<PROG
#!/bin/bash
mkdir -p build/qual-runs/stub && echo receipt > build/qual-runs/stub/r.json
[ -n "${STUB_REAL:-}" ] && { mkdir -p "$STUB_REAL/build/qual-runs/leak"; echo x > "$STUB_REAL/build/qual-runs/leak/r.json"; }
exit 1
PROG
chmod +x "$out"
STUB
chmod +x "$T/cc"
run() { # keepdir, extra env...
    local keep=$1; shift
    env "$@" CC="$T/cc" CFLAGS= RX_PROD_ARGUS_FLAGS= RX_R13_SRCS=x RX_PROD_ARGUS_SRCS=y AIENOS_CAP_LIB= \
        RX_OP_MUT_KEEP="$keep" bash "$F/tests/runtime/rx_operator_mutants.sh" "$T/cli" 2>&1
}
before=$(find "$F/build/qual-runs" | LC_ALL=C sort)
out=$(run "$T/kept1" X=1); rc=$?
check "failing run exits non-zero" '[ $rc != 0 ]'
check "failing run keeps its logs" '[ -s "$T/kept1/base/run.log" ] && [ -s "$T/kept1/base/build.log" -o -e "$T/kept1/base/build.log" ]'
check "failing run says where the logs are" 'echo "$out" | grep -q "logs kept in $T/kept1"'
check "kept logs include the mutant receipt folder" '[ -s "$T/kept1/base/qual-runs/stub/r.json" ]'
check "mutant receipt did not land in the real build/qual-runs" '[ "$(find "$F/build/qual-runs" | LC_ALL=C sort)" = "$before" ]'
check "no leak reported when nothing leaked" '! echo "$out" | grep -q "wrote under the real"'
out=$(run "$T/kept2" STUB_REAL="$F"); rc=$?
check "a program writing under the real build/qual-runs is reported" 'echo "$out" | grep -q "FAIL mutant runs wrote under the real"'
check "that run exits non-zero" '[ $rc != 0 ]'
mkdir -p "$T/kept3"
out=$(run "$T/kept3" X=1)
check "an existing keep folder is not overwritten" 'echo "$out" | grep -q "not keeping logs" && [ -z "$(ls -A "$T/kept3")" ]'
echo "rx_operator_mutants_evidence_test: $FAILS failures"
[ "$FAILS" = 0 ]
