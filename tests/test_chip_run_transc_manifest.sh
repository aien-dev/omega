#!/bin/bash
# Host-only check of tools/manifests/numeric_transc.chiprun and its wrapper
# tools/run_numeric_transc_gate.sh. No GPU, no chip sources, no real build: a fake omega and
# fake physics (git repos in a temp dir), a tiny fake chip binary and fake host tier, run through
# the real module via the CHIPRUN_SELFTEST seam (same method as tests/test_chip_run.sh).
#   tests/test_chip_run_transc_manifest.sh
# Checks: (1) receipt keys and key order equal the old script's (pinned below; cross-checked with
# origin/main:tools/run_numeric_transc_gate.sh while main still has the old script); the sorted
# difference must be empty or listed in tests/chip_run_transc_receipt_allowlist.txt, and no stale
# allowlist entry may remain; (2) gcc source list equals the old script's; (3) refusal line and
# exit code for an unknown op and for a missing physics checkout; (4) verdicts, console lines,
# exit codes, receipt values, quiet flag, host tier refusal, fuser refusal.
# Each check is also run against a mutated manifest and must fail there.
set -u
HERE=$(cd -P "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MAN=$HERE/tools/manifests/numeric_transc.chiprun
ALLOW=$HERE/tests/chip_run_transc_receipt_allowlist.txt
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
FAILS=0
ok() { echo "PASS $1"; }
bad() { echo "FAIL $1"; FAILS=$((FAILS + 1)); }
FAKE_PHYS=/nonexistent-physics-for-test

# Pinned from the old script (origin/main before this change): receipt keys in order, gcc sources in order.
OLD_KEYS="gate row reference omega_commit omega_tree_clean_before omega_tree_clean_after omega_commit_unchanged_after physics_commit physics_lock_pin physics_tree_clean_before physics_tree_clean_after physics_commit_unchanged_after binary_sha256 chip_log_sha256 kernels nvdisasm_check host_tier ops_requested ops chip_exit_status started_utc finished_utc verdict"
OLD_SRCS="tests/test_omega_numeric_transc_gb10.c src/omega_numeric_divsqrt_gb10.c src/omega_numeric_transc.c src/omega_numeric.c src/omega_numeric_provenance.c src/omega_blackwell_encoder.c src/omega_blackwell_codegen.c src/omega_blackwell_matmul.c src/omega_blackwell_qmd.c src/sha256.c $FAKE_PHYS/nvrm/nvrm.c $FAKE_PHYS/m16/m16_native.c"

# Cross-check of the pins against the old script, only while origin/main still has it.
OLDSH=$(git -C "$HERE" show origin/main:tools/run_numeric_transc_gate.sh 2>/dev/null)
if printf '%s\n' "$OLDSH" | grep -q 'flock -x 9'; then
    printf '%s\n' "$OLDSH" | sed -n '/BODY=\$(jq -n/,/verdict: \$verdict }/p' | sed -E 's/\{op:[^}]*\}//g; s/"[^"]*"//g' | grep -oE '[a-z_0-9]+:' | tr -d ':' | tr '\n' ' ' | sed 's/ $//' > "$T/oldkeys"
    [ "$(cat "$T/oldkeys")" = "$OLD_KEYS" ] && ok "0 pinned key list equals the old script's" || bad "0 pinned key list differs from the old script: $(cat "$T/oldkeys")"
    printf '%s\n' "$OLDSH" | sed -n '/-o "\$BIN"/,/m16_native\.c/p' | grep -oE '(tests|src)/[A-Za-z0-9_]+\.c|\$PHYSICS/[a-z0-9/_]+\.c' | sed "s#\\\$PHYSICS#$FAKE_PHYS#" | tr '\n' ' ' | sed 's/ $//' > "$T/oldsrcs"
    [ "$(cat "$T/oldsrcs")" = "$OLD_SRCS" ] && ok "0 pinned source list equals the old script's" || bad "0 pinned source list differs from the old script: $(cat "$T/oldsrcs")"
else
    echo "NOTE 0 origin/main no longer has the old script; the pinned lists are the reference"
fi

# ---------- fake world ----------
cat > "$T/chip.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *m = getenv("FAKE_MODE"); if (!m) m = "pass";
    const char *fl = getenv("FAKE_FLAG");
    printf("noise line before results\n");
    if (fl) { FILE *f = fopen(fl, "r"); char b[512] = ""; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; fclose(f); } b[strcspn(b, "\n")] = 0; printf("noise flag=%s\n", b); }
    const char *v = !strcmp(m, "fail") ? "FAIL" : (!strcmp(m, "notrun") || !strcmp(m, "notrun0")) ? "NOT_RUN" : "PASS";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--chip")) continue;
        printf("RESULT chip %s checked=%s exhaustive=%s mismatches=%s unwritten=0 verdict=%s\n", argv[i],
               strcmp(v, "NOT_RUN") ? "4294967296" : "0", strcmp(v, "NOT_RUN") ? "true" : "false", strcmp(v, "FAIL") ? "0" : "7", v);
    }
    if (strcmp(m, "silent")) printf("VERDICT %s\n", v);
    return (strcmp(m, "pass") && strcmp(m, "silent") && strcmp(m, "notrun0")) ? 1 : 0;
}
EOC
mkdir -p "$T/bin" "$T/tmp" "$T/home"
gcc -O0 -o "$T/bin/chip" "$T/chip.c" || { echo "FAIL setup: host gcc could not build the fake chip binary"; exit 1; }
FLAG=$T/flag; LOCK=$T/gpu.lock
g() { local d=$1; shift; git -C "$d" -c user.email=t@t -c user.name=t -c commit.gpgsign=false "$@"; }

W=""; OM=""; PHYS=""; N=0
build_world() { # MANIFEST
    N=$((N + 1)); W=$T/w$N; OM=$W/omega; PHYS=$W/physics
    mkdir -p "$PHYS/nvrm" "$OM/tools/manifests" "$W/tmp"
    git init -q "$PHYS" && echo "/* fake */" > "$PHYS/nvrm/nvrm.c" && g "$PHYS" add -A && g "$PHYS" commit -q -m physics
    git init -q "$OM"
    cp "$HERE/tools/chip_run.sh" "$HERE/tools/run_numeric_transc_gate.sh" "$OM/tools/"
    cp "$1" "$OM/tools/manifests/numeric_transc.chiprun"
    git -C "$PHYS" rev-parse HEAD > "$OM/physics.lock"
    echo "build/" > "$OM/.gitignore"
    printf '#!/bin/sh\nif [ "$1" = --digest ]; then printf "EXP2 12 aaaa\\nLOG2 34 bbbb\\n"; exit ${FAKE_DIGEST_RC:-0}; fi\necho "[PASS] fake host check"; echo "FAKE_HOST_LAST"; exit ${FAKE_HOST_RC:-0}\n' > "$OM/tools/fakehost.sh"
    printf '#!/bin/sh\necho "[PASS] fake nvd"\necho "nvdisasm fake provenance line"\necho "VERDICT PASS"\nexit ${FAKE_NVD_RC:-0}\n' > "$OM/tools/divsqrt_nvdisasm_check.sh"
    printf 'build/test_omega_numeric_transc_gb10_cpu:\n\tmkdir -p build && cp tools/fakehost.sh $@ && chmod +x $@\n' > "$OM/Makefile"
    chmod +x "$OM/tools/"*.sh "$OM/tools/run_numeric_transc_gate.sh"
    g "$OM" add -A && g "$OM" commit -q -m omega
    rm -f "$FLAG"
}
OUT=$T/out.txt; RC=0
# runw NAME [ENV=VALUE ...] -- wrapper args
runw() {
    local name=$1; RUNNAME=$name; shift; local extra=(); while [ "$1" != -- ]; do extra+=("$1"); shift; done; shift
    rm -f "$FLAG"; g "$OM" clean -fdxq
    ( cd "$T" && env -u PHYSICS PHYSICS_DIR="$PHYS" CHIPRUN_SELFTEST=1 CHIPRUN_PREBUILT_BIN="$T/bin/chip" CHIPRUN_QUIET_FLAG="$FLAG" \
        CHIPRUN_GPU_LOCK="$LOCK" CHIPRUN_EST_LOAD_CMD=true TMPDIR="$W/tmp" TRANSC_GB10_EVIDENCE_DIR="$W/ev-$name" FAKE_FLAG="$FLAG" \
        FAKE_MODE=pass "${extra[@]}" bash "$OM/tools/run_numeric_transc_gate.sh" "$@" ) > "$OUT" 2>&1
    RC=$?
    R=$(ls "$W"/ev-"$name"/*.json 2>/dev/null | head -1)
}

# ck_diag: after a failing check, show the first 15 lines of the last run's console, once per run name
# (RUNNAME is set by runw and by the two inline runs). Lines start with "     | " so they never
# match the "^PASS" / "^FAIL" greps below. It does not change what counts as a pass.
ck_diag() { [ "${DIAG_NAME:-}" = "${RUNNAME:-}" ] && return 0; DIAG_NAME=${RUNNAME:-}; head -n 15 "$OUT" 2>/dev/null | sed 's/^/     | /'; return 0; }

# suite MANIFEST: every world-based check; prints "PASS id" / "FAIL id" lines.
suite() {
    local man=$1 id k; RUNNAME=""; DIAG_NAME=""
    build_world "$man"
    ck() { id=$1; shift; if "$@"; then echo "PASS $id"; else echo "FAIL $id"; ck_diag; fi; }
    last_is() { [ "$(tail -n 1 "$OUT")" = "$1" ]; }
    has() { grep -Eq -- "$1" "$OUT"; }
    lacks() { ! grep -Eq -- "$1" "$OUT"; }
    rj() { [ -n "${R:-}" ] && jq -e "$1" "$R" > /dev/null 2>&1; }

    # happy path, default ops through the wrapper
    runw happy -- ; HAPPY_R=$R
    ck happy_exit test "$RC" = 0
    ck happy_last_line last_is "VERDICT PASS"
    ck happy_results_echoed test "$(grep -c '^RESULT chip ' "$OUT")" = 9
    ck happy_noise_not_echoed lacks '^noise'
    ck happy_receipt_line has '^RECEIPT /.*\.json$'
    ck happy_no_chiprun_line lacks '^CHIP_RUN:'
    ck happy_default_ops rj '.ops_requested == ["EXP2","LOG2","SIGMOID","TANH","SIN","COS","ERF","GELU","RSQRT"] and (.ops | map(.op)) == ["EXP2","LOG2","SIGMOID","TANH","SIN","COS","ERF","GELU","RSQRT"]'
    ck happy_ops_typed rj '.ops[0] == {op:"EXP2",checked:4294967296,exhaustive:true,mismatches:0,unwritten:0,verdict:"PASS"}'
    ck happy_kernels rj '.kernels == [{op:"EXP2",instructions:12,sha256:"aaaa"},{op:"LOG2",instructions:34,sha256:"bbbb"}]'
    ck happy_nvd rj '.nvdisasm_check == "nvdisasm fake provenance line"'
    ck happy_host_tier rj '.host_tier == "FAKE_HOST_LAST"'
    ck happy_gate_fields rj '.gate == "E1-TRANSC-GB10" and .verdict == "PASS" and .chip_exit_status == 0 and (.row | startswith("E1 gap table row 10")) and (.reference | startswith("omega_math_<op>"))'
    ck happy_flag_dropped test ! -e "$FLAG"
    ck happy_flag_owner grep -q 'flag=lane2 E1 transcendental GB10 chip run start=' "$W"/ev-happy/blobs/*.log
    ck happy_mode_0444 test "$(stat -c %a "$R" 2>/dev/null)" = 444
    # (1) receipt keys, ordered, equal the old script's
    jq -r 'keys_unsorted | join(" ")' "$HAPPY_R" > "$W/newkeys" 2>/dev/null
    ck keys_ordered test "$(cat "$W/newkeys")" = "$OLD_KEYS"
    # (1b) sorted difference against the allowlist, both directions
    tr ' ' '\n' <<< "$OLD_KEYS" | sort > "$W/ko"; tr ' ' '\n' < "$W/newkeys" | sed '/^$/d' | sort > "$W/kn"
    { comm -13 "$W/ko" "$W/kn" | sed 's/^/+/'; comm -23 "$W/ko" "$W/kn" | sed 's/^/-/'; } | sort > "$W/diff"
    grep -E '^[+-][a-z_0-9]+ ' "$ALLOW" | cut -d' ' -f1 | sort > "$W/allowed"
    ck keys_allowlist cmp -s "$W/diff" "$W/allowed"

    # other verdicts: exit code, console, receipt verdict
    runw notrun FAKE_MODE=notrun -- EXP2
    ck notrun_exit test "$RC" = 1
    ck notrun_last_line last_is "VERDICT NOT_RUN"
    ck notrun_receipt rj '.verdict == "NOT_RUN" and .ops[0].exhaustive == false and .ops_requested == ["EXP2"]'
    runw fail FAKE_MODE=fail -- LOG2 TANH
    ck fail_exit test "$RC" = 1
    ck fail_last_line last_is "VERDICT FAIL"
    ck fail_receipt rj '.verdict == "FAIL" and .ops[0].mismatches == 7 and .ops_requested == ["LOG2","TANH"]'
    runw silent FAKE_MODE=silent -- EXP2
    ck silent_exit test "$RC" = 1
    ck silent_last_line last_is "VERDICT NOT_RUN"
    ck silent_receipt rj '.verdict == "NOT_RUN" and .chip_exit_status == 0'
    runw notrun0 FAKE_MODE=notrun0 -- EXP2
    ck notrun0_exit test "$RC" = 1
    ck notrun0_last_line last_is "VERDICT NOT_RUN"
    ck notrun0_receipt rj '.verdict == "NOT_RUN" and .chip_exit_status == 0'
    runw passrc FAKE_MODE=passrc -- EXP2
    ck passrc_exit test "$RC" = 1
    ck passrc_last_line last_is "VERDICT FAIL"
    ck passrc_receipt rj '.verdict == "FAIL" and .chip_exit_status == 1'

    # (3) refusals: exit 1 and VERDICT NOT_RUN
    runw badop -- EXP2 NOPE
    ck badop_refusal test "$RC" = 1 -a "$(cat "$OUT")" = "$(printf 'REFUSED: unknown op NOPE\nVERDICT NOT_RUN')"
    ck badop_nothing_written test -z "$(ls "$W"/ev-badop 2>/dev/null)" -a ! -e "$FLAG"
    runw nophys PHYSICS_DIR="$T/nonexistent" -- EXP2
    ck nophys_refusal test "$RC" = 1
    ck nophys_lines test "$(sed -n 1p "$OUT")" = "REFUSED: no physics checkout at $T/nonexistent" -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    runw defphys HOME="$T/home" PHYSICS_DIR= -- EXP2
    ck default_physics_dir has 'REFUSED: no physics checkout at .*/home/workspace/hive-worktrees/physics-gate14-e95e3ed$'
    runw hostfail FAKE_HOST_RC=1 -- EXP2
    ck hostfail_refusal test "$RC" = 1 -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    ck hostfail_text has '^REFUSED: host tier failed'
    ck hostfail_no_chip lacks '^== chip run'
    ck hostfail_no_flag test ! -e "$FLAG"
    runw nvdfail FAKE_NVD_RC=1 -- EXP2
    ck nvdfail_refusal test "$RC" = 1 -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    ck nvdfail_no_chip lacks '^== chip run'
    ck nvdfail_text has '^host tier \(0\) or nvdisasm check \(1\) failed$'
    runw digfail FAKE_DIGEST_RC=1 -- EXP2
    ck digfail_refusal test "$RC" = 1 -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    ck digfail_no_chip lacks '^== chip run'
    ck digfail_text has '^kernel digest failed$'
    # fuser: any open handle on the lock file refuses, even without flock
    exec 8> "$LOCK"; runw fuser -- EXP2; exec 8>&-
    ck fuser_refusal test "$RC" = 1 -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    ck fuser_text has "^REFUSED: $LOCK is held\$"
    ck fuser_no_host_tier lacks '^== host tier'
    # dirty omega
    echo x > "$OM/junk"; rm -f "$FLAG"; RUNNAME=dirty
    ( cd "$T" && env -u PHYSICS PHYSICS_DIR="$PHYS" CHIPRUN_SELFTEST=1 CHIPRUN_PREBUILT_BIN="$T/bin/chip" CHIPRUN_QUIET_FLAG="$FLAG" CHIPRUN_GPU_LOCK="$LOCK" \
        CHIPRUN_EST_LOAD_CMD=true TMPDIR="$W/tmp" TRANSC_GB10_EVIDENCE_DIR="$W/ev-dirty" bash "$OM/tools/run_numeric_transc_gate.sh" EXP2 ) > "$OUT" 2>&1; RC=$?
    rm -f "$OM/junk"
    ck dirty_refusal test "$RC" = 1 -a "$(sed -n 1p "$OUT")" = "REFUSED: omega tree $OM is dirty" -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    # quiet flag up
    echo other > "$FLAG"; RUNNAME=flagup
    ( cd "$T" && env -u PHYSICS PHYSICS_DIR="$PHYS" CHIPRUN_SELFTEST=1 CHIPRUN_PREBUILT_BIN="$T/bin/chip" CHIPRUN_QUIET_FLAG="$FLAG" CHIPRUN_GPU_LOCK="$LOCK" \
        CHIPRUN_EST_LOAD_CMD=true TMPDIR="$W/tmp" TRANSC_GB10_EVIDENCE_DIR="$W/ev-flagup" bash "$OM/tools/run_numeric_transc_gate.sh" EXP2 ) > "$OUT" 2>&1; RC=$?
    ck flagup_refusal test "$RC" = 1 -a "$(sed -n 1p "$OUT")" = "REFUSED: quiet flag is up: other" -a "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
    ck flagup_left_alone test "$(cat "$FLAG")" = other
    rm -f "$FLAG"
}

# (2) gcc source list
load_list() { ( PHYSICS=$FAKE_PHYS; . "$1" 2>/dev/null || exit 1; read -ra A <<< "$TEST_SOURCE $SOURCES $EXTRA_BUILD_SOURCES"; echo "${A[*]}" ); }

suite "$MAN" > "$T/suite.out"
cat "$T/suite.out"; grep -c '^FAIL' "$T/suite.out" > "$T/nfail"; FAILS=$((FAILS + $(cat "$T/nfail")))
[ "$(grep -c '^PASS' "$T/suite.out")" -ge 45 ] || bad "suite ran fewer than 45 checks"
[ "$(load_list "$MAN")" = "$OLD_SRCS" ] && ok "2 source list identical to old script" || bad "2 source list: $(load_list "$MAN")"

# every file the manifest names exists in the real tree
( PHYSICS=$FAKE_PHYS; . "$MAN"; for s in $TEST_SOURCE $SOURCES; do [ -f "$HERE/$s" ] || { echo "missing $s"; exit 1; }; done ) && ok "2b sources exist" || bad "2b a named source is missing"

# ---------- mutants: each manifest or wrapper change must make a named check fail ----------
mutant() { # NAME SED-EXPR EXPECTED-FAILING-ID
    local name=$1 expr=$2 want=$3
    sed "$expr" "$MAN" > "$T/m_$name"
    cmp -s "$T/m_$name" "$MAN" && { bad "mutant $name changed nothing"; return; }
    suite "$T/m_$name" > "$T/m_$name.out" 2>&1
    grep -q "^FAIL $want\$" "$T/m_$name.out" && ok "mutant $name fails $want" || bad "mutant $name did not fail $want"
}
mutant nokernels 's/^  kernels:/  kernels_x:/' keys_ordered
mutant renamekey 's/^  nvdisasm_check: /  zz_nvdisasm_check: /' keys_ordered
mutant extrakey 's/^  gate: \$r.gate,/  gate: $r.gate, owner: $r.owner,/' keys_allowlist
mutant dropkey '/^  row: /d' keys_allowlist
mutant verdictword 's/elif \$cw == "" then "NOT_RUN"/elif $cw == "" then "FAIL"/' silent_receipt
mutant failword 's/elif \$cw == "PASS" then "FAIL"/elif $cw == "PASS" then "PASS"/' passrc_receipt
mutant refuseexit 's/^REFUSE_EXIT=1/REFUSE_EXIT=2/' nophys_refusal
mutant refuseline "s/^REFUSE_VERDICT_LINE=.*/REFUSE_VERDICT_LINE='CHIP_RUN: NOT_RUN'/" nophys_lines
mutant style 's/^FINAL_LINE_STYLE=verdict/FINAL_LINE_STYLE=chip_run/' happy_last_line
mutant echore '/^ECHO_RE=/d' happy_noise_not_echoed
mutant nofuser 's/^GPU_LOCK_MODE=fuser/GPU_LOCK_MODE=flock/' fuser_refusal
mutant noquiet 's/^RAISE_QUIET=1/RAISE_QUIET=0/' happy_flag_owner
mutant owner 's/^OWNER="lane2 E1 transcendental GB10 chip run"/OWNER="other owner"/' happy_flag_owner
mutant gate 's/^GATE=E1-TRANSC-GB10/GATE=E1-TRANSC/' happy_gate_fields
mutant nohost "/^HOST_TIER_CMD=/,/^'\$/c\\HOST_TIER_CMD=true" hostfail_refusal
mutant dirtyok 's/^REFUSE_DIRTY_OMEGA=1/REFUSE_DIRTY_OMEGA=0/' dirty_refusal
mutant nonvd 's#^tools/divsqrt_nvdisasm_check.sh > #true > #' nvdfail_refusal
mutant nokdig 's#^build/test_omega_numeric_transc_gb10_cpu --digest > .*#true#' happy_kernels
mutant nokdigcheck 's#^build/test_omega_numeric_transc_gb10_cpu --digest > \(.*\) || { echo "kernel digest failed"; exit 1; }$#build/test_omega_numeric_transc_gb10_cpu --digest > \1#' digfail_refusal
mutant pass_loose "s/^PASS_LINE=.*/PASS_LINE='^VERDICT'/" notrun0_receipt
sed 's#^SOURCES="src/omega_numeric_divsqrt_gb10.c #SOURCES="#' "$MAN" > "$T/m_dropsrc"
sed 's#src/sha256.c#src/sha256_nope.c#' "$MAN" > "$T/m_badfile"
for m in m_dropsrc m_badfile; do [ "$(load_list "$T/$m")" = "$OLD_SRCS" ] && bad "mutant $m passed check 2" || ok "mutant $m fails check 2"; done
( PHYSICS=$FAKE_PHYS; . "$T/m_badfile"; for s in $TEST_SOURCE $SOURCES; do [ -f "$HERE/$s" ] || exit 1; done ) && bad "mutant badfile passed 2b" || ok "mutant badfile fails 2b"

[ "$FAILS" = 0 ] && { echo "CHIP_RUN_TRANSC_MANIFEST: PASS"; exit 0; }
echo "CHIP_RUN_TRANSC_MANIFEST: FAIL ($FAILS)"; exit 1
