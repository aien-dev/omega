#!/bin/bash
# tests/test_chip_run.sh -- host-only self-test of tools/chip_run.sh. No GPU, no chip sources.
# Builds a throwaway fake omega and fake physics (git repos) in a mktemp dir and runs the module
# against tiny fake chip binaries (compiled with host gcc from 3-20 line C files, because the
# module's nm check needs real ELF files). Prints one line per case and a final
# "CHIP_RUN_SELFTEST: PASS" or "CHIP_RUN_SELFTEST: FAIL"; exits nonzero on any failure.
#   tests/test_chip_run.sh             run every case
#   tests/test_chip_run.sh --mutants   mutation check: for each "# REFUSAL:<id>" line in the
#       module, delete that line in a temp copy and require this test to report "FAIL <id>:"
#       (MODULE=path runs the test against any copy of the module; do this by hand to try a mutant)
# Needs: bash git jq gcc nm flock coreutils.
set -u
TESTS_DIR=$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SRC_MODULE=${MODULE:-$TESTS_DIR/../tools/chip_run.sh}

if [ "${1:-}" = --mutants ]; then
    M=$(mktemp -d) || exit 1; trap 'rm -rf "$M"' EXIT; bad=0; n=0
    # the unmutated suite must pass first: a failing baseline can print "FAIL <id>:" for every mutant (no arguments, so no recursion)
    MODULE="$SRC_MODULE" bash "${BASH_SOURCE[0]}" > "$M/base.out" 2>&1 \
        || { echo "CHIP_RUN_MUTANTS: FAIL baseline self-test does not pass"; grep -E '^(FAIL|CHIP_RUN_SELFTEST)' "$M/base.out" | head -20 | sed 's/^/     | /'; exit 1; }
    for id in $(grep -o '# REFUSAL:[a-z_]*$' "$SRC_MODULE" | sed 's/.*://' | sort -u); do
        n=$((n + 1))
        sed "s/^.*# REFUSAL:$id\$/:/" "$SRC_MODULE" > "$M/chip_run.sh"
        cmp -s "$M/chip_run.sh" "$SRC_MODULE" && { echo "NOCHANGE $id"; bad=1; continue; }
        MODULE="$M/chip_run.sh" bash "${BASH_SOURCE[0]}" > "$M/out" 2>&1
        if grep -q "^FAIL $id:" "$M/out"; then echo "killed   $id"; else echo "SURVIVED $id"; bad=1; fi
    done
    # the jq array fix has no refusal line to delete: revert the "x" prefix idiom in a copy and need the named case to FAIL
    # (the copy must contain the plain old idiom, else the sed missed and the "kill" would be for the wrong reason)
    jqmut() { # NAME CASE SED-SCRIPT OLD-IDIOM-TEXT
        n=$((n + 1))
        sed "$3" "$SRC_MODULE" > "$M/chip_run.sh"
        cmp -s "$M/chip_run.sh" "$SRC_MODULE" && { echo "NOCHANGE $1"; bad=1; return; }
        grep -qF -- "$4" "$M/chip_run.sh" || { echo "NOCHANGE $1 (old idiom not in the copy)"; bad=1; return; }
        MODULE="$M/chip_run.sh" bash "${BASH_SOURCE[0]}" > "$M/out" 2>&1
        if grep -q "^FAIL $2:" "$M/out"; then echo "killed   $1"; else echo "SURVIVED $1"; bad=1; fi
    }
    jqmut jq_vlines_prefix dash_verdict '/--argjson vlines/{s/ | map(\.\[1:\])//;s|\${VLINES\[@\]/#/x}|${VLINES[@]}|;}' \
        "'\$ARGS.positional' --args \"\${VLINES[@]}\""
    jqmut jq_gate_args_prefix happy '/--argjson run_args/{s/ | map(\.\[1:\])//;s|\${GATE_ARGS\[@\]/#/x}|${GATE_ARGS[@]}|;}' \
        "'\$ARGS.positional' --args \"\${GATE_ARGS[@]}\""
    [ "$n" -gt 0 ] || bad=1
    [ "$bad" = 0 ] && { echo "CHIP_RUN_MUTANTS: PASS ($n mutants killed)"; exit 0; }
    echo "CHIP_RUN_MUTANTS: FAIL"; exit 1
fi

T=$(mktemp -d) || exit 1
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/tmp" "$T/bin"
PHYS=$T/physics; OM=$T/omega; FLAG=$T/flag; LOCK=$T/gpu.lock; OUT=$T/out.txt; BAD=0; RC=0
g() { local d=$1; shift; git -C "$d" -c user.email=t@t -c user.name=t -c commit.gpgsign=false "$@"; }

# fake binaries
cat > "$T/ok.c" <<'EOC'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    const char *m = getenv("FAKE_MODE"); if (!m) m = "pass";
    printf("fake chip args=%d\n", argc - 1);
    if (getenv("FAKE_CMD") && system(getenv("FAKE_CMD")) != 0) return 9;
    if (!strcmp(m, "silent")) return 0;
    if (!strcmp(m, "fail")) { printf("RESULT chip x ok=false\nVERDICT FAIL\n"); return 0; }
    if (!strcmp(m, "failpass")) { printf("VERDICT FAIL\nVERDICT PASS\n"); return 0; }
    if (!strcmp(m, "passrc")) { printf("VERDICT PASS\n"); return 3; }
    if (!strcmp(m, "rc1")) { printf("VERDICT FAIL\n"); return 1; }
    if (!strcmp(m, "rc2")) { printf("VERDICT PASS\n"); return 2; }
    if (!strcmp(m, "dash")) { printf("--- VERDICT PASS\n"); return 0; }
    printf("RESULT chip x ok=true\nVERDICT PASS\n");
    return 0;
}
EOC
printf '#include <math.h>\n#include <stdio.h>\nint main(void){ volatile float x = 2.0f; printf("VERDICT PASS %%f\\n", expf(x)); return 0; }\n' > "$T/libm.c"
printf 'extern int cuInit(unsigned) __attribute__((weak));\n#include <stdio.h>\nint main(void){ if (cuInit) cuInit(0); puts("VERDICT PASS"); return 0; }\n' > "$T/cuda.c"
gcc -O0 -o "$T/bin/ok" "$T/ok.c" && gcc -O0 -fno-builtin -o "$T/bin/libm" "$T/libm.c" -lm && gcc -O0 -o "$T/bin/cuda" "$T/cuda.c" \
    || { echo "FAIL setup: host gcc could not build the fake binaries"; echo "CHIP_RUN_SELFTEST: FAIL"; exit 1; }
printf '#!/bin/sh\necho VERDICT PASS\n' > "$T/bin/script"; chmod +x "$T/bin/script"

# fake physics (pinned) and fake omega (clean, with the module under test)
mkdir -p "$PHYS/nvrm" "$OM/tools" "$OM/src"
git init -q "$PHYS" && echo "/* fake */" > "$PHYS/nvrm/nvrm.c" && g "$PHYS" add -A && g "$PHYS" commit -q -m physics
PIN=$(git -C "$PHYS" rev-parse HEAD)
git init -q "$OM" && cp "$SRC_MODULE" "$OM/tools/chip_run.sh" && echo "$PIN" > "$OM/physics.lock" && echo "/* fake */" > "$OM/src/a.c" && echo "int main(void){ return undefined_thing; }" > "$OM/src/bad.c" \
    && printf "#include <stdio.h>\\nint main(void){ puts(\"VERDICT PASS\"); return 0; }\\n" > "$OM/src/okchip.c" \
    && g "$OM" add -A && g "$OM" commit -q -m omega
OM_HEAD=$(git -C "$OM" rev-parse HEAD)

mkman() { cat > "$1" <<EOM
GATE=selftest-gate
TEST_SOURCE=src/a.c
VERDICT_RE='^RESULT|^VERDICT'
PASS_LINE='^VERDICT PASS\$'
OWNER="chip_run selftest"
EVIDENCE_DIR=$T/ev-default
RAISE_QUIET=1
TAKE_GPU_LOCK=1
REFUSE_DIRTY_OMEGA=1
RUN_ARGS="--chip"
HOST_TIER_CMD='echo host-tier-ok'
RECEIPT_EXTRA_JQ='. + {extra_marker: "x"}'
${2:-}
EOM
}
mkman "$T/m.sh"; mkman "$T/m-hostfail.sh" "HOST_TIER_CMD=false"
mkman "$T/m-map.sh" "REFUSE_EXIT=1; REFUSE_VERDICT_LINE='VERDICT NOT_RUN'"
mkman "$T/m-off.sh" "RAISE_QUIET=0; TAKE_GPU_LOCK=0"
mkman "$T/m-all.sh" "REQUIRE_ALL_PASS=1; VERDICT_RE='^VERDICT'"
mkman "$T/m-pt.sh" "CHILD_EXIT_PASSTHROUGH=1"; mkman "$T/m-pt-bad.sh" "CHILD_EXIT_PASSTHROUGH=2"
mkman "$T/m-dash.sh" "VERDICT_RE='^--- VERDICT'; PASS_LINE='^--- VERDICT PASS\$'"
CRMAN=$T/m.sh; PD=(--physics-dir "$PHYS")

reset_world() {
    g "$OM" reset -q --hard "$OM_HEAD"; g "$OM" clean -fdq
    g "$PHYS" reset -q --hard "$PIN"; g "$PHYS" clean -fdq
    rm -f "$FLAG"; exec 8>&-
}
# cr ID [NAME=VALUE ...] -- [module args]: run the module from outside both trees
cr() {
    local id=$1; shift; local extra=() ev=(--evidence-dir "$T/ev-$id"); [ -z "${NOEVID:-}" ] || ev=()
    while [ "$1" != -- ]; do extra+=("$1"); shift; done; shift
    ( cd "$T" && env -u PHYSICS_DIR -u PHYSICS CHIPRUN_SELFTEST=1 CHIPRUN_PREBUILT_BIN="$T/bin/ok" CHIPRUN_QUIET_FLAG="$FLAG" \
        CHIPRUN_GPU_LOCK="$LOCK" CHIPRUN_EST_LOAD_CMD=true FAKE_MODE=pass TMPDIR="$T/tmp" "${extra[@]}" \
        bash "${RAWMOD:-$OM/tools/chip_run.sh}" "$CRMAN" "${ev[@]}" "$@" ) > "$OUT" 2>&1
    RC=$?
}
bad() { echo "FAIL $1: $2"; sed 's/^/     | /' "$OUT" | head -8; BAD=$((BAD + 1)); }
# expect ID RC REGEX: exit code and a matching output line
expect() {
    if [ "$RC" = "$2" ] && grep -Eq -- "$3" "$OUT"; then echo "ok   $1 (exit $RC)"; else bad "$1" "exit $RC, wanted $2 and /$3/"; fi
}
# assert ID DESC cmd...: extra condition for a case
assert() { local id=$1 d=$2; shift 2; if "$@"; then echo "ok   $id: $d"; else bad "$id" "$d"; fi; }
no_flag() { [ ! -e "$FLAG" ]; }
flag_is_other() { [ "$(cat "$FLAG" 2>/dev/null)" = other ]; }
receipt_verdict() { [ "$(jq -r .verdict "$T"/ev-"$1"/*.json 2>/dev/null)" = "$2" ]; }
# jqt FILTER FILE: jq -e with its stdout dropped, so assert still prints its own ok or FAIL line
jqt() { jq -e "$@" > /dev/null; }

reset_world; cr selftest_override CHIPRUN_SELFTEST=0 -- "${PD[@]}"
expect selftest_override 2 '^REFUSED: .*self-test override'
reset_world; cr no_physics -- --physics-dir "$T/nonexistent"
expect no_physics 2 '^REFUSED: no physics checkout'
reset_world; CRMAN=$T/m-map.sh cr refuse_map -- --physics-dir "$T/nonexistent"
expect refuse_map 1 '^VERDICT NOT_RUN$'; CRMAN=$T/m.sh
reset_world; g "$PHYS" commit -q --allow-empty -m moved; cr physics_pin -- "${PD[@]}"
expect physics_pin 2 '^REFUSED: physics checkout is at .*physics.lock pins'
reset_world; echo x > "$PHYS/junk"; cr physics_dirty -- "${PD[@]}"
expect physics_dirty 2 '^REFUSED: physics checkout .* is dirty'
reset_world; echo x > "$OM/junk"; cr omega_dirty -- "${PD[@]}"
expect omega_dirty 2 '^REFUSED: omega tree .* is dirty'
reset_world; cr evidence_inside -- "${PD[@]}" --evidence-dir "$OM/evid"
expect evidence_inside 2 '^REFUSED: evidence dir .* inside a candidate tree'
reset_world; cr evidence_inside_physics -- "${PD[@]}" --evidence-dir "$PHYS/evid"
expect evidence_inside_physics 2 '^REFUSED: evidence dir .* inside a candidate tree'
reset_world; echo other > "$FLAG"; cr quiet_flag -- "${PD[@]}"
expect quiet_flag 2 '^REFUSED: quiet flag is up: other'
assert quiet_flag "a flag that is not ours is left alone" flag_is_other
reset_world; cr est_load CHIPRUN_EST_LOAD_CMD='echo 4242' -- "${PD[@]}"
expect est_load 2 '^REFUSED: an est_load process is running'
reset_world; exec 8> "$LOCK"; flock -x 8; cr gpu_lock -- "${PD[@]}"; exec 8>&-
expect gpu_lock 2 '^REFUSED: GPU lock .* is held'
assert gpu_lock "the quiet flag we raised is dropped on refusal" no_flag
reset_world; CRMAN=$T/m-hostfail.sh cr host_tier -- "${PD[@]}"
expect host_tier 2 '^REFUSED: host tier failed'; CRMAN=$T/m.sh
reset_world; cr nm_unreadable CHIPRUN_PREBUILT_BIN="$T/bin/script" -- "${PD[@]}"
expect nm_unreadable 2 '^REFUSED: nm cannot read'
reset_world; cr libm CHIPRUN_PREBUILT_BIN="$T/bin/libm" -- "${PD[@]}"
expect libm 2 '^REFUSED: libm math symbols'
reset_world; cr cuda CHIPRUN_PREBUILT_BIN="$T/bin/cuda" -- "${PD[@]}"
expect cuda 2 '^REFUSED: CUDA symbols'
reset_world; cr rc_nonzero FAKE_MODE=passrc -- "${PD[@]}"
expect rc_nonzero 1 '^CHIP_RUN: FAIL binary exit status 3'
reset_world; cr no_verdict FAKE_MODE=silent -- "${PD[@]}"
expect no_verdict 1 '^CHIP_RUN: FAIL no verdict line'
reset_world; cr fail_verdict FAKE_MODE=fail -- "${PD[@]}"
expect fail_verdict 1 '^CHIP_RUN: FAIL last verdict line does not match PASS_LINE'
assert fail_verdict "a FAIL receipt is still written" receipt_verdict fail_verdict FAIL
reset_world; cr omega_dirty_after FAKE_CMD="touch $OM/dirt" -- "${PD[@]}"
expect omega_dirty_after 1 '^CHIP_RUN: FAIL omega tree dirty after run'
reset_world; cr physics_dirty_after FAKE_CMD="touch $PHYS/dirt" -- "${PD[@]}"
expect physics_dirty_after 1 '^CHIP_RUN: FAIL physics tree dirty after run'
reset_world; cr head_moved FAKE_CMD="git -C $OM -c user.email=t@t -c user.name=t -c commit.gpgsign=false commit -q --allow-empty -m x" -- "${PD[@]}"
expect head_moved 1 '^CHIP_RUN: FAIL a HEAD moved'

# raw ENV=VALUE... -- args: run the module with every seam variable unset (no CHIPRUN_SELFTEST)
raw() {
    local extra=(); while [ "$1" != -- ]; do extra+=("$1"); shift; done; shift
    ( cd "$T" && env -u PHYSICS_DIR -u PHYSICS -u CHIPRUN_SELFTEST -u CHIPRUN_PREBUILT_BIN -u CHIPRUN_QUIET_FLAG -u CHIPRUN_GPU_LOCK \
        -u CHIPRUN_EST_LOAD_CMD TMPDIR="$T/tmp" "${extra[@]}" bash "${RAWMOD:-$OM/tools/chip_run.sh}" "$@" ) > "$OUT" 2>&1
    RC=$?
}
# test seam: each of the four variables ALONE, without CHIPRUN_SELFTEST=1, is refused (manifest is m-off: no real flag or lock)
for sv in "seam_prebuilt CHIPRUN_PREBUILT_BIN $T/bin/ok" "seam_quiet CHIPRUN_QUIET_FLAG $FLAG" "seam_gpu CHIPRUN_GPU_LOCK $LOCK" "seam_est CHIPRUN_EST_LOAD_CMD true"; do
    read -r sid svar sval <<< "$sv"
    reset_world; raw "$svar=$sval" -- "$T/m-off.sh" --evidence-dir "$T/ev-$sid" "${PD[@]}"
    expect "$sid" 2 "^REFUSED: $svar is a self-test override"
done
# checked before the manifest is read: a missing manifest still gets the seam message
reset_world; raw CHIPRUN_PREBUILT_BIN="$T/bin/ok" -- "$T/nope.sh" --evidence-dir "$T/ev-seam_before" "${PD[@]}"
expect seam_before 2 '^REFUSED: CHIPRUN_PREBUILT_BIN is a self-test override'
# checked after the manifest is read: a manifest that sets the variable cannot slip it in
mkman "$T/m-seamset.sh" "RAISE_QUIET=0; TAKE_GPU_LOCK=0; CHIPRUN_PREBUILT_BIN=$T/bin/ok"
reset_world; raw -- "$T/m-seamset.sh" --evidence-dir "$T/ev-seam_after" "${PD[@]}"
expect seam_after 2 '^REFUSED: CHIPRUN_PREBUILT_BIN is a self-test override'
# even with CHIPRUN_SELFTEST=1 in the environment, a manifest cannot change a seam variable or set CHIPRUN_SELFTEST
mkman "$T/m-seamchg.sh" "RAISE_QUIET=0; TAKE_GPU_LOCK=0; CHIPRUN_PREBUILT_BIN=$T/bin/libm"
reset_world; raw CHIPRUN_SELFTEST=1 CHIPRUN_PREBUILT_BIN="$T/bin/ok" -- "$T/m-seamchg.sh" --evidence-dir "$T/ev-seam_changed" "${PD[@]}"
expect seam_changed 2 '^REFUSED: manifest changed CHIPRUN_PREBUILT_BIN'
mkman "$T/m-selfset.sh" "RAISE_QUIET=0; TAKE_GPU_LOCK=0; CHIPRUN_SELFTEST=1"
reset_world; raw -- "$T/m-selfset.sh" --evidence-dir "$T/ev-seam_selftest" "${PD[@]}"
expect seam_selftest 2 '^REFUSED: manifest changed CHIPRUN_SELFTEST'
# a manifest that assigns QUIET_FLAG, GPU_LOCK and EST_LOAD_CMD is undone after it is sourced. Each hostile value
# would refuse the run if it leaked (flag already up, lock held, est_load answers); the chip checks the flag is at the seam path.
mkman "$T/m-restore.sh" "QUIET_FLAG=$T/hostile-flag; GPU_LOCK=$T/hostile.lock; EST_LOAD_CMD='echo 4242'"
reset_world; rm -f "$LOCK" "$T/hostile.lock"; echo hostile > "$T/hostile-flag"; exec 8> "$T/hostile.lock"; flock -x 8
CRMAN=$T/m-restore.sh cr restore_vars FAKE_CMD="test -e $FLAG" -- "${PD[@]}"; exec 8>&-; CRMAN=$T/m.sh
expect restore_vars 0 '^CHIP_RUN: PASS'
assert restore_vars "the flag is dropped at the seam path after the run" no_flag
assert restore_vars "the manifest's flag path is left alone" test "$(cat "$T/hostile-flag" 2>/dev/null)" = hostile
assert restore_vars "the GPU lock was opened at the seam path" test -e "$LOCK"

# usage, arguments, manifest
reset_world; raw --
expect usage 2 '^REFUSED: usage'
reset_world; cr bad_arg -- "${PD[@]}" --bogus
expect bad_arg 2 '^REFUSED: unknown argument --bogus'
reset_world; cr arg_value -- --physics-dir
expect arg_value 2 '^REFUSED: --physics-dir needs a value'
reset_world; CRMAN=$T/nope.sh cr no_manifest -- "${PD[@]}"
expect no_manifest 2 '^REFUSED: no manifest at'
mkman "$T/m-loadfail.sh" "false"
reset_world; CRMAN=$T/m-loadfail.sh cr manifest_load -- "${PD[@]}"
expect manifest_load 2 '^REFUSED: manifest .* failed to load'
mkman "$T/m-novars.sh" 'OWNER=""'
reset_world; CRMAN=$T/m-novars.sh cr manifest_vars -- "${PD[@]}"
expect manifest_vars 2 '^REFUSED: manifest does not set OWNER'
mkman "$T/m-gate.sh" 'GATE="bad/gate"'
reset_world; CRMAN=$T/m-gate.sh cr gate_chars -- "${PD[@]}"
expect gate_chars 2 '^REFUSED: GATE .* characters'
mkman "$T/m-noevid.sh" 'EVIDENCE_DIR=""'
reset_world; NOEVID=1; CRMAN=$T/m-noevid.sh cr no_evidence -- "${PD[@]}"; unset NOEVID
expect no_evidence 2 '^REFUSED: no evidence dir'
# REFUSE_EXIT must be an integer 1..125; REQUIRE_ALL_PASS must be 0 or 1
for rx in "refuse_exit 0" "refuse_exit_text abc" "refuse_exit_big 126" "refuse_exit_empty ''"; do
    read -r rid rval <<< "$rx"; mkman "$T/m-$rid.sh" "REFUSE_EXIT=$rval"
    reset_world; CRMAN=$T/m-$rid.sh cr "$rid" -- "${PD[@]}"
    expect "$rid" 1 '^CHIP_RUN: BAD_MANIFEST REFUSE_EXIT='
done
mkman "$T/m-rx125.sh" "REFUSE_EXIT=125"
reset_world; CRMAN=$T/m-rx125.sh cr refuse_exit_125 -- --physics-dir "$T/nonexistent"
expect refuse_exit_125 125 '^REFUSED: no physics checkout'
mkman "$T/m-rap-bad.sh" "REQUIRE_ALL_PASS=yes"
reset_world; CRMAN=$T/m-rap-bad.sh cr require_all_value -- "${PD[@]}"
expect require_all_value 1 '^CHIP_RUN: BAD_MANIFEST REQUIRE_ALL_PASS='

# refusals that need a special world
mkdir -p "$T/nogit-om/tools" "$T/nogit/nvrm"; cp "$SRC_MODULE" "$T/nogit-om/tools/chip_run.sh"
reset_world; RAWMOD=$T/nogit-om/tools/chip_run.sh cr omega_head -- "${PD[@]}"
expect omega_head 2 '^REFUSED: cannot read omega HEAD'
mkman "$T/m-nolock.sh" "REFUSE_DIRTY_OMEGA=0"
reset_world; rm -f "$OM/physics.lock"; CRMAN=$T/m-nolock.sh cr physics_lock -- "${PD[@]}"
expect physics_lock 2 '^REFUSED: cannot read physics.lock'
reset_world; cr physics_head -- --physics-dir "$T/nogit"
expect physics_head 2 '^REFUSED: cannot read physics HEAD'
reset_world; cr mktemp TMPDIR="$T/nonexistent" -- "${PD[@]}"
expect mktemp 2 '^REFUSED: cannot create a run directory'
mkman "$T/m-race.sh" "HOST_TIER_CMD='echo other > $FLAG'"
reset_world; CRMAN=$T/m-race.sh cr flag_create -- "${PD[@]}"
expect flag_create 2 '^REFUSED: could not create the quiet flag'
assert flag_create "a flag created by someone else in the race is left alone" flag_is_other
reset_world; cr gpu_open CHIPRUN_GPU_LOCK="$T/nodir/x.lock" -- "${PD[@]}"
expect gpu_open 2 '^REFUSED: cannot open'
assert gpu_open "the quiet flag we raised is dropped on refusal" no_flag
reset_world; cr prebuilt_not_exec CHIPRUN_PREBUILT_BIN="$T/nonexistent" -- "${PD[@]}"
expect prebuilt_not_exec 2 '^REFUSED: CHIPRUN_PREBUILT_BIN .* is not executable'
mkman "$T/m-nosrc.sh" "TEST_SOURCE=src/nope.c"
reset_world; CRMAN=$T/m-nosrc.sh cr missing_source CHIPRUN_PREBUILT_BIN= -- "${PD[@]}"
expect missing_source 2 '^REFUSED: missing source src/nope.c'
mkman "$T/m-badsrc.sh" "TEST_SOURCE=src/bad.c"
reset_world; CRMAN=$T/m-badsrc.sh cr build_failed CHIPRUN_PREBUILT_BIN= -- "${PD[@]}"
expect build_failed 2 '^REFUSED: chip build failed'
mkman "$T/m-oksrc.sh" "TEST_SOURCE=src/okchip.c"
reset_world; CRMAN=$T/m-oksrc.sh cr build_ok CHIPRUN_PREBUILT_BIN= -- "${PD[@]}"
expect build_ok 0 '^CHIP_RUN: PASS'
# quiet flag: dropped only if still ours
reset_world; cr flag_not_ours FAKE_CMD="echo other > $FLAG" -- "${PD[@]}"
expect flag_not_ours 0 '^CHIP_RUN: PASS'
assert flag_not_ours "a flag someone else rewrote during the run is left alone" flag_is_other
# verdict logic: FAIL line then PASS line
reset_world; cr last_line_default FAKE_MODE=failpass -- "${PD[@]}"
expect last_line_default 0 '^CHIP_RUN: PASS'
reset_world; CRMAN=$T/m-all.sh cr fail_any_verdict FAKE_MODE=failpass -- "${PD[@]}"
expect fail_any_verdict 1 '^CHIP_RUN: FAIL a verdict line does not match PASS_LINE'
assert fail_any_verdict "a FAIL receipt is still written" receipt_verdict fail_any_verdict FAIL
reset_world; CRMAN=$T/m-all.sh cr all_pass_ok FAKE_MODE=pass -- "${PD[@]}"
expect all_pass_ok 0 '^CHIP_RUN: PASS'
# child exit passthrough: fake binary exiting 0, 1, 2 (exit 2 is the trap harness NOT_RUN code)
reset_world; CRMAN=$T/m-pt.sh cr child_exit_pass0 FAKE_MODE=pass -- "${PD[@]}"
expect child_exit_pass0 0 '^CHIP_RUN: PASS'
reset_world; CRMAN=$T/m-pt.sh cr child_exit_pass1 FAKE_MODE=rc1 -- "${PD[@]}"
expect child_exit_pass1 1 '^CHIP_RUN: FAIL'
reset_world; CRMAN=$T/m-pt.sh cr child_exit_passthrough FAKE_MODE=rc2 -- "${PD[@]}"
expect child_exit_passthrough 2 '^CHIP_RUN: FAIL binary exit status 2'
assert child_exit_passthrough "receipt still written with chip_exit_status 2" jq -e '.chip_exit_status == 2 and .verdict == "FAIL"' "$T"/ev-child_exit_passthrough/*.json
reset_world; CRMAN=$T/m-pt.sh cr child_exit_zero_fail FAKE_MODE=fail -- "${PD[@]}"
expect child_exit_zero_fail 1 '^CHIP_RUN: FAIL'
reset_world; CRMAN=$T/m.sh cr child_exit_default FAKE_MODE=rc2 -- "${PD[@]}"
expect child_exit_default 1 '^CHIP_RUN: FAIL binary exit status 2'
reset_world; CRMAN=$T/m-pt-bad.sh cr child_exit_value -- "${PD[@]}"
expect child_exit_value 1 '^CHIP_RUN: BAD_MANIFEST CHILD_EXIT_PASSTHROUGH='; CRMAN=$T/m.sh
# evidence reuse: a second run keeps one log blob; a tampered blob is caught
reset_world; cr reuse -- "${PD[@]}"; cr reuse -- "${PD[@]}"
expect reuse 0 '^CHIP_RUN: PASS'
assert reuse "one log blob for two identical logs" test "$(ls "$T"/ev-reuse/blobs/*.log 2>/dev/null | wc -l)" = 1
RB=$(ls "$T"/ev-reuse/blobs/*.log 2>/dev/null | head -1); chmod u+w "$RB" 2>/dev/null; echo tamper >> "$RB"
reset_world; cr reuse -- "${PD[@]}"
expect blob_tamper 1 '^CHIP_RUN: FAIL log blob does not match its digest'

# physics dir resolution order: flag, PHYSICS_DIR, PHYSICS
reset_world; cr order_env_dir PHYSICS="$T/nonexistent" PHYSICS_DIR="$PHYS" --
expect order_env_dir 0 '^CHIP_RUN: PASS'
reset_world; cr order_flag PHYSICS_DIR="$T/nonexistent" -- "${PD[@]}"
expect order_flag 0 '^CHIP_RUN: PASS'
reset_world; cr order_env_physics PHYSICS="$PHYS" --
expect order_env_physics 0 '^CHIP_RUN: PASS'
# optional steps off: flag up and lock held do not matter
reset_world; echo other > "$FLAG"; exec 8> "$LOCK"; flock -x 8; CRMAN=$T/m-off.sh cr optional_off -- "${PD[@]}"; exec 8>&-; CRMAN=$T/m.sh
expect optional_off 0 '^CHIP_RUN: PASS'
assert optional_off "a flag we never raised is untouched" flag_is_other

# seam: RECEIPT_EXTRA_JQ can read the whole HOST_TIER_CMD output as $hostlog (empty without one)
mkman "$T/m-hostlog.sh" "HOST_TIER_CMD='echo AAA; echo BBB'; RECEIPT_EXTRA_JQ='. + {hl: \$hostlog}'"
reset_world; CRMAN=$T/m-hostlog.sh cr hostlog -- "${PD[@]}"
expect hostlog 0 '^CHIP_RUN: PASS'
assert hostlog "receipt carries the whole host tier output" jq -e '.hl == "AAA\nBBB\n" and .host_tier == "BBB"' "$T"/ev-hostlog/*.json
mkman "$T/m-hostlog0.sh" "HOST_TIER_CMD=''; RECEIPT_EXTRA_JQ='. + {hl: \$hostlog}'"
reset_world; CRMAN=$T/m-hostlog0.sh cr hostlog_none -- "${PD[@]}"
expect hostlog_none 0 '^CHIP_RUN: PASS'
assert hostlog_none "no host tier gives an empty \$hostlog" jq -e '.hl == ""' "$T"/ev-hostlog_none/*.json

# seam: RECEIPT_EXTRA_JQ may replace a FAIL verdict with another upper-case word; PASS is never forged or removed
mkman "$T/m-vw.sh" "RECEIPT_EXTRA_JQ='.verdict = \"NOT_RUN\"'"
reset_world; CRMAN=$T/m-vw.sh cr verdict_word_ok FAKE_MODE=fail -- "${PD[@]}"
expect verdict_word_ok 1 '^CHIP_RUN: NOT_RUN last verdict line'
assert verdict_word_ok "receipt verdict is the rewritten word" receipt_verdict verdict_word_ok NOT_RUN
reset_world; CRMAN=$T/m-vw.sh cr verdict_pass_changed -- "${PD[@]}"
expect verdict_pass_changed 1 '^CHIP_RUN: FAIL RECEIPT_EXTRA_JQ changed a PASS verdict'
mkman "$T/m-vf.sh" "RECEIPT_EXTRA_JQ='.verdict = \"PASS\"'"
reset_world; CRMAN=$T/m-vf.sh cr verdict_forged FAKE_MODE=fail -- "${PD[@]}"
expect verdict_forged 1 '^CHIP_RUN: FAIL RECEIPT_EXTRA_JQ forged a PASS verdict'
mkman "$T/m-vb.sh" "RECEIPT_EXTRA_JQ='.verdict = \"not run\"'"
reset_world; CRMAN=$T/m-vb.sh cr verdict_word FAKE_MODE=fail -- "${PD[@]}"
expect verdict_word 1 '^CHIP_RUN: FAIL receipt verdict is not an upper-case word'

# seam: ECHO_RE limits the stdout echo of the chip log; the log blob stays complete
mkman "$T/m-echo.sh" "ECHO_RE='^VERDICT'"
reset_world; CRMAN=$T/m-echo.sh cr echo_re -- "${PD[@]}"
expect echo_re 0 '^VERDICT PASS$'
assert echo_re "default-echoed noise line is not shown with ECHO_RE" bash -c '! grep -q "^fake chip args" "$1"' _ "$OUT"
assert echo_re "the log blob still holds the whole log" grep -q '^fake chip args' "$T"/ev-echo_re/blobs/*.log
reset_world; cr echo_default -- "${PD[@]}"
assert echo_default "without ECHO_RE the whole chip log is echoed" grep -q '^fake chip args' "$OUT"

# seam: FINAL_LINE_STYLE=verdict prints "VERDICT <word>" last (old transc style), also for fatal errors
mkman "$T/m-fs.sh" "FINAL_LINE_STYLE=verdict; RECEIPT_EXTRA_JQ='.verdict = \"NOT_RUN\"'"
reset_world; CRMAN=$T/m-fs.sh cr final_style FAKE_MODE=fail -- "${PD[@]}"
expect final_style 1 '^VERDICT NOT_RUN$'
assert final_style "no CHIP_RUN: line in verdict style" bash -c '! grep -q "^CHIP_RUN:" "$1"' _ "$OUT"
assert final_style "the last line is the verdict line" test "$(tail -n 1 "$OUT")" = "VERDICT NOT_RUN"
mkman "$T/m-fs2.sh" "FINAL_LINE_STYLE=verdict"
reset_world; CRMAN=$T/m-fs2.sh cr final_style_pass -- "${PD[@]}"
expect final_style_pass 0 '^VERDICT PASS$'
mkman "$T/m-fs3.sh" "FINAL_LINE_STYLE=verdict; RECEIPT_EXTRA_JQ='.verdict = \"PASS\"'"
reset_world; CRMAN=$T/m-fs3.sh cr final_style_fatal FAKE_MODE=fail -- "${PD[@]}"
expect final_style_fatal 1 '^RECEIPT_EXTRA_JQ forged a PASS verdict$'
assert final_style_fatal "fatal ends with VERDICT FAIL" test "$(tail -n 1 "$OUT")" = "VERDICT FAIL"
mkman "$T/m-fs4.sh" "FINAL_LINE_STYLE=bogus"
reset_world; CRMAN=$T/m-fs4.sh cr final_style_value -- "${PD[@]}"
expect final_style_value 1 '^CHIP_RUN: BAD_MANIFEST FINAL_LINE_STYLE='

# happy path and receipt
reset_world; cr happy -- "${PD[@]}" -- --x 1
expect happy 0 '^VERDICT PASS$'
grep -q '^CHIP_RUN: PASS$' "$OUT" || bad happy "no final CHIP_RUN: PASS line"
assert happy "quiet flag dropped after the run" no_flag
R=$(ls "$T"/ev-happy/*.json 2>/dev/null | head -1)
assert happy "exactly one receipt" test "$(ls "$T"/ev-happy/*.json 2>/dev/null | wc -l)" = 1
assert happy "receipt mode is 0444" test "$(stat -c %a "$R" 2>/dev/null)" = 444
assert happy "receipt name is the sha256 of its content" test "$(sha256sum "$R" 2>/dev/null | cut -d' ' -f1).json" = "$(basename "$R")"
for f in gate owner omega_commit omega_tree_clean_before omega_tree_clean_after omega_commit_unchanged_after physics_commit physics_lock_pin \
         physics_tree_clean_before physics_tree_clean_after physics_commit_unchanged_after binary_sha256 chip_log_sha256 run_args host_tier \
         verdict_lines chip_exit_status started_utc finished_utc verdict reason extra_marker; do
    assert happy "receipt has field $f" jqt --arg f "$f" 'has($f)' "$R"
done
assert happy "receipt verdict PASS, run_args from the command line" jqt '.verdict == "PASS" and .run_args == ["--x","1"] and .host_tier == "host-tier-ok" and .omega_commit == "'"$OM_HEAD"'"' "$R"
LS=$(jq -r .chip_log_sha256 "$R" 2>/dev/null)
assert happy "log blob exists, mode 0444, named by its sha256" test "$(stat -c %a "$T/ev-happy/blobs/$LS.log" 2>/dev/null)" = 444 -a "$(sha256sum "$T/ev-happy/blobs/$LS.log" 2>/dev/null | cut -d' ' -f1)" = "$LS"

# a verdict line jq 1.7 would read as an option (it starts with "--") must reach the receipt intact;
# "- VERDICT PASS" (dash, space) would not be one, so it would not catch the bug
reset_world; CRMAN=$T/m-dash.sh cr dash_verdict FAKE_MODE=dash -- "${PD[@]}"; CRMAN=$T/m.sh
expect dash_verdict 0 '^CHIP_RUN: PASS'
RD=$(ls "$T"/ev-dash_verdict/*.json 2>/dev/null | head -1)
assert dash_verdict "receipt verdict_lines is the dash line, unchanged" jqt '.verdict_lines == ["--- VERDICT PASS"]' "$RD"
# no run args at all (RUN_ARGS empty, none on the command line): the binary gets none and run_args is []
mkman "$T/m-noargs.sh" 'RUN_ARGS=""'
reset_world; CRMAN=$T/m-noargs.sh cr empty_args -- "${PD[@]}"; CRMAN=$T/m.sh
expect empty_args 0 '^CHIP_RUN: PASS'
RE=$(ls "$T"/ev-empty_args/*.json 2>/dev/null | head -1)
assert empty_args "receipt run_args is the empty list" jqt '.run_args == []' "$RE"
assert empty_args "the binary was started with no arguments" grep -q '^fake chip args=0$' "$T"/ev-empty_args/blobs/*.log

if [ "$BAD" = 0 ]; then echo "CHIP_RUN_SELFTEST: PASS"; exit 0; fi
echo "CHIP_RUN_SELFTEST: FAIL ($BAD)"; exit 1
