#!/bin/sh
# R16-G2 inventory tool self-test. Usage: run.sh <path-to-r16_loop_inventory>
# Copies the fixture repos to a scratch directory, makes each a git repo, and
# checks: clean fixture PASSES; each planted defect FAILS for the right reason;
# JSON output is byte-stable. No network; seconds.
set -u
TOOL=${1:?usage: run.sh <r16_loop_inventory binary>}
case $TOOL in /*) ;; *) TOOL=$(pwd)/$TOOL ;; esac
HERE=$(cd "$(dirname "$0")" && pwd)
FAILS=0
SCR=$(mktemp -d "${TMPDIR:-/tmp}/r16inv.XXXXXX") || exit 2
trap 'rm -rf "$SCR"' EXIT INT TERM

setup() {
    rm -rf "$SCR/w" && mkdir -p "$SCR/w" && cp -R "$HERE/fixture/." "$SCR/w/" || exit 2
    for r in omega aien-sovereign-core aegis-runtime aienos physics; do
        g() { git -C "$SCR/w/$r" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false "$@"; }
        g init -q && g add -A && g commit -qm fixture || exit 2
    done
}
commit_all() {
    for r in omega aien-sovereign-core aegis-runtime aienos physics; do
        [ -d "$SCR/w/$r/.git" ] || continue
        git -C "$SCR/w/$r" add -A
        git -C "$SCR/w/$r" -c user.name=r16 -c user.email=r16@invalid -c commit.gpgsign=false commit -qm planted >/dev/null 2>&1
    done
}
run() {
    R16_REPO_OMEGA=$SCR/w/omega R16_REPO_SOVEREIGN_CORE=$SCR/w/aien-sovereign-core \
    R16_REPO_AEGIS_RUNTIME=$SCR/w/aegis-runtime R16_REPO_AIENOS=$SCR/w/aienos \
    R16_REPO_PHYSICS=$SCR/w/physics "$TOOL" --map "$SCR/w/map.md" --json "$SCR/out.json" 2>"$SCR/err.txt"
}
# expect <name> <exit> [stderr-pattern]
expect() {
    name=$1 want=$2 pat=${3:-}
    run; got=$?
    if [ "$got" -ne "$want" ]; then
        echo "FAIL $name: exit $got, want $want"; sed 's/^/    /' "$SCR/err.txt"; FAILS=$((FAILS + 1)); return
    fi
    if [ -n "$pat" ] && ! grep -q -- "$pat" "$SCR/err.txt"; then
        echo "FAIL $name: stderr lacks '$pat'"; sed 's/^/    /' "$SCR/err.txt"; FAILS=$((FAILS + 1)); return
    fi
    echo "ok   $name (exit $got)"
}

# 1. clean fixture: every site classified, class-A omega site under a legacy_oracle name
setup
expect clean-fixture-passes 0 "sites=11 unclassified=0 question=0 bad_class=0 a_reachable=0 stale=0 map_errors=0 skipped=0 -> PASS"
cp "$SCR/out.json" "$SCR/a.json"; run; if cmp -s "$SCR/a.json" "$SCR/out.json"; then echo "ok   json-byte-stable"; else echo "FAIL json-byte-stable"; FAILS=$((FAILS + 1)); fi

# 2. planted run_until_complete loop (spec R16-G2 case)
setup
printf 'int busy(void);\nvoid rx_run_until_complete(void);\nvoid drive(void) {\n    while (busy()) {\n        rx_run_until_complete();\n    }\n}\n' >> "$SCR/w/omega/src/world.c"; commit_all
expect planted-run-until-complete-fails 1 "UNCLASSIFIED omega src/world.c:.*named:run_until_complete"

# 3. planted unlabelled while (1)
setup
printf 'void spin(void) {\n    while (1) {\n    }\n}\n' >> "$SCR/w/physics/nvrm.c"; commit_all
expect planted-while-1-fails 1 "UNCLASSIFIED physics nvrm.c:.*while:unbounded"

# 4. planted Rust heartbeat loop (bounded, wait word in body)
setup
printf 'fn tick() {}\nfn heartbeat_loop() {\n    for _ in 0..10 {\n        tick();\n    }\n}\n' >> "$SCR/w/aegis-runtime/src/agent.rs"; commit_all
expect planted-rust-tick-loop-fails 1 "UNCLASSIFIED aegis-runtime src/agent.rs:.*for:body:tick"

# 5. a '?' row fails
setup
sed 's/| FX-10 \(.*\) | E | true |/| FX-10 \1 | ? | ? |/' "$HERE/fixture/map.md" > "$SCR/w/map.md"
expect question-row-fails 1 "QUESTION FX-10"

# 6. omega class A under a production name fails
setup
git -C "$SCR/w/omega" mv src/legacy_oracle_demo.c src/demo.c; commit_all
sed 's#src/legacy_oracle_demo.c#src/demo.c#' "$HERE/fixture/map.md" > "$SCR/w/map.md"
expect omega-class-a-reachable-fails 1 "A-REACHABLE FX-01"

# 7. a classified site that disappeared (non-A) is a stale row
setup
sed 's/while (\*m == 0) {/if (*m == 0) {/; s/^    }$//' "$HERE/fixture/physics/nvrm.c" > "$SCR/w/physics/nvrm.c"; commit_all
expect stale-row-fails 1 "STALE FX-11"

# 8. a missing repository is SKIPPED, and skipping is not a pass
setup
rm -rf "$SCR/w/physics"
expect missing-repo-skipped-fails 1 "SKIPPED physics"

# 9. manual row whose line is gone is stale
setup
sed 's/^int wait_marker(volatile int \*m) {$/int wait_marker2(volatile int *m) {/' "$HERE/fixture/physics/nvrm.c" > "$SCR/w/physics/nvrm.c"; commit_all
expect manual-row-gone-fails 1 "STALE FX-M1"

# 10. bad class letter fails
setup
sed 's/| FX-07 \(.*\) | F | false |/| FX-07 \1 | G | false |/' "$HERE/fixture/map.md" > "$SCR/w/map.md"
expect bad-class-fails 1 "BAD-CLASS FX-07"

if [ "$FAILS" -ne 0 ]; then echo "R16 inventory self-test: $FAILS FAILED"; exit 1; fi
echo "R16 inventory self-test: PASS"
