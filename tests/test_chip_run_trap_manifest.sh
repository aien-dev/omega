#!/bin/bash
# Host-only check of tools/manifests/unwritten_trap.chiprun and its wrapper. No GPU, no build.
#   tests/test_chip_run_trap_manifest.sh
# Checks: (1) manifest sets every required variable and names only existing sources;
# (2) its gcc source list equals the old script's, pinned as a literal (git show 2dc9dd8:tools/run_unwritten_trap.sh);
# (3) the wrapper with a missing physics dir prints REFUSED and the NOT_RUN line, exit 2;
# (4) the module gcc flags equal the old script's; (5) the wrapper passes exit 0/1/2 through;
# (6) the manifest sets CHILD_EXIT_PASSTHROUGH=1 and the old default args.
# Each check is also run against a mutated manifest and must fail there.
set -u
HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MAN=$HERE/tools/manifests/unwritten_trap.chiprun
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
FAILS=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
FAKE_PHYS=/nonexistent-physics-for-test

# Prints "<TEST_SOURCE> <SOURCES> <EXTRA>" as one token per line, or fails.
load_list() { ( PHYSICS=$FAKE_PHYS; . "$1" 2>/dev/null || exit 1; read -ra A <<< "$TEST_SOURCE $SOURCES $EXTRA_BUILD_SOURCES"; printf '%s\n' "${A[@]}" ); }
check_vars() { ( PHYSICS=$FAKE_PHYS; . "$1" 2>/dev/null || exit 1
    for v in GATE OWNER TEST_SOURCE SOURCES EXTRA_BUILD_SOURCES RUN_ARGS VERDICT_RE PASS_LINE EVIDENCE_DIR REFUSE_VERDICT_LINE REFUSE_EXIT REFUSE_DIRTY_OMEGA; do
        [ -n "${!v:-}" ] || { echo "unset $v"; exit 1; }; done
    [ "$REFUSE_EXIT" = 2 ] && [ "$REFUSE_VERDICT_LINE" = "OMEGA_UNWRITTEN_TRAP: NOT_RUN" ] && [ "$RUN_ARGS" = "--chip --repeats 3000" ] || { echo "old exit/line/args not kept"; exit 1; }
    [ "$EVIDENCE_DIR" = "$HOME/workspace/evidence-out/OMEGA-UNWRITTEN-TRAP" ] && [ "${REQUIRE_ALL_PASS:-}" = 0 ] && [ "${CHILD_EXIT_PASSTHROUGH:-}" = 1 ] || { echo "evidence dir, REQUIRE_ALL_PASS or CHILD_EXIT_PASSTHROUGH wrong"; exit 1; }
    for s in $TEST_SOURCE $SOURCES; do [ -f "$HERE/$s" ] || { echo "missing $s"; exit 1; }; done ); }
# Old script, pinned as literals. Source: git show 2dc9dd8:tools/run_unwritten_trap.sh (the last
# commit before the wrapper replaced it). origin/main no longer holds it once the wrapper merges.
OLD_LIST='tests/test_omega_unwritten_trap_gb10.c
src/omega_unwritten_trap.c
src/omega_numeric_divsqrt_gb10.c
src/omega_numeric_transc.c
src/omega_numeric.c
src/omega_numeric_provenance.c
src/omega_blackwell_encoder.c
src/omega_blackwell_codegen.c
src/omega_blackwell_matmul.c
src/omega_blackwell_qmd.c
src/sha256.c
/nonexistent-physics-for-test/nvrm/nvrm.c
/nonexistent-physics-for-test/m16/m16_native.c'
# gcc flags and include paths of the old script, in order (the -o and its sources excluded).
OLD_FLAGS='-std=gnu11
-O2
-Wall
-Wextra
-Werror
-ffp-contract=off
-fno-fast-math
-pthread
-Isrc
-I"$PHYSICS/nvrm"
-I"$PHYSICS/m16"
-I"$NV/src/common/sdk/nvidia/inc"
-I"$NV/kernel-open/common/inc"
-I"$NV/kernel-open/nvidia-uvm"
-I"$NV/src/nvidia/arch/nvalloc/unix/include"'
check_list_matches_old() {
    printf '%s\n' "$OLD_LIST" > "$T/old.list"
    load_list "$1" > "$T/new.list" || { echo "manifest failed to load"; return 1; }
    diff "$T/old.list" "$T/new.list" > "$T/diff.out" || { cat "$T/diff.out"; return 1; }; }
# $1 = a copy of tools/chip_run.sh. Its gcc flags must equal OLD_FLAGS.
check_flags_match_old() {
    sed -n '/gcc -std=gnu11/,/-o "\$BIN"/p' "$1" | tr -s ' \\\n' '\n\n\n' | grep -E '^-' | grep -vx -- '-o' > "$T/new.flags"
    printf '%s\n' "$OLD_FLAGS" > "$T/old.flags"
    [ -s "$T/new.flags" ] || { echo "no gcc line found"; return 1; }
    diff "$T/old.flags" "$T/new.flags" > "$T/diff.out" || { cat "$T/diff.out"; return 1; }; }
# $1 = a copy of the wrapper. With a stub chip_run.sh beside it that exits $STUB_RC, the wrapper
# must exit with exactly that code (the old script's 0 clean, 1 hits or device error, 2 NOT_RUN).
check_wrapper_exit() {
    local d=$T/w$RANDOM; mkdir -p "$d/tools/manifests"; cp "$1" "$d/tools/run_unwritten_trap.sh"
    printf '#!/bin/bash\nexit "$STUB_RC"\n' > "$d/tools/chip_run.sh"; : > "$d/tools/manifests/unwritten_trap.chiprun"
    local rc; for rc in 0 1 2; do STUB_RC=$rc bash "$d/tools/run_unwritten_trap.sh" > /dev/null 2>&1; [ "$?" = "$rc" ] || { echo "stub exit $rc came out as $?"; return 1; }; done; }

# Mutants of the manifest.
sed 's#^SOURCES="src/omega_unwritten_trap.c #SOURCES="#' "$MAN" > "$T/m_dropsrc"
sed 's#src/sha256.c#src/sha256_nope.c#' "$MAN" > "$T/m_badfile"
sed '/^PASS_LINE=/d' "$MAN" > "$T/m_novar"
sed 's#^REFUSE_EXIT=2#REFUSE_EXIT=1#' "$MAN" > "$T/m_exit"
sed "s#evidence-out/OMEGA-UNWRITTEN-TRAP#evidence/unwritten_trap#" "$MAN" > "$T/m_evid"
sed '/^CHILD_EXIT_PASSTHROUGH=/d' "$MAN" > "$T/m_nopass"
sed 's#^RUN_ARGS="--chip --repeats 3000"#RUN_ARGS="--chip --repeats 300"#' "$MAN" > "$T/m_args"
sed 's#-Werror #-Wno-error #' "$HERE/tools/chip_run.sh" > "$T/cr_noerror"
sed 's#-O2 #-O3 #' "$HERE/tools/chip_run.sh" > "$T/cr_o3"
sed 's#-fno-fast-math ##' "$HERE/tools/chip_run.sh" > "$T/cr_fastmath"
sed 's#^exec \(.*\)$#\1; [ $? = 0 ] || exit 1#' "$HERE/tools/run_unwritten_trap.sh" > "$T/w_map"

check_vars "$MAN" > "$T/o" 2>&1 && ok "1 manifest variables and source files" || { bad "1 manifest variables and source files: $(cat "$T/o")"; }
check_list_matches_old "$MAN" > "$T/o" 2>&1 && ok "2 source list identical to old script" || bad "2 source list identical to old script: $(cat "$T/o")"

for m in m_novar m_badfile m_exit m_evid m_nopass m_args; do check_vars "$T/$m" > /dev/null 2>&1 && bad "mutant $m passed check 1" || ok "mutant $m fails check 1"; done
for m in m_dropsrc m_badfile; do check_list_matches_old "$T/$m" > /dev/null 2>&1 && bad "mutant $m passed check 2" || ok "mutant $m fails check 2"; done

check_flags_match_old "$HERE/tools/chip_run.sh" > "$T/o" 2>&1 && ok "4 module gcc flags identical to old script" || bad "4 module gcc flags identical to old script: $(cat "$T/o")"
for m in cr_noerror cr_o3 cr_fastmath; do check_flags_match_old "$T/$m" > /dev/null 2>&1 && bad "mutant $m passed check 4" || ok "mutant $m fails check 4"; done
check_wrapper_exit "$HERE/tools/run_unwritten_trap.sh" > "$T/o" 2>&1 && ok "5 wrapper passes exit 0, 1, 2 through" || bad "5 wrapper passes exit 0, 1, 2 through: $(cat "$T/o")"
check_wrapper_exit "$T/w_map" > /dev/null 2>&1 && bad "mutant w_map passed check 5" || ok "mutant w_map fails check 5"
OUT=$(env -u PHYSICS_DIR PHYSICS=$FAKE_PHYS bash "$HERE/tools/run_unwritten_trap.sh" 2>&1); RC=$?
{ [ "$RC" = 2 ] && printf '%s\n' "$OUT" | grep -q '^REFUSED: no physics checkout' && printf '%s\n' "$OUT" | grep -qx 'OMEGA_UNWRITTEN_TRAP: NOT_RUN'; } && ok "3 wrapper refuses with exit 2 and NOT_RUN line" || bad "3 wrapper refusal (rc=$RC): $OUT"
# Mutant: the same wrapper pointed at a manifest with the wrong refuse exit must not give 2.
cp "$T/m_exit" "$T/m_exit.chiprun"
OUT=$(env -u PHYSICS_DIR PHYSICS=$FAKE_PHYS bash "$HERE/tools/chip_run.sh" "$T/m_exit.chiprun" 2>&1); RC=$?
[ "$RC" = 2 ] && bad "mutant wrong REFUSE_EXIT still exits 2" || ok "mutant wrong REFUSE_EXIT does not exit 2"

[ "$FAILS" = 0 ] && { echo "CHIP_RUN_TRAP_MANIFEST: PASS"; exit 0; }
echo "CHIP_RUN_TRAP_MANIFEST: FAIL ($FAILS)"; exit 1
