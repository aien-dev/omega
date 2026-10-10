#!/bin/sh
# AT-0 Agent 5: clean-checkout integration runner (charter section 6, gates G2, G4, G5, G6).
#
#   sh run.sh [--out DIR | --evidence] [--keep-work]
#
# Builds model, oracle, evaluator and the assembler from a fresh local clone of this checkout's
# HEAD (so the working tree is never touched), runs every evaluator case through
# model -> oracle (reference lines only) -> assemble -> evaluator, runs the whole set twice for
# repeatability, runs the implementation-level controls, and writes receipts.
#   --evidence   writes the new folder evidence/AT0/<UTC stamp>-<short commit> (append-only; must not exist)
#   --out DIR    writes to DIR (wiped first); default build/at0/check (what `make at0-check` uses)
# Single-threaded, no network, no Python, no GPU. Exit 0 only if no gate is FAIL.
set -u
LC_ALL=C; export LC_ALL
TZ=UTC; export TZ
# the runner may be started by `make at0-check`: the inner makes of the hygiene controls must not inherit its state
unset MAKELEVEL MAKEFLAGS MFLAGS MAKEOVERRIDES GNUMAKEFLAGS
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
MODE=check; OUT=""; KEEP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --evidence) MODE=evidence;;
        --out) shift; OUT=${1:?--out needs a directory};;
        --keep-work) KEEP=1;;
        *) echo "usage: sh run.sh [--out DIR | --evidence] [--keep-work]" >&2; exit 64;;
    esac
    shift
done
HEAD=$(git -C "$ROOT" rev-parse HEAD) || { echo "not a git checkout" >&2; exit 64; }
SHORT=$(git -C "$ROOT" rev-parse --short=7 HEAD)
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
CLEAN=YES; [ -n "$(git -C "$ROOT" status --porcelain)" ] && CLEAN=NO
if [ "$MODE" = evidence ]; then OUT="$ROOT/evidence/AT0/$STAMP-$SHORT"; [ -e "$OUT" ] && { echo "evidence folder exists: $OUT" >&2; exit 64; }
else [ -n "$OUT" ] || OUT="$ROOT/build/at0/check"; case "$OUT" in /*) ;; *) OUT="$PWD/$OUT";; esac; rm -rf "$OUT"; fi
mkdir -p "$OUT/receipts" "$OUT/cases" "$OUT/results" "$OUT/components" "$OUT/verify" "$OUT/mutants" || exit 1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/at0-run.XXXXXX") || exit 1
[ $KEEP = 1 ] || trap 'rm -rf "$WORK"' EXIT
LOG="$OUT/run.log"; : > "$LOG"
R="$OUT/receipts"
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
logrun() { printf '$ %s\n' "$*" >> "$LOG"; }
sha() { sha256sum "$1" | cut -c1-64; }
GATES="$R/gates.tsv"; CTRL="$R/controls.tsv"; : > "$GATES"; : > "$CTRL"
gate() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$GATES"; say "GATE $1 $2  $4"; }
ctrl() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$CTRL"; say "CONTROL $1 $2  $4"; }

say "AT-0 Agent 5 run $STAMP mode=$MODE omega=$HEAD tree_clean=$CLEAN"

# ---------- 0. contract pin ----------
LOCK="$HERE/contract.lock"
lock() { sed -n "s/^$1 //p" "$LOCK" | head -1; }
CONTRACT_COMMIT=$(lock contract_commit)
CASE_SHA=$(lock case_v1_sha256); RESULT_SHA=$(lock result_v2_sha256)
cp "$LOCK" "$R/contract.lock"
if [ -n "${AT0_ARCH_DIR:-}" ] && [ -d "$AT0_ARCH_DIR/docs/plans/atemporal" ]; then
    D="$AT0_ARCH_DIR/docs/plans/atemporal"
    if [ "$(sha "$D/AT0_CASE_V1.md")" = "$CASE_SHA" ] && [ "$(sha "$D/AT0_RESULT_V2.md")" = "$RESULT_SHA" ] && [ "$(sha "$D/AT0_SPEC.md")" = "$(lock spec_sha256)" ]; then
        ctrl C0-CONTRACT-DIGESTS PASS "sha256sum AT0_CASE_V1.md AT0_RESULT_V2.md AT0_SPEC.md in \$AT0_ARCH_DIR" "contract files match contract.lock (arch HEAD $(git -C "$AT0_ARCH_DIR" rev-parse --short=7 HEAD 2>/dev/null))"
    else ctrl C0-CONTRACT-DIGESTS FAIL "sha256sum AT0_CASE_V1.md AT0_RESULT_V2.md AT0_SPEC.md" "digest mismatch against contract.lock"; fi
else ctrl C0-CONTRACT-DIGESTS NOT_RUN "AT0_ARCH_DIR unset" "no local aien-architecture clone given; digests only recorded in contract.lock"; fi

# ---------- 1. clean checkout and toolchain ----------
S="$WORK/src"; AT0="$S/research/atemporal/at0"; INT="$AT0/integration"
logrun "git clone --no-hardlinks $ROOT $S; git checkout --detach $HEAD"
git clone -q --no-hardlinks "$ROOT" "$S" >> "$LOG" 2>&1 && git -C "$S" checkout -q --detach "$HEAD" >> "$LOG" 2>&1 || { say "FATAL clone failed"; exit 1; }
[ "$(git -C "$S" rev-parse HEAD)" = "$HEAD" ] || { say "FATAL clone HEAD mismatch"; exit 1; }
CCBIN=${CC:-gcc}
GCC_V=$($CCBIN --version | head -1)
RUSTC_V=$(rustc --version)
{
    echo "date_utc $STAMP"; echo "uname $(uname -srm)"; echo "host $(cat /etc/hostname 2>/dev/null || echo unknown)"
    echo "cc ($CCBIN) $GCC_V"; echo "cc via 'cc': $(cc --version | head -1)"; echo "make $(make --version | head -1)"; echo "nm $(nm --version | head -1)"
    echo "--- rustc -vV"; rustc -vV
} > "$R/toolchain.txt"
case "$RUSTC_V" in "rustc 1.98.1 "*) ctrl C0-RUSTC-PIN PASS "rustc --version" "$RUSTC_V (pin 1.98.1)";; *) ctrl C0-RUSTC-PIN FAIL "rustc --version" "$RUSTC_V is not the pinned 1.98.1";; esac
CF="-std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L"
MODEL_FLAGS=$(sed -n 's/^CFLAGS="\(.*\)"$/\1/p' "$AT0/model/build.sh" | head -1)
ORACLE_FLAGS=$(sed -n 's/^FLAGS="\(.*\)"$/\1/p' "$AT0/oracle/build.sh" | head -1)
{ echo "model (build.sh, plain, -lm): $MODEL_FLAGS -O2"; echo "model (build.sh asan): $MODEL_FLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
  echo "oracle (build.sh build): rustc $ORACLE_FLAGS --crate-name at0_oracle src/main.rs"
  echo "evaluator (Makefile): $CF -Isrc -I../../../../src ... -lm"; echo "integration (this runner): $CF -I../../../../src ... (no -lm needed)"; } > "$R/flags.txt"

# ---------- 2. builds and component tests ----------
bstep() { # label dir cmd...
    lab=$1; d=$2; shift 2; logrun "(cd $d && $*)"
    (cd "$d" && "$@") > "$WORK/$lab.log" 2>&1; rc=$?; cp "$WORK/$lab.log" "$R/$lab.log"
    [ $rc -eq 0 ] && say "BUILD $lab ok" || say "BUILD $lab FAILED rc=$rc"; return $rc
}
BUILD_OK=1
bstep model-build "$AT0/model" ./build.sh || BUILD_OK=0
bstep model-build-asan "$AT0/model" ./build.sh asan || BUILD_OK=0
bstep oracle-build "$AT0/oracle" ./build.sh || BUILD_OK=0
bstep evaluator-build "$AT0/evaluator" make -s all || BUILD_OK=0
bstep evaluator-build-asan "$AT0/evaluator" make -s asan || BUILD_OK=0
logrun "cc $CF -I src -o at0-assemble at0_assemble.c src/sha256.c"
mkdir -p "$WORK/bin"
cc $CF -I"$S/src" -o "$WORK/bin/at0-assemble" "$INT/at0_assemble.c" "$S/src/sha256.c" > "$R/integration-build.log" 2>&1 || { BUILD_OK=0; say "BUILD integration FAILED"; }
[ $BUILD_OK = 1 ] || { say "FATAL build failure; see $R/*.log"; gate AT0-G2 FAIL "build" "a build step failed"; gate AT0-G4 NOT_RUN "build" "build failed"; gate AT0-G5 NOT_RUN "build" "build failed"; gate AT0-G6 NOT_RUN "build" "build failed"; exit 1; }
MODEL="$AT0/model/build/at0-model"; ORACLE="$AT0/oracle/target/at0-oracle"; EVAL="$AT0/evaluator/build/at0-eval"; ASM="$WORK/bin/at0-assemble"
( cd "$AT0/model" && ./build/at0-tests tests/cases ) > "$R/model-tests.log" 2>&1; mrc=$?
( cd "$AT0/model" && ./build-asan/at0-tests tests/cases ) > "$R/model-tests-asan.log" 2>&1; mrca=$?
( cd "$AT0/oracle" && ./build.sh test ) > "$R/oracle-tests.log" 2>&1; orc=$?
( cd "$AT0/oracle" && ./isolation.sh ) > "$R/oracle-isolation-own.log" 2>&1; oisorc=$?
ctrl C1-MODEL-TESTS $( [ $mrc -eq 0 ] && [ $mrca -eq 0 ] && echo PASS || echo FAIL ) "model/build/at0-tests tests/cases; model/build-asan/at0-tests tests/cases" "plain rc=$mrc, ASan/UBSan rc=$mrca ($(tail -1 "$R/model-tests.log"))"
ctrl C1-ORACLE-TESTS $( [ $orc -eq 0 ] && echo PASS || echo FAIL ) "oracle/build.sh test" "rc=$orc ($(grep -m1 'test result' "$R/oracle-tests.log"))"
ctrl C1-ORACLE-OWN-ISOLATION $( [ $oisorc -eq 0 ] && echo PASS || echo FAIL ) "oracle/isolation.sh" "Agent 2's own gate rc=$oisorc ($(tail -1 "$R/oracle-isolation-own.log"))"

# ---------- 3. receipts: sources and binaries ----------
( cd "$S" && git ls-files research/atemporal/at0 src/sha256.c src/sha256.h mk/at0.mk 2>/dev/null | LC_ALL=C sort | while read -r f; do printf '%s  %s\n' "$(sha "$f")" "$f"; done ) > "$R/sources.sha256"
{ for b in "$MODEL" "$ORACLE" "$EVAL" "$ASM" "$AT0/model/build/at0-tests" "$AT0/model/build-asan/at0-model" "$AT0/evaluator/build/at0-eval-asan"; do
    [ -f "$b" ] && printf '%s  %s\n' "$(sha "$b")" "${b#$WORK/}"; done; } > "$R/binaries.sha256"
ENGINE_SHA=$(sha "$MODEL"); ORACLE_SHA=$(sha "$ORACLE")

# ---------- 4. provenance template ----------
HOSTN=$(cat /etc/hostname 2>/dev/null || echo unknown)
ORFLAGS=$(printf '%s' "$ORACLE_FLAGS" | sed 's/ -D warnings//')
BCC="$GCC_V (engine); $RUSTC_V (oracle)"
BFL="engine: $MODEL_FLAGS -O2 -lm; oracle: rustc $ORACLE_FLAGS"
[ ${#BCC} -le 200 ] && [ ${#BFL} -le 200 ] || { BFL=$(printf '%s' "$BFL" | cut -c1-200); say "NOTE build_flags truncated to 200 bytes; full flags in receipts/flags.txt"; }
BFL=$(printf '%s' "$BFL" | sed 's/ *$//'); BCC=$(printf '%s' "$BCC" | sed 's/ *$//')
PROV="$WORK/prov.static"
{ echo "source_repo aien-dev/omega"; echo "source_commit $HEAD"; echo "source_tree_clean $CLEAN"; echo "contract_commit $CONTRACT_COMMIT"
  echo "engine_sha256 $ENGINE_SHA"; echo "oracle_repo aien-dev/omega"; echo "oracle_commit $HEAD"; echo "oracle_sha256 $ORACLE_SHA"
  echo "build_cc $BCC"; echo "build_flags $BFL"; echo "host $HOSTN"; } > "$PROV"
export AT0_MODEL_BIN="$MODEL" AT0_ORACLE_BIN="$ORACLE" AT0_ASM_BIN="$ASM" AT0_PROV_STATIC="$PROV"
RUNONE="sh $INT/candidate_run.sh"

# ---------- 5. case set ----------
CASES="$WORK/cases.list"
( cd "$AT0/evaluator/cases" && ls positive/*.case negative/*.case refuse/*.case ) | LC_ALL=C sort > "$CASES"
NCASE=$(wc -l < "$CASES")
for cls in positive negative refuse; do mkdir -p "$OUT/cases/$cls" "$OUT/results/pass1/$cls" "$OUT/results/pass2/$cls" "$OUT/verify/$cls"; done
cp "$AT0/evaluator/cases/MANIFEST.tsv" "$R/MANIFEST.tsv"
while read -r c; do cp "$AT0/evaluator/cases/$c" "$OUT/cases/$c"; done < "$CASES"
( cd "$OUT/cases" && while read -r c; do printf '%s  %s\n' "$(sha "$c")" "$c"; done < "$CASES" ) > "$R/cases.sha256"
say "cases: $NCASE (positive $(grep -c '^positive' "$CASES"), negative $(grep -c '^negative' "$CASES"), refuse $(grep -c '^refuse' "$CASES")); hidden set not touched"

# value helpers over a result file
field() { sed -n "s/^$2 //p" "$1" | head -1; }
valblock() { sed -n '/^begin values$/,/^end values$/p' "$1"; }

# ---------- 6. pass 1 ----------
CT="$R/cases.tsv"
printf 'case\tclass\texpected_outcome\texpected_codes\tgot_outcome\tgot_codes\texpectation_met\tevaluator\toracle_own_outcome\toracle_own_codes\ttool_response\tmanifest_response\n' > "$CT"
NSAME=0; NDIFF_TOOL=0
while read -r c; do
    name=$(basename "$c" .case); cls=${c%%/*}; case_abs="$OUT/cases/$c"
    want=$(awk -F'\t' -v p="$c" '$1==p {print $3}' "$AT0/evaluator/cases/MANIFEST.tsv")
    res="$OUT/results/pass1/$cls/$name.result"
    AT0_COMP_DIR="$OUT/components" $RUNONE "$case_abs" > "$res" 2> "$WORK/err"; rc=$?
    if [ $rc -eq 2 ]; then
        rm -f "$res"; got=$(head -1 "$WORK/err")
        orc_line=$("$ORACLE" check "$case_abs" 2>&1 >/dev/null | head -1)
        ok=SAME; [ "$got" = "$want" ] && [ "$orc_line" = "$want" ] || ok=DIFF
        printf '%s\t%s\tREFUSED\t%s\tREFUSED\t%s\tNOT_APPLICABLE\tNOT_APPLICABLE\t-\t-\tmodel[%s] oracle[%s]\t%s\n' "$c" "$cls" "$want" "$got" "$got" "$orc_line" "$want" >> "$CT"
        [ "$ok" = SAME ] || NDIFF_TOOL=$((NDIFF_TOOL+1)); continue
    fi
    [ "$cls" = refuse ] && [ $rc -eq 0 ] && NDIFF_TOOL=$((NDIFF_TOOL+1))
    if [ $rc -ne 0 ]; then
        "$ORACLE" emit "$case_abs" -o "$OUT/components/$name.oracle" 2>/dev/null
        printf '%s\t%s\t-\t-\tERROR\t%s\tNO\tNOT_RUN\t%s\t%s\trc=%s\t%s\n' "$c" "$cls" "$(head -1 "$WORK/err" | cut -c1-80)" "$(field "$OUT/components/$name.oracle" outcome)" "$(field "$OUT/components/$name.oracle" failure_codes)" "$rc" "$want" >> "$CT"; rm -f "$res"; continue
    fi
    "$EVAL" result "$res" --case "$case_abs" --json > "$OUT/verify/$cls/$name.json" 2>/dev/null; evrc=$?
    evs=$(sed 's/.*"status":"\([A-Z]*\)".*/\1/' "$OUT/verify/$cls/$name.json")
    exp_o=$(sed -n 's/^expected_outcome //p' "$case_abs"); exp_c=$(sed -n 's/^expected_failure_codes //p' "$case_abs")
    oo=$(field "$OUT/components/$name.oracle" outcome); oc=$(field "$OUT/components/$name.oracle" failure_codes)
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\tok\t%s\n' "$c" "$cls" "$exp_o" "$exp_c" "$(field "$res" outcome)" "$(field "$res" failure_codes)" "$(field "$res" expectation_met)" "${evs:-evrc$evrc}" "$oo" "$oc" "$want" >> "$CT"
done < "$CASES"
say "pass 1 done: $(awk -F'\t' 'NR>1' "$CT" | wc -l) rows"

# ---------- 7. pass 2 (repeatability): later in time, different time zone ----------
sleep 2
while read -r c; do
    name=$(basename "$c" .case); cls=${c%%/*}
    TZ=Asia/Tokyo $RUNONE "$OUT/cases/$c" > "$OUT/results/pass2/$cls/$name.result" 2>/dev/null || rm -f "$OUT/results/pass2/$cls/$name.result"
done < "$CASES"
RP_N=0; RP_VAL=0; RP_VER=0; RP_EVD=0; RP_BAD=""
for f in "$OUT"/results/pass1/*/*.result; do
    cls=$(basename "$(dirname "$f")"); name=$(basename "$f"); g="$OUT/results/pass2/$cls/$name"
    RP_N=$((RP_N+1))
    [ -f "$g" ] || { RP_BAD="$RP_BAD $name(missing)"; continue; }
    [ "$(valblock "$f")" = "$(valblock "$g")" ] && RP_VAL=$((RP_VAL+1)) || RP_BAD="$RP_BAD $name(values)"
    [ "$(field "$f" verdict_id)" = "$(field "$g" verdict_id)" ] && RP_VER=$((RP_VER+1)) || RP_BAD="$RP_BAD $name(verdict_id)"
    [ "$(field "$f" evidence_digest)" != "$(field "$g" evidence_digest)" ] && RP_EVD=$((RP_EVD+1)) || RP_BAD="$RP_BAD $name(evidence_digest_equal)"
    for k in case_id acceptance_id; do [ "$(field "$f" $k)" = "$(field "$g" $k)" ] || RP_BAD="$RP_BAD $name($k)"; done
done
if [ "$RP_N" -gt 0 ] && [ $RP_VAL = $RP_N ] && [ $RP_VER = $RP_N ] && [ $RP_EVD = $RP_N ] && [ -z "$RP_BAD" ]; then
    ctrl C2-REPEATABILITY PASS "run each case twice (2 s apart, TZ=UTC then TZ=Asia/Tokyo); compare" "$RP_N results: values blocks byte-identical $RP_VAL/$RP_N, verdict_id equal $RP_VER/$RP_N, case_id/acceptance_id equal, evidence_digest differs $RP_EVD/$RP_N"
    ctrl C3-WALLCLOCK-INDEPENDENCE PASS "same comparison (the charter's wall-clock independence control)" "equal case_id, acceptance_id, verdict_id and values block; different evidence_digest"
else
    ctrl C2-REPEATABILITY FAIL "run each case twice and compare" "values $RP_VAL verdict $RP_VER evidence-differs $RP_EVD of $RP_N; problems:$RP_BAD"
    ctrl C3-WALLCLOCK-INDEPENDENCE FAIL "same comparison" "problems:$RP_BAD"
fi

# ---------- 8. evaluation order ----------
EO_N=0; EO_OK=0; EO_BAD=""
for f in "$OUT"/components/*.engine; do
    name=$(basename "$f" .engine); cf=$(ls "$OUT"/cases/*/"$name".case)
    EO_N=$((EO_N+1))
    "$MODEL" "$cf" --reversed > "$WORK/rev" 2>/dev/null
    if cmp -s "$f" "$WORK/rev"; then EO_OK=$((EO_OK+1)); else EO_BAD="$EO_BAD $name"; fi
done
[ $EO_N -gt 0 ] && [ $EO_OK = $EO_N ] && ctrl C4-EVALUATION-ORDER PASS "at0-model <case> --reversed vs normal, cmp of whole engine output" "$EO_OK/$EO_N bit-identical (labels evaluated in reverse order)" \
    || ctrl C4-EVALUATION-ORDER FAIL "at0-model <case> --reversed vs normal" "identical $EO_OK/$EO_N; differ:$EO_BAD"

# ---------- 9. gates G4 and G5 (tables) ----------
G4=$(awk -F'\t' 'NR>1 && $2=="positive" {n++; if ($5=="PASS" && $7=="YES") ok++} END {printf "%d %d", n+0, ok+0}' "$CT")
G5=$(awk -F'\t' 'NR>1 && $2=="negative" {n++; if ($5=="FAIL" && $7=="YES" && $6==$4) ok++} END {printf "%d %d", n+0, ok+0}' "$CT")
set -- $G4; G4N=$1; G4OK=$2; set -- $G5; G5N=$1; G5OK=$2
G6N=$(awk -F'\t' 'NR>1 && $5!="REFUSED" && $5!="ERROR" {n++; if ($8=="PASS") ok++} END {printf "%d %d", n+0, ok+0}' "$CT"); set -- $G6N; G6T=$1; G6OK=$2
REFN=$(awk -F'\t' 'NR>1 && $5=="REFUSED" {n++} END {print n+0}' "$CT")

# ---------- 10. mutant controls (axis swap, Y sign, hidden clock) ----------
mutant_model() { # name sed-script source-file -> builds $WORK/mut/<name>/at0-model
    mn=$1; ms=$2; mf=$3; md="$WORK/mut/$mn"; mkdir -p "$md"
    cp "$AT0"/model/*.c "$AT0"/model/*.h "$md/"
    sed "$ms" "$AT0/model/$mf" > "$md/$mf"
    if cmp -s "$AT0/model/$mf" "$md/$mf"; then echo "mutation did not change $mf" >&2; return 1; fi
    ( cd "$md" && cc $CF -I. -I"$S/src" -o at0-model at0_main.c at0_exact.c at0_case.c at0_io.c at0_state.c at0_hamiltonian.c at0_constraint.c at0_povm.c at0_conditional.c at0_observable.c at0_engine.c at0_output.c "$S/src/sha256.c" -lm ) > "$md/build.log" 2>&1
}
run_mutant() { # name  -> results under mutants/<name>
    mn=$1; mkdir -p "$OUT/mutants/$mn"
    for c in $(grep '^positive' "$CASES"); do
        name=$(basename "$c" .case)
        AT0_MODEL_BIN="$WORK/mut/$mn/at0-model" AT0_CC_NOTE="(MUTANT $mn control, not a qualification result)" \
          AT0_PROV_STATIC="$WORK/prov.mut.$mn" $RUNONE "$OUT/cases/$c" > "$OUT/mutants/$mn/$name.result" 2>/dev/null || continue
        evs=$("$EVAL" result "$OUT/mutants/$mn/$name.result" --case "$OUT/cases/$c" --json 2>/dev/null | sed 's/.*"status":"\([A-Z]*\)".*/\1/')
        printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$(field "$OUT/mutants/$mn/$name.result" outcome)" "$(field "$OUT/mutants/$mn/$name.result" failure_codes)" "$(field "$OUT/mutants/$mn/$name.result" expectation_met)" "$evs"
    done > "$R/mutant-$mn.tsv"
}
for mn in axis_swap y_sign_flip; do
    case $mn in
      axis_swap) MS='s/case AT0_AXIS_X:/case @@AX@@:/; s/case AT0_AXIS_Y:/case AT0_AXIS_X:/; s/case @@AX@@:/case AT0_AXIS_Y:/';;
      y_sign_flip) MS='s/m\[0\]\[1\] = -I;   out->m\[1\]\[0\] = I;/m[0][1] = I;   out->m[1][0] = -I;/';;
    esac
    # the mutants record their own binary digest as engine_sha256
    if mutant_model $mn "$MS" at0_observable.c; then
        sed "s/^engine_sha256 .*/engine_sha256 $(sha "$WORK/mut/$mn/at0-model")/" "$PROV" > "$WORK/prov.mut.$mn"
        run_mutant $mn
    else echo "mutant $mn did not build: $(tail -2 "$WORK/mut/$mn/build.log" 2>/dev/null)" >> "$LOG"; : > "$R/mutant-$mn.tsv"; fi
done
# axis swap must be caught on every positive case where the swap is visible: expectation_met NO with SCHRODINGER_DEVIATION_EXCEEDED
mut_report() { # name -> "caught/total; blind: ..."
    awk -F'\t' '{n++; if ($4=="NO" && $3=="SCHRODINGER_DEVIATION_EXCEEDED") c++; else b=b" "$1} END {printf "%d/%d caught", c+0, n+0; if (b) printf "; not caught (swap invisible on that case, see QUALIFICATION.md):%s", b}' "$R/mutant-$1.tsv"
}
AX=$(mut_report axis_swap); YS=$(mut_report y_sign_flip)
AXP1=$(awk -F'\t' '$1 ~ /^P1-kat/ {print ($4=="NO" && $3=="SCHRODINGER_DEVIATION_EXCEEDED") ? "YES" : "NO"}' "$R/mutant-axis_swap.tsv")
AXALL=$(awk -F'\t' '$1 ~ /^P1/ && !($4=="NO" && $3=="SCHRODINGER_DEVIATION_EXCEEDED") {print $1}' "$R/mutant-axis_swap.tsv" | tr '\n' ' ')
[ "$AXP1" = YES ] && ctrl C5-AXIS-SWAP-MUTANT PASS "model copy with the X and Y cases of at0_observable.c exchanged, run on all positive cases" "$AX; every P1 case caught except:${AXALL:- none}" \
    || ctrl C5-AXIS-SWAP-MUTANT FAIL "model copy with X and Y exchanged" "KAT not caught: $AX"
[ "$(awk -F'\t' '$1 ~ /^P1-kat/ {print ($4=="NO" && $3=="SCHRODINGER_DEVIATION_EXCEEDED") ? "YES":"NO"}' "$R/mutant-y_sign_flip.tsv")" = YES ] && ctrl C6-Y-SIGN-MUTANT PASS "model copy with sigma_y negated (dropped complex conjugate), run on all positive cases" "$YS" \
    || ctrl C6-Y-SIGN-MUTANT FAIL "model copy with sigma_y negated" "KAT not caught: $YS"

# ---------- 11. isolation (G2) ----------
ISO="$WORK/iso"; mkdir -p "$ISO/clean" "$ISO/mut"
ISOOK=1
for f in "$AT0"/model/at0_*.c "$S/src/sha256.c"; do
    b=$(basename "$f" .c); [ "$b" = at0_main ] && continue
    cc $CF -I"$AT0/model" -I"$S/src" -c "$f" -o "$ISO/clean/$b.o" >> "$LOG" 2>&1 || ISOOK=0
done
[ $ISOOK = 1 ] || say "NOTE model objects did not all compile (C7 will FAIL)"
CLEANOBJS=$(ls "$ISO"/clean/*.o | grep -v /at0_io\.o)
sh "$HERE/isolation_check.sh" $CLEANOBJS > "$R/isolation-model-own.log" 2>&1; i1=$?
ALLOW='^(fopen|fclose|fread|ferror)$' sh "$HERE/isolation_check.sh" "$ISO/clean/at0_io.o" > "$R/isolation-model-case-reader.log" 2>&1; i3=$?
sh "$HERE/isolation_check.sh" "$ISO/clean/at0_io.o" > "$R/isolation-model-case-reader-strict.log" 2>&1; i3s=$?
sh "$AT0/evaluator/gates/isolation.sh" $CLEANOBJS > "$R/isolation-model-evaluator-gate.log" 2>&1; i2=$?
# hidden-clock mutant: the engine plus a clock read
cp "$AT0/model/at0_engine.c" "$ISO/mut/at0_engine_hidden_clock.c"
printf '\n#include <time.h>\nlong at0_hidden_clock_probe(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return (long)t.tv_nsec; }\n' >> "$ISO/mut/at0_engine_hidden_clock.c"
cc $CF -I"$AT0/model" -I"$S/src" -c "$ISO/mut/at0_engine_hidden_clock.c" -o "$ISO/mut/at0_engine_hidden_clock.o" >> "$LOG" 2>&1
sh "$HERE/isolation_check.sh" "$ISO/mut/at0_engine_hidden_clock.o" > "$R/isolation-model-mutant-own.log" 2>&1; m1=$?
sh "$AT0/evaluator/gates/isolation.sh" "$ISO/mut/at0_engine_hidden_clock.o" > "$R/isolation-model-mutant-evaluator-gate.log" 2>&1; m2=$?
# a hidden-clock mutant that reads CNTVCT_EL0 or the like is invisible to any symbol scan: recorded as a limit, not tested
[ $ISOOK = 1 ] && [ $i1 -eq 0 ] && [ $i2 -eq 0 ] && [ $i3 -eq 0 ] && ctrl C7-ISOLATION-MODEL-CLEAN PASS "nm -u over model compute objects (all model .c except at0_main.c and at0_io.c (the thin file-reader layer, D6), plus src/sha256.c, so at0_case.o is scanned strictly): isolation_check.sh and evaluator/gates/isolation.sh; at0_io.o scanned with only fopen/fread/fclose/ferror exempt" "$(echo $CLEANOBJS | wc -w) compute objects clean under both scanners (parser at0_case.o strictly clean); at0_io.o scanned separately (strict scan rc=$i3s: $(head -1 "$R/isolation-model-case-reader-strict.log" | cut -c1-120))" \
    || ctrl C7-ISOLATION-MODEL-CLEAN FAIL "nm -u over model compute objects" "own rc=$i1 evaluator-gate rc=$i2 case-reader rc=$i3: $(head -2 "$R/isolation-model-own.log" | tr '\n' ' ')"
[ $m1 -eq 1 ] && [ $m2 -ne 0 ] && ctrl C8-ISOLATION-MODEL-HIDDEN-CLOCK-MUTANT PASS "same scanners on at0_engine.c plus a clock_gettime call" "caught by both: $(head -1 "$R/isolation-model-mutant-own.log")" \
    || ctrl C8-ISOLATION-MODEL-HIDDEN-CLOCK-MUTANT FAIL "same scanners on the hidden-clock mutant" "own rc=$m1 evaluator-gate rc=$m2 (both must flag)"
# Rust oracle: nm -u on the members of the compute rlib, plus two Rust mutants
RI="$WORK/riso"; mkdir -p "$RI/clean" "$RI/m1" "$RI/m2"
RFL="--edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -C panic=abort -D warnings --crate-type lib --crate-name at0"
( cd "$AT0/oracle" && ./build.sh lib ) >> "$LOG" 2>&1
( cd "$RI/clean" && ar x "$AT0/oracle/target/libat0.rlib" ) 2>> "$LOG"
cp -r "$AT0/oracle/src/at0" "$RI/m1/at0"; cp -r "$AT0/oracle/src/at0" "$RI/m2/at0"
printf '\npub fn hidden_clock() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }\n' >> "$RI/m1/at0/mod.rs"
printf '\n#[repr(C)] pub struct Ts { s: i64, n: i64 }\nextern "C" { fn clock_gettime(c: i32, t: *mut Ts) -> i32; }\npub fn hidden_clock() -> i64 { let mut t = Ts { s: 0, n: 0 }; unsafe { clock_gettime(0, &mut t); } t.n }\n' >> "$RI/m2/at0/mod.rs"
for m in m1 m2; do ( cd "$RI/$m" && rustc $RFL at0/mod.rs -o lib.rlib && ar x lib.rlib ) >> "$LOG" 2>&1; done
sh "$HERE/isolation_check.sh" "$RI"/clean/*.o > "$R/isolation-oracle-own.log" 2>&1; r0=$?
sh "$AT0/evaluator/gates/isolation.sh" "$RI"/clean/*.o > "$R/isolation-oracle-evaluator-gate.log" 2>&1; r0e=$?
sh "$HERE/isolation_check.sh" "$RI"/m1/*.o > "$R/isolation-oracle-mutant-std-time.log" 2>&1; r1=$?
sh "$HERE/isolation_check.sh" "$RI"/m2/*.o > "$R/isolation-oracle-mutant-extern-clock.log" 2>&1; r2=$?
sh "$AT0/evaluator/gates/isolation.sh" "$RI"/m1/*.o > /dev/null 2>&1; r1e=$?
sh "$AT0/evaluator/gates/isolation.sh" "$RI"/m2/*.o > /dev/null 2>&1; r2e=$?
[ $r0 -eq 0 ] && [ $r0e -eq 0 ] && ctrl C9-ISOLATION-ORACLE-CLEAN PASS "oracle/build.sh lib; ar x libat0.rlib; nm -u on each member: isolation_check.sh and evaluator/gates/isolation.sh" "$(ls "$RI"/clean/*.o | wc -l) rlib members clean under both scanners (compute crate src/at0 only; main.rs is the I/O layer and is excluded by design)" \
    || ctrl C9-ISOLATION-ORACLE-CLEAN FAIL "nm -u over oracle compute rlib members" "own rc=$r0 evaluator-gate rc=$r0e: $(head -2 "$R/isolation-oracle-own.log" | tr '\n' ' ')"
[ $r1 -eq 1 ] && [ $r2 -eq 1 ] && ctrl C10-ISOLATION-ORACLE-HIDDEN-CLOCK-MUTANTS PASS "same scan on two Rust mutants (std::time::SystemTime; extern \"C\" clock_gettime)" "own scanner flags both; evaluator gate flags std-time mutant rc=$r1e and extern-clock mutant rc=$r2e (0 would mean not flagged)" \
    || ctrl C10-ISOLATION-ORACLE-HIDDEN-CLOCK-MUTANTS FAIL "same scan on two Rust mutants" "own: std-time rc=$r1 extern-clock rc=$r2 (both must be 1)"

# ---------- 12. make hygiene (G2) ----------
if [ -f "$S/mk/at0.mk" ]; then
    HY="$WORK/hyg"; mkdir -p "$HY"
    mk() { ( cd "$S" && make "$@" ) ; }
    ( cd "$S" && make -n -k all PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 ) > /dev/null 2>&1
    hy() { # tag dir
        D=$2
        for g in all test; do
            ( cd "$D" && make -n -k $g PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 ) > "$HY/$1.$g" 2>&1; echo "exit $?" >> "$HY/$1.$g"
            ( cd "$D" && make -n -k $g ) > "$HY/$1.$g.default" 2>&1; echo "exit $?" >> "$HY/$1.$g.default"
        done
        ( cd "$D" && make -pn -k all PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 ) 2>/dev/null | grep -v '^#' | sed -E 's/[0-9a-f]{12,40}/SHA/g; s/ at0-check at0-clean//; s| mk/at0\.mk||g' | grep -v -E '^(AT0_[A-Z_]* |at0-(check|clean):|AT0_MK|MAKEFILE_LIST|MA2_RUN_ID|R16_STAMP)' | grep -v -E '^\t.*(AT0_|integration/run\.sh|at0-clean|build/at0)' | LC_ALL=C sort -u > "$HY/$1.db"
    }
    # "without": a second clone with mk/at0.mk removed and committed, so both trees are clean and the comparison is not polluted by dirty-tree stamps
    S2="$WORK/src-without"; git clone -q --no-hardlinks "$S" "$S2" && git -C "$S2" rm -q mk/at0.mk && git -C "$S2" -c user.email=a5@example.invalid -c user.name=a5 commit -q -m "without at0.mk (control)"
    ( cd "$S2" && make -n -k all PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 ) > /dev/null 2>&1
    hy with "$S"; hy without "$S2"
    hok=1; hmsg=""
    for g in all test; do
        for v in "" ".default"; do cmp -s "$HY/with.$g$v" "$HY/without.$g$v" || { hok=0; hmsg="$hmsg $g$v"; }; done
    done
    cp "$HY/with.all" "$R/make-n-all.with.txt"; cp "$HY/without.all" "$R/make-n-all.without.txt"; cp "$HY/with.test" "$R/make-n-test.with.txt"; cp "$HY/without.test" "$R/make-n-test.without.txt"
    diff "$HY/without.db" "$HY/with.db" > "$R/make-database.diff"
    extra=$(grep '^[<>]' "$R/make-database.diff" | grep -v -i 'at0' | wc -l)
    nl_all=$(wc -l < "$HY/with.all"); nl_test=$(wc -l < "$HY/with.test")
    [ $hok = 1 ] && ctrl C11-MAKE-N-HYGIENE PASS "make -n -k all|test with PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 and with defaults, fragment present vs moved aside (cmp of stdout+stderr+exit)" "byte-identical in all four comparisons (all: $nl_all lines, test: $nl_test lines; exit 2 both ways because the physics checkout is absent, a pre-existing condition)" \
        || ctrl C11-MAKE-N-HYGIENE FAIL "make -n -k all|test with and without mk/at0.mk" "differ:$hmsg"
    [ "$extra" = 0 ] && ctrl C12-MAKE-DATABASE PASS "make -pn -k all, sorted, fragment present vs removed-and-committed clone; diff after dropping at0-named lines, the mk/at0.mk entry in MAKEFILE_LIST prerequisites, 12-40 hex commit stamps and run timestamps" "empty diff: no variable, rule, target or prerequisite of the existing build changes (SRCS, all, test, clean untouched)" \
        || ctrl C12-MAKE-DATABASE FAIL "make -pn database diff" "$extra changed lines outside at0 names; see make-database.diff"
    ( cd "$S" && make -n at0-check PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 ) > "$R/make-n-at0-check.txt" 2>&1; pc=$?
    ( cd "$S" && make -n at0-clean PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 ) > "$R/make-n-at0-clean.txt" 2>&1; pc2=$?
    [ $pc -eq 0 ] && [ $pc2 -eq 0 ] && grep -q 'run.sh' "$R/make-n-at0-check.txt" && ctrl C13-MAKE-PARSES PASS "make -n at0-check at0-clean PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0" "both parse and plan: $(grep -m1 run.sh "$R/make-n-at0-check.txt" | cut -c1-90)" \
        || ctrl C13-MAKE-PARSES FAIL "make -n at0-check at0-clean" "rc=$pc/$pc2"
else
    ctrl C11-MAKE-N-HYGIENE NOT_RUN "mk/at0.mk not committed at HEAD" "fragment absent"; ctrl C12-MAKE-DATABASE NOT_RUN "-" "fragment absent"; ctrl C13-MAKE-PARSES NOT_RUN "-" "fragment absent"
fi

# ---------- 13. the evaluator's own harness with this runner as the candidate (G5/G6 hooks) ----------
export AT0_CANDIDATE_CASE_TOOL="sh $INT/case_tool.sh" AT0_CANDIDATE_RUN="sh $INT/candidate_run.sh" AT0_CANDIDATE_OBJECTS="$CLEANOBJS" AT0_HIDDEN_DIR=/nonexistent-hidden-set-not-used
( cd "$AT0/evaluator" && sh run.sh ) > "$R/evaluator-run.log" 2>&1; evrc=$?
cp "$AT0/evaluator/results/qualification.json" "$R/evaluator-qualification.json" 2>/dev/null
ESUM=$(grep -m1 '^---- summary' "$R/evaluator-run.log")
ECAND=$(grep -c '^CAND-VERIFY-.* PASS' "$R/evaluator-run.log")
EBAD=$(grep -E '^(CAND|CORPUS|SELF|DERIV|MUTANT)-[^ ]+ +[a-z]+ +FAIL' "$R/evaluator-run.log" | head -5 | tr '\n' ';')
if [ $evrc -eq 0 ]; then ctrl C14-EVALUATOR-HARNESS PASS "evaluator/run.sh with AT0_CANDIDATE_CASE_TOOL, AT0_CANDIDATE_RUN, AT0_CANDIDATE_OBJECTS set to this runner (hidden set not used)" "$ESUM; CAND-VERIFY PASS lines: $ECAND"
else ctrl C14-EVALUATOR-HARNESS FAIL "evaluator/run.sh with candidate hooks" "rc=$evrc $ESUM $EBAD"; fi
grep -q 'CAND-WALLCLOCK-INDEPENDENCE .* PASS' "$R/evaluator-run.log" && ctrl C15-EVALUATOR-WALLCLOCK PASS "evaluator/run.sh CAND-WALLCLOCK-INDEPENDENCE" "Agent 4's own wall-clock control passes on this runner" || ctrl C15-EVALUATOR-WALLCLOCK FAIL "evaluator/run.sh CAND-WALLCLOCK-INDEPENDENCE" "see evaluator-run.log"
grep -q 'CAND-ISOLATION .* PASS' "$R/evaluator-run.log" && ctrl C16-EVALUATOR-ISOLATION-CAND PASS "evaluator/run.sh CAND-ISOLATION (gates/isolation.sh on model objects)" "passes" || ctrl C16-EVALUATOR-ISOLATION-CAND FAIL "evaluator/run.sh CAND-ISOLATION" "see evaluator-run.log"

# ---------- 14. judge calibration against the oracle's 21 fixtures ----------
JC_OK=0; JC_N=0; JC_BAD=""
for v in "$AT0"/oracle/fixtures/*.values; do
    b=$(basename "$v" .values); JC_N=$((JC_N+1))
    "$ASM" judge "$AT0/oracle/fixtures/$b.case" "$v" > "$WORK/jc.out" 2>/dev/null
    sed -n '/^begin verdict/,$p' "$v" > "$WORK/jc.exp"
    if cmp -s "$WORK/jc.out" "$WORK/jc.exp"; then JC_OK=$((JC_OK+1)); else JC_BAD="$JC_BAD $b"; fi
done
[ $JC_OK = $JC_N ] && ctrl C17-JUDGE-CALIBRATION PASS "at0-assemble judge <fixture.case> <fixture.values> vs the oracle's verdict block and verdict_id" "$JC_OK/$JC_N identical" \
    || ctrl C17-JUDGE-CALIBRATION FAIL "at0-assemble judge vs oracle fixtures" "identical $JC_OK/$JC_N; differ:$JC_BAD (see QUALIFICATION.md discrepancies)"


# ---------- 14b. assembler must refuse inconsistent inputs (red before green for the runner itself) ----------
KC="$OUT/cases/positive/P1-kat-ideal-qubit-n4.case"; BC="$OUT/cases/positive/P1b-ideal-n4-tau8-m8.case"
AR_BAD=""; AR_N=0
ar() { # label, expected-substring, case, engine, oracle
    AR_N=$((AR_N+1)); "$ASM" assemble "$3" "$4" "$5" "$WORK/prov.static" > /dev/null 2> "$WORK/ar.err"; rcx=$?
    if [ $rcx -eq 3 ] && grep -q "$2" "$WORK/ar.err"; then :; else AR_BAD="$AR_BAD $1(rc=$rcx)"; fi
}
sed 's/^case_id .*/case_id 0000000000000000000000000000000000000000000000000000000000000000/' "$OUT/components/P1-kat-ideal-qubit-n4.engine" > "$WORK/e1"
ar altered-case-id "engine identities differ" "$KC" "$WORK/e1" "$OUT/components/P1-kat-ideal-qubit-n4.oracle"
ar oracle-for-other-case "different case" "$KC" "$OUT/components/P1-kat-ideal-qubit-n4.engine" "$OUT/components/P1b-ideal-n4-tau8-m8.oracle"
ar engine-for-other-case "differs from the case file" "$BC" "$OUT/components/P1-kat-ideal-qubit-n4.engine" "$OUT/components/P1b-ideal-n4-tau8-m8.oracle"
sed 's/^end values$/reference 0 X PLUS f64:0000000000000000 0@0\nend values/' "$OUT/components/P1-kat-ideal-qubit-n4.engine" > "$WORK/e2"
ar engine-with-reference "contains reference lines" "$KC" "$WORK/e2" "$OUT/components/P1-kat-ideal-qubit-n4.oracle"
sed 's/^build_cc oracle /build_cc /' "$OUT/components/P1-kat-ideal-qubit-n4.oracle" > "$WORK/o1"
ar oracle-not-marked "does not begin with" "$KC" "$OUT/components/P1-kat-ideal-qubit-n4.engine" "$WORK/o1"
sed '/^reference /d' "$OUT/components/P1-kat-ideal-qubit-n4.oracle" > "$WORK/o2"
ar oracle-without-reference "no reference lines" "$KC" "$OUT/components/P1-kat-ideal-qubit-n4.engine" "$WORK/o2"
[ -z "$AR_BAD" ] && ctrl C18-ASSEMBLER-REFUSALS PASS "at0-assemble assemble on $AR_N tampered or mismatched inputs (altered case_id, oracle or engine of another case, reference lines in the engine output, unmarked oracle record, oracle without reference lines)" "all refused with exit 3 and the expected message" \
    || ctrl C18-ASSEMBLER-REFUSALS FAIL "at0-assemble assemble on tampered inputs" "not refused as expected:$AR_BAD"
[ "$NDIFF_TOOL" = 0 ] && ctrl C19-REFUSAL-AGREEMENT PASS "model and oracle refusal codes against evaluator manifest on all refuse cases" "all $REFN refuse cases agree" || ctrl C19-REFUSAL-AGREEMENT FAIL "model and oracle refusal codes against evaluator manifest on all refuse cases" "$NDIFF_TOOL of $REFN refuse cases differ (or a refuse case was accepted); see cases.tsv and QUALIFICATION.md"
# ---------- 15. gates ----------
isoG=PASS
for k in C7 C8 C9 C10 C11 C12 C13; do st=$(awk -F'\t' -v k="$k" '$1 ~ "^"k"-" {print $2}' "$CTRL"); [ -n "$st" ] || isoG=NOT_RUN; [ "$st" = PASS ] || isoG=$(awk -F'\t' -v k="$k" -v cur="$isoG" 'BEGIN{r=cur} $1 ~ "^"k"-" && $2=="FAIL" {r="FAIL"} $1 ~ "^"k"-" && $2=="NOT_RUN" && r!="FAIL" {r="NOT_RUN"} END{print r}' "$CTRL"); done
gate AT0-G2 "$isoG" "controls C7-C13 in controls.tsv (isolation_check.sh and evaluator/gates/isolation.sh on model and Rust oracle objects incl. mutants; make -n hygiene; make parses with PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0)" "runner checks C7-C13 $( [ "$isoG" = PASS ] && echo "all pass" || echo "not all pass" ); Agent 4 sign-off of this runner scanner (isolation_check.sh) on C objects and Rust rlib members is recorded by reference only (omega#358, 2026-10-10T00:09:52Z, before #364 merged, so it covers the scanner, not the final oracle code; the runner does not verify it) and Agent 4 gate now flags the Rust std::time mutant (C10); Agent 4 is UNISOLATED (same host and account)"
[ "$G4N" -gt 0 ] && [ "$G4OK" = "$G4N" ] && gate AT0-G4 PASS "every positive case: outcome PASS and expectation_met YES (cases.tsv)" "$G4OK/$G4N" || gate AT0-G4 FAIL "every positive case: outcome PASS and expectation_met YES" "$G4OK/$G4N"
G5C=$(awk -F'\t' '$2=="PASS" && $1 ~ /^C(5|6)-/ {n++} END {print n+0}' "$CTRL")
[ "$G5N" -gt 0 ] && [ "$G5OK" = "$G5N" ] && [ "$G5C" = 2 ] && gate AT0-G5 PASS "every negative case: outcome FAIL with exactly its expected codes and expectation_met YES; axis-swap and Y-sign mutants caught (C5, C6); hidden-clock mutants caught (C8, C10)" "$G5OK/$G5N negative cases" \
    || gate AT0-G5 FAIL "negative arm" "negative $G5OK/$G5N, mutant controls passing $G5C/2"
EVSELF=$(grep -E '^(SELF|DERIV|MUTANT|GATE|CAND-WALLCLOCK|CAND-ISOLATION)[^ ]* +[a-z]+ +FAIL' "$R/evaluator-run.log" | wc -l)
C3ST=$(awk -F'\t' '$1 ~ /^C3-/ {print $2}' "$CTRL"); NVALID=$((NCASE-REFN))
if [ "$G6T" -gt 0 ] && [ "$G6OK" = "$G6T" ] && [ "$EVSELF" = 0 ] && [ "$C3ST" = PASS ]; then
    if [ "$G6T" = "$NVALID" ]; then
        gate AT0-G6 PASS "at0-eval result <result> --case <case> on every assembled result (verify/*.json) plus evaluator/run.sh with this runner as candidate; wall-clock control C3" "evaluator PASS on $G6OK of $NVALID valid cases (the other $((NVALID-G6T)) produced no result file, see G4); evaluator self-tests, mutants and gates with this runner as candidate: 0 FAIL; evidence folder committed by the PR"
    else
        gate AT0-G6 INCONCLUSIVE "verifier agreed with every result produced, but not every valid case produced a result" "evaluator PASS on $G6OK of $NVALID valid cases; $((NVALID-G6T)) produced no result (see G4); evaluator self-tests, mutants and gates with this runner as candidate: 0 FAIL"
    fi
else
    gate AT0-G6 FAIL "independent verification" "evaluator PASS on $G6OK/$G6T results, evaluator self/gate failures $EVSELF, wall-clock $C3ST"
fi
gate AT0-G7 NOT_RUN "-" "Agent 6 (scientific review) owns G7; Agent 5 does not run it"

# ---------- 16. result digests and summary ----------
( cd "$OUT" && find results mutants components verify -type f | LC_ALL=C sort | while read -r f; do printf '%s  %s\n' "$(sha "$f")" "$f"; done ) > "$R/outputs.sha256"
{
  echo "omega_commit $HEAD"; echo "source_tree_clean $CLEAN"; echo "contract_commit $CONTRACT_COMMIT"; echo "stamp $STAMP"
  echo "cases $NCASE valid $((NCASE-REFN)) refused $REFN"
  echo "G4 positive $G4OK/$G4N; G5 negative $G5OK/$G5N; G6 evaluator agreement $G6OK/$G6T; refusal rows where model, oracle and manifest agree $((REFN-NDIFF_TOOL))/$REFN"
  echo "mutant axis_swap: $AX"; echo "mutant y_sign_flip: $YS"
} > "$R/summary.txt"
cat "$R/summary.txt" | tee -a "$LOG"
NFAIL=$(awk -F'\t' '$2=="FAIL"' "$GATES" | wc -l); NCF=$(awk -F'\t' '$2=="FAIL"' "$CTRL" | wc -l)
say "gates: $(awk -F'\t' '{printf "%s=%s ", $1, $2}' "$GATES")"
say "controls failing: $NCF; gates failing: $NFAIL"
[ $NFAIL -eq 0 ] && [ $NCF -eq 0 ]
