#!/bin/bash
# Host-only check of tools/manifests/unwritten_trap.chiprun and its wrapper. No GPU, no build.
#   tests/test_chip_run_trap_manifest.sh
# Checks: (1) manifest sets every required variable and names only existing sources;
# (2) its gcc source list equals the old script's (origin/main:tools/run_unwritten_trap.sh);
# (3) the wrapper with a missing physics dir prints REFUSED and the NOT_RUN line, exit 2.
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
    [ "$EVIDENCE_DIR" = "$HOME/workspace/evidence-out/OMEGA-UNWRITTEN-TRAP" ] && [ "${REQUIRE_ALL_PASS:-}" = 0 ] || { echo "evidence dir or REQUIRE_ALL_PASS wrong"; exit 1; }
    for s in $TEST_SOURCE $SOURCES; do [ -f "$HERE/$s" ] || { echo "missing $s"; exit 1; }; done ); }
check_list_matches_old() {
    git -C "$HERE" show origin/main:tools/run_unwritten_trap.sh > "$T/old.sh" 2>/dev/null || { echo "cannot read old script"; return 1; }
    sed -n '/-o "\$BIN"/,/m16_native\.c/p' "$T/old.sh" | tr ' ' '\n' | grep '\.c"\?$' | tr -d '"' | sed "s#\\\$PHYSICS#$FAKE_PHYS#" > "$T/old.list"
    [ -s "$T/old.list" ] || { echo "old list empty"; return 1; }
    load_list "$1" > "$T/new.list" || { echo "manifest failed to load"; return 1; }
    diff "$T/old.list" "$T/new.list" > "$T/diff.out" || { cat "$T/diff.out"; return 1; }; }

# Mutants of the manifest.
sed 's#^SOURCES="src/omega_unwritten_trap.c #SOURCES="#' "$MAN" > "$T/m_dropsrc"
sed 's#src/sha256.c#src/sha256_nope.c#' "$MAN" > "$T/m_badfile"
sed '/^PASS_LINE=/d' "$MAN" > "$T/m_novar"
sed 's#^REFUSE_EXIT=2#REFUSE_EXIT=1#' "$MAN" > "$T/m_exit"
sed "s#evidence-out/OMEGA-UNWRITTEN-TRAP#evidence/unwritten_trap#" "$MAN" > "$T/m_evid"

check_vars "$MAN" > "$T/o" 2>&1 && ok "1 manifest variables and source files" || { bad "1 manifest variables and source files: $(cat "$T/o")"; }
check_list_matches_old "$MAN" > "$T/o" 2>&1 && ok "2 source list identical to old script" || bad "2 source list identical to old script: $(cat "$T/o")"

for m in m_novar m_badfile m_exit m_evid; do check_vars "$T/$m" > /dev/null 2>&1 && bad "mutant $m passed check 1" || ok "mutant $m fails check 1"; done
for m in m_dropsrc m_badfile; do check_list_matches_old "$T/$m" > /dev/null 2>&1 && bad "mutant $m passed check 2" || ok "mutant $m fails check 2"; done

OUT=$(env -u PHYSICS_DIR PHYSICS=$FAKE_PHYS bash "$HERE/tools/run_unwritten_trap.sh" 2>&1); RC=$?
{ [ "$RC" = 2 ] && printf '%s\n' "$OUT" | grep -q '^REFUSED: no physics checkout' && printf '%s\n' "$OUT" | grep -qx 'OMEGA_UNWRITTEN_TRAP: NOT_RUN'; } && ok "3 wrapper refuses with exit 2 and NOT_RUN line" || bad "3 wrapper refusal (rc=$RC): $OUT"
# Mutant: the same wrapper pointed at a manifest with the wrong refuse exit must not give 2.
cp "$T/m_exit" "$T/m_exit.chiprun"
OUT=$(env -u PHYSICS_DIR PHYSICS=$FAKE_PHYS bash "$HERE/tools/chip_run.sh" "$T/m_exit.chiprun" 2>&1); RC=$?
[ "$RC" = 2 ] && bad "mutant wrong REFUSE_EXIT still exits 2" || ok "mutant wrong REFUSE_EXIT does not exit 2"

[ "$FAILS" = 0 ] && { echo "CHIP_RUN_TRAP_MANIFEST: PASS"; exit 0; }
echo "CHIP_RUN_TRAP_MANIFEST: FAIL ($FAILS)"; exit 1
