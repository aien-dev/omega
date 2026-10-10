#!/bin/sh
# AT-1 Agent 5: clean-checkout integration runner (AT1_CHARTER.md sections 3, 6; gates G1 to G6).
#
#   sh run.sh [--out DIR | --evidence] [--keep-work]
#
# Builds the AT-1 oracle, engine and evaluator from a fresh local clone of this checkout's HEAD (the
# working tree is never touched), runs every public evaluator case through
#   oracle -> engine (which splices every reference_* line of the oracle record, charter reading (c))
#   -> the evaluator's own runner (its splice, verifier, gate lines)
# twice for repeatability, and runs the integration controls (contract digests, splice agreement,
# evaluation order, real-engine mutants, isolation, make hygiene). Writes receipts.
#   --evidence   writes the new folder evidence/AT1/<UTC stamp>-<short commit> (append-only; must not exist)
#   --out DIR    writes to DIR (wiped first); default build/at1/check (what `make at1-check` uses)
# Optional environment:
#   AT1_ARCH_DIR       a local aien-architecture git clone: enables the contract digest control C0
#   AT1_QUALIFY_COMMIT an omega commit whose AT-1 component sources must equal HEAD's (control C0)
#   CC                 C compiler for the integration controls (default cc); components use their own builds
# Single-threaded, CPU only, no network, no Python, no GPU. Never reads a hidden case.
# Exit 0 only if no control and no gate is FAIL. POSIX sh; Linux (GNU tools) and macOS (BSD tools).
set -u
LC_ALL=C; export LC_ALL
TZ=UTC; export TZ
# the runner may be started by `make at1-check`: inner makes must not inherit its state
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
if [ "$MODE" = evidence ]; then
    OUT="$ROOT/evidence/AT1/$STAMP-$SHORT"
    [ -e "$OUT" ] && { echo "evidence folder exists: $OUT" >&2; exit 64; }
else
    [ -n "$OUT" ] || OUT="$ROOT/build/at1/check"
    case "$OUT" in /*) ;; *) OUT="$PWD/$OUT";; esac
    case "$OUT" in "$ROOT/evidence"|"$ROOT/evidence/"*) echo "--out must not point into evidence/; use --evidence" >&2; exit 64;; esac
    rm -rf "$OUT"
fi
mkdir -p "$OUT/receipts" "$OUT/cases" "$OUT/results" "$OUT/components" "$OUT/mutants" || exit 1
WORK=$(mktemp -d "${TMPDIR:-/tmp}/at1-run.XXXXXX") || exit 1
[ $KEEP = 1 ] || trap 'rm -rf "$WORK"' EXIT
LOG="$OUT/run.log"; : > "$LOG"
R="$OUT/receipts"
say() { printf '%s\n' "$*" | tee -a "$LOG"; }
logrun() { printf '$ %s\n' "$*" >> "$LOG"; }
if command -v sha256sum > /dev/null 2>&1; then SHATOOL="sha256sum"; else SHATOOL="shasum -a 256"; fi
sha() { $SHATOOL "$1" | cut -c1-64; }
shastdin() { $SHATOOL | cut -c1-64; }
GATES="$R/gates.tsv"; CTRL="$R/controls.tsv"; : > "$GATES"; : > "$CTRL"
gate() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$GATES"; say "GATE $1 $2  $4"; }
ctrl() { printf '%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$4" >> "$CTRL"; say "CONTROL $1 $2  $4"; }
cst() { awk -F'\t' -v k="$1" '$1==k {print $2}' "$CTRL" | tail -1; }
field() { sed -n "s/^$2 //p" "$1" | head -1; }
valblock() { sed -n '/^begin values$/,/^end values$/p' "$1"; }
# a result without the three lines the contract allows to differ between honest runs
stable() { grep -v -E '^(run_started_utc|run_finished_utc|evidence_digest) ' "$1"; }
CCBIN=${CC:-cc}
TAB=$(printf '\t')

say "AT-1 Agent 5 run $STAMP mode=$MODE omega=$HEAD tree_clean=$CLEAN"

# ---------- 0. contract pin (C0) ----------
LOCK="$HERE/contract.lock"
lock() { sed -n "s/^$1 //p" "$LOCK" | head -1; }
CONTRACT_COMMIT=$(lock contract_commit); SPEC_COMMIT=$(lock spec_commit)
cp "$LOCK" "$R/contract.lock"
if [ -n "${AT1_ARCH_DIR:-}" ] && git -C "$AT1_ARCH_DIR" rev-parse HEAD > /dev/null 2>&1; then
    A=$AT1_ARCH_DIR; P=docs/plans/atemporal; c0bad=""
    adig() { git -C "$A" show "$1:$P/$2" 2>/dev/null | shastdin; }
    for row in "AT1_CASE_V1 case_v1_sha256 $CONTRACT_COMMIT" "AT1_RESULT_V1 result_v1_sha256 $CONTRACT_COMMIT" "AT0_RESULT_V2 result_v2_sha256 $CONTRACT_COMMIT" "AT1_SPEC spec_sha256 $SPEC_COMMIT"; do
        set -- $row
        want=$(lock "$2"); got=$(adig "$3" "$1.md"); head_got=$(sha "$A/$P/$1.md" 2>/dev/null)
        [ "$got" = "$want" ] || c0bad="$c0bad $1@${3%"${3#???????}"}($got)"
        [ "$head_got" = "$want" ] || c0bad="$c0bad $1@working-tree($head_got)"
        printf '%s %s at %s: %s; working tree: %s; lock: %s\n' "$1" "$2" "$3" "$got" "$head_got" "$want" >> "$R/contract-digests.txt"
    done
    # the frozen rows of the freeze record carry the same digests
    for k in "AT1_CASE_V1 case_v1_sha256" "AT1_RESULT_V1 result_v1_sha256" "AT0_RESULT_V2 result_v2_sha256"; do
        set -- $k
        grep -q "^| $1 | .*| \`$(lock "$2")\` |" "$A/$P/AT0_FREEZE.md" || c0bad="$c0bad freeze-row-$1"
    done
    ch=$(adig "$(lock charter_readings_commit)" AT1_CHARTER.md)
    [ "$ch" = "$(lock charter_sha256_at_readings_commit)" ] || c0bad="$c0bad charter-at-readings-commit($ch)"
    printf 'aien-architecture HEAD %s\n' "$(git -C "$A" rev-parse HEAD)" >> "$R/contract-digests.txt"
    [ -z "$c0bad" ] && ctrl C0-CONTRACT-DIGESTS PASS "sha256 of AT1_CASE_V1, AT1_RESULT_V1, AT0_RESULT_V2 at contract_commit and AT1_SPEC at spec_commit (git show) and in the working tree of \$AT1_ARCH_DIR; AT0_FREEZE.md rows; AT1_CHARTER at the readings commit" "all match contract.lock and the AT0_FREEZE.md rows (arch HEAD $(git -C "$A" rev-parse --short=7 HEAD))" \
        || ctrl C0-CONTRACT-DIGESTS FAIL "contract digests" "mismatch:$c0bad"
else ctrl C0-CONTRACT-DIGESTS NOT_RUN "AT1_ARCH_DIR unset or not a git clone" "no local aien-architecture clone given; digests only recorded in contract.lock"; fi

# ---------- 1. clean checkout and toolchain ----------
S="$WORK/src"; AT1="$S/research/atemporal/at1"; INT="$AT1/integration"
logrun "git clone --no-hardlinks $ROOT $S; git checkout --detach $HEAD"
git clone -q --no-hardlinks "$ROOT" "$S" >> "$LOG" 2>&1 && git -C "$S" checkout -q --detach "$HEAD" >> "$LOG" 2>&1 || { say "FATAL clone failed"; exit 1; }
[ "$(git -C "$S" rev-parse HEAD)" = "$HEAD" ] || { say "FATAL clone HEAD mismatch"; exit 1; }
[ -d "$INT" ] || { say "FATAL HEAD has no research/atemporal/at1/integration (commit the runner first)"; exit 1; }
COMPS="research/atemporal/at1/model research/atemporal/at1/oracle research/atemporal/at1/evaluator src/sha256.c src/sha256.h"
if [ -n "${AT1_QUALIFY_COMMIT:-}" ]; then
    QC=$(git -C "$S" rev-parse "$AT1_QUALIFY_COMMIT^{commit}" 2>/dev/null)
    if [ -n "$QC" ] && git -C "$S" diff --quiet "$QC" "$HEAD" -- $COMPS; then
        ctrl C0-COMPONENTS-AT-QUALIFIED-COMMIT PASS "git diff --quiet $QC HEAD -- $COMPS" "component sources at HEAD are byte-identical to $QC; HEAD differs from it only in: $(git -C "$S" diff --name-only "$QC" "$HEAD" | sed 's|/[^/]*$||' | sort -u | tr '\n' ' ')"
    else ctrl C0-COMPONENTS-AT-QUALIFIED-COMMIT FAIL "git diff --quiet $AT1_QUALIFY_COMMIT HEAD -- $COMPS" "component sources differ (or commit unknown)"; fi
else ctrl C0-COMPONENTS-AT-QUALIFIED-COMMIT NOT_RUN "AT1_QUALIFY_COMMIT unset" "components are those of HEAD"; fi
HOSTN=$(cat /etc/hostname 2>/dev/null || uname -n)
OSDESC=$(uname -srm); command -v sw_vers > /dev/null 2>&1 && OSDESC="$(sw_vers -productName) $(sw_vers -productVersion) $(uname -m)"
CC_V=$($CCBIN --version 2>&1 | head -1)
RUSTC_V=$(rustc --version 2>&1)
{
    echo "date_utc $STAMP"; echo "uname $(uname -srm)"; echo "host $HOSTN"; echo "os $OSDESC"
    echo "cc ($CCBIN) $CC_V"; echo "make $(make --version 2>&1 | head -1)"; echo "nm $(nm --version 2>&1 | head -1)"; echo "sh $(ls -l /bin/sh 2>&1)"
    echo "sha tool $SHATOOL"; echo "git $(git --version)"
    echo "--- rustc -vV"; rustc -vV 2>&1
} > "$R/toolchain.txt"
MCFLAGS=$(sed -n 's/^CFLAGS = //p' "$AT1/model/Makefile" | head -1)
OFLAGS=$(sed -n 's/^FLAGS="\(.*\)"$/\1/p' "$AT1/oracle/build.sh" | head -1)
{ echo "model (model/Makefile CFLAGS, LDLIBS -lm): $MCFLAGS"; echo "oracle (oracle/build.sh): rustc $OFLAGS --crate-name at1_oracle src/main.rs"
  echo "evaluator (evaluator/Makefile): rustc --edition 2021 -O src/main.rs"; echo "integration objects for G2 (this runner): $CCBIN $MCFLAGS -c"; } > "$R/flags.txt"

# ---------- 2. builds and component tests (oracle first: its build records the clean state of its directory) ----------
bstep() { # label dir cmd...
    lab=$1; d=$2; shift 2; logrun "(cd $d && $*)"
    (cd "$d" && "$@") > "$R/$lab.log" 2>&1; rc=$?
    [ $rc -eq 0 ] && say "BUILD $lab ok" || say "BUILD $lab FAILED rc=$rc"; return $rc
}
BUILD_OK=1
bstep oracle-build "$AT1/oracle" sh build.sh || BUILD_OK=0
bstep model-build "$AT1/model" make || BUILD_OK=0
bstep evaluator-build "$AT1/evaluator" make all || BUILD_OK=0
MODEL="$AT1/model/build/at1-model"; ORACLE="$AT1/oracle/target/at1-oracle"; EVAL="$AT1/evaluator/build/at1-eval"
if [ $BUILD_OK != 1 ] || [ ! -x "$MODEL" ] || [ ! -x "$ORACLE" ] || [ ! -x "$EVAL" ]; then
    say "FATAL build failure; see $R/*-build.log"
    for g in AT1-G1 AT1-G2 AT1-G3 AT1-G4 AT1-G5 AT1-G6; do gate $g NOT_RUN "build" "a component did not build"; done
    exit 1
fi
[ -z "$(git -C "$S" status --porcelain)" ] && ctrl C1-BUILD-LEAVES-TREE-CLEAN PASS "git status --porcelain in the clone after the three builds" "empty (build outputs are git-ignored)" \
    || ctrl C1-BUILD-LEAVES-TREE-CLEAN FAIL "git status --porcelain in the clone after the builds" "$(git -C "$S" status --porcelain | head -3 | tr '\n' ' ')"
OSTATE=$("$ORACLE" emit "$AT1/evaluator/cases/positive/P2-kat-rotated-level-n4.case" 2>/dev/null | sed -n 's/^source_tree_clean //p')
( cd "$AT1/model" && make test ) > "$R/model-tests.log" 2>&1; mrc=$?
( cd "$AT1/model" && make asan ) > "$R/model-tests-asan.log" 2>&1; mrca=$?
( cd "$AT1/oracle" && sh build.sh test ) > "$R/oracle-tests.log" 2>&1; orc=$?
( cd "$AT1/oracle" && sh isolation.sh ) > "$R/oracle-isolation-own.log" 2>&1; oisorc=$?
( cd "$AT1/evaluator" && make test CC="$CCBIN" ) > "$R/evaluator-tests.log" 2>&1; erc=$?
ctrl C1-MODEL-TESTS "$( [ $mrc -eq 0 ] && [ $mrca -eq 0 ] && echo PASS || echo FAIL )" "(cd model && make test; make asan)" "plain rc=$mrc ($(grep -E 'passed|failed' "$R/model-tests.log" | tail -1)); asan rc=$mrca ($(grep -E 'passed|failed|skipped' "$R/model-tests-asan.log" | tail -1))"
ctrl C1-ORACLE-TESTS "$( [ $orc -eq 0 ] && echo PASS || echo FAIL )" "(cd oracle && sh build.sh test)" "rc=$orc ($(grep -m1 'test result' "$R/oracle-tests.log"))"
ctrl C1-ORACLE-OWN-ISOLATION "$( [ $oisorc -eq 0 ] && echo PASS || echo FAIL )" "(cd oracle && sh isolation.sh)" "Agent 2's own gate rc=$oisorc ($(tail -1 "$R/oracle-isolation-own.log"))"
ctrl C1-EVALUATOR-TESTS "$( [ $erc -eq 0 ] && echo PASS || echo FAIL )" "(cd evaluator && make test)" "rc=$erc; $(grep -E '^(SELFTEST|corpus-check|MUTANT_RECEIPT|AT1_ISOLATION_CONTROLS)' "$R/evaluator-tests.log" | tr '\n' ';')"
cp "$AT1/evaluator/build/ISOLATION_RECEIPT.txt" "$R/evaluator-isolation-controls.txt" 2>/dev/null

# ---------- 3. receipts: sources and binaries ----------
( cd "$S" && git ls-files research/atemporal/at1 src/sha256.c src/sha256.h mk/at1.mk | LC_ALL=C sort | while read -r f; do printf '%s  %s\n' "$(sha "$f")" "$f"; done ) > "$R/sources.sha256"
{ for b in "$MODEL" "$ORACLE" "$EVAL" "$AT1/model/build/test_model" "$AT1/oracle/target/at1-oracle-test"; do
    [ -f "$b" ] && printf '%s  %s\n' "$(sha "$b")" "${b#"$WORK"/}"; done; } > "$R/binaries.sha256"
ENGINE_SHA=$(sha "$MODEL"); ORACLE_SHA=$(sha "$ORACLE")

# ---------- 4. provenance template (the engine reads no clock; the shim adds the two run times) ----------
ptext() { printf '%s' "$1" | tr -cd ' -~' | tr -s ' ' | sed 's/^ //; s/ $//' | cut -c1-200 | sed 's/ $//'; }
BCC=$(ptext "$CC_V (engine); $RUSTC_V (oracle)")
BFL=$(ptext "engine: $MCFLAGS -lm; oracle: rustc $OFLAGS")
HOSTT=$(ptext "$HOSTN $OSDESC")
PROV="$WORK/prov.static"
{ echo "source_repo aien-dev/omega"; echo "source_commit $HEAD"; echo "source_tree_clean $CLEAN"; echo "contract_commit $CONTRACT_COMMIT"
  echo "engine_sha256 $ENGINE_SHA"; echo "oracle_repo aien-dev/omega"; echo "oracle_commit $HEAD"; echo "oracle_sha256 $ORACLE_SHA"
  echo "build_cc $BCC"; echo "build_flags $BFL"; echo "host $HOSTT"; } > "$PROV"
cp "$PROV" "$R/provenance.static"
export AT1_MODEL_BIN="$MODEL" AT1_ORACLE_BIN="$ORACLE" AT1_PROV_STATIC="$PROV"
ENGT="sh $INT/engine_shim.sh {case} {out}"; ORAT="sh $INT/oracle_shim.sh {case} {out}"

# ---------- 5. case set (public corpus only; the hidden sets are never touched) ----------
EC="$AT1/evaluator/cases"
cp -R "$EC/." "$OUT/cases/"
( cd "$OUT/cases" && find . -type f | sed 's|^\./||' | LC_ALL=C sort | while read -r c; do printf '%s  %s\n' "$(sha "$c")" "$c"; done ) > "$R/cases.sha256"
NPOS=$(grep -c '^positive/' "$EC/MANIFEST.tsv"); NNEG=$(grep -c '^negative/' "$EC/MANIFEST.tsv"); NREF=$(grep -c '^refuse/' "$EC/MANIFEST.tsv")
NCASE=$((NPOS+NNEG+NREF))
say "cases: $NCASE (positive $NPOS, negative $NNEG, refuse $NREF); hidden sets not touched"

# ---------- 6. pass 1: the evaluator's own runner with the two shims ----------
P1="$OUT/results/pass1"; P2="$OUT/results/pass2"
logrun "(cd evaluator && sh run.sh '$ENGT' '$ORAT' cases $P1)"
( cd "$AT1/evaluator" && AT1_COMP_DIR="$OUT/components" sh run.sh "$ENGT" "$ORAT" cases "$P1" ) > "$R/evaluator-run-pass1.log" 2>&1; ev1=$?
say "pass 1: evaluator run.sh rc=$ev1"

# ---------- 7. pass 2 (repeatability): later, other time zone, other working directory ----------
sleep 2
logrun "(cd /; TZ=Asia/Tokyo sh evaluator/run.sh '$ENGT' '$ORAT' cases $P2)"
( cd / && TZ=Asia/Tokyo sh "$AT1/evaluator/run.sh" "$ENGT" "$ORAT" cases "$P2" ) > "$R/evaluator-run-pass2.log" 2>&1; ev2=$?
say "pass 2: evaluator run.sh rc=$ev2"
RP_N=0; RP_BYTE=0; RP_STABLE=0; RP_BAD=""
for f in "$P1"/engine/*.result "$P1"/oracle/*.result "$P1"/spliced/*.result; do
    [ -f "$f" ] || continue
    rel=${f#"$P1"/}; g="$P2/$rel"; RP_N=$((RP_N+1))
    [ -f "$g" ] || { RP_BAD="$RP_BAD $rel(missing)"; continue; }
    if cmp -s "$f" "$g"; then RP_BYTE=$((RP_BYTE+1)); RP_STABLE=$((RP_STABLE+1))
    elif [ "$(stable "$f")" = "$(stable "$g")" ]; then RP_STABLE=$((RP_STABLE+1))
    else RP_BAD="$RP_BAD $rel"; fi
done
for t in TABLE.tsv VERDICT_IDS.tsv; do cmp -s "$P1/$t" "$P2/$t" || RP_BAD="$RP_BAD $t"; done
if [ $RP_N -gt 0 ] && [ $RP_STABLE = $RP_N ] && [ -z "$RP_BAD" ] && [ $ev1 = $ev2 ]; then
    ctrl C2-REPEATABILITY PASS "evaluator run.sh twice (2 s apart; TZ=UTC from evaluator/, then TZ=Asia/Tokyo from /); cmp of every engine, oracle and spliced result, TABLE.tsv, VERDICT_IDS.tsv" "$RP_N result files: $RP_STABLE/$RP_N identical outside run_started_utc, run_finished_utc, evidence_digest ($RP_BYTE/$RP_N byte-identical); TABLE.tsv and VERDICT_IDS.tsv byte-identical; both runs rc=$ev1"
else ctrl C2-REPEATABILITY FAIL "evaluator run.sh twice; compare" "stable $RP_STABLE/$RP_N, rc $ev1/$ev2; differ:$RP_BAD"; fi

# ---------- 8. integration controls over pass 1 ----------
# C3: the engine's own splice and the evaluator's independent splice give the same file
SP_N=0; SP_OK=0; SP_BAD=""
for f in "$P1"/spliced/*.result; do
    [ -f "$f" ] || continue
    b=$(basename "$f"); SP_N=$((SP_N+1))
    cmp -s "$f" "$P1/engine/$b" && SP_OK=$((SP_OK+1)) || SP_BAD="$SP_BAD $b"
done
[ $SP_N -gt 0 ] && [ $SP_OK = $SP_N ] && ctrl C3-SPLICE-AGREEMENT PASS "cmp results/pass1/spliced/<case>.result results/pass1/engine/<case>.result (engine splice + judge vs evaluator splice + re-judge + reseal)" "$SP_OK/$SP_N byte-identical (same verdict block, verdict_id, provenance and evidence_digest)" \
    || ctrl C3-SPLICE-AGREEMENT FAIL "engine result vs evaluator splice" "identical $SP_OK/$SP_N; differ:$SP_BAD"
# C4: reading (c): the engine result's reference_* lines are exactly the oracle record's, in values-block
# order, and every other values line is the engine's own (at1-model values)
RC_N=0; RC_OK=0; RC_BAD=""
for f in "$P1"/engine/*.result; do
    [ -f "$f" ] || continue
    stem=$(basename "$f" .result); [ "$stem" = T9-second-run ] && continue
    cls=${stem%%_*}; name=${stem#*_}; cf="$EC/$cls/$name.case"; o="$OUT/components/$stem.oracle"
    RC_N=$((RC_N+1))
    [ -f "$o" ] || { RC_BAD="$RC_BAD $stem(no-record)"; continue; }
    valblock "$f" | grep '^reference_' > "$WORK/rc.e"; valblock "$o" | grep '^reference_' > "$WORK/rc.o"
    valblock "$f" | sed '1d;$d' | grep -v '^reference_' > "$WORK/rc.own"; "$MODEL" values "$cf" > "$WORK/rc.val" 2>/dev/null
    if [ -s "$WORK/rc.o" ] && cmp -s "$WORK/rc.e" "$WORK/rc.o" && cmp -s "$WORK/rc.own" "$WORK/rc.val" \
       && [ "$(cut -d' ' -f1 "$WORK/rc.e" | uniq | tr '\n' ' ')" = "reference_label reference_clock_probability reference_ideal reference_interacting " ]; then RC_OK=$((RC_OK+1)); else RC_BAD="$RC_BAD $stem"; fi
done
[ $RC_N -gt 0 ] && [ $RC_OK = $RC_N ] && ctrl C4-SPLICE-READING-C PASS "per engine result: reference_* lines vs the oracle record the engine consumed (components/), other values lines vs at1-model values <case>" "$RC_OK/$RC_N: all four reference_* families copied verbatim in values-block order (label, clock_probability, ideal, interacting), nothing else spliced" \
    || ctrl C4-SPLICE-READING-C FAIL "reading (c) splice check" "ok $RC_OK/$RC_N; bad:$RC_BAD"
# C5: every engine result names the pinned contract commit and the pinned provenance
CC_N=0; CC_OK=0
for f in "$P1"/engine/*.result; do [ -f "$f" ] || continue; CC_N=$((CC_N+1)); [ "$(field "$f" contract_commit)" = "$CONTRACT_COMMIT" ] && [ "$(field "$f" source_commit)" = "$HEAD" ] && [ "$(field "$f" engine_sha256)" = "$ENGINE_SHA" ] && [ "$(field "$f" oracle_sha256)" = "$ORACLE_SHA" ] && CC_OK=$((CC_OK+1)); done
for f in "$OUT"/components/*.oracle; do [ -f "$f" ] || continue; CC_N=$((CC_N+1)); [ "$(field "$f" contract_commit)" = "$CONTRACT_COMMIT" ] && [ "$(field "$f" engine_sha256)" = "$ORACLE_SHA" ] && [ "$(field "$f" source_commit)" = "$HEAD" ] && CC_OK=$((CC_OK+1)); done
[ $CC_N -gt 0 ] && [ $CC_OK = $CC_N ] && ctrl C5-PROVENANCE-PIN PASS "contract_commit, source_commit, engine_sha256, oracle_sha256 of every engine result and oracle record" "$CC_OK/$CC_N name contract_commit $CONTRACT_COMMIT, omega $SHORT and the built binaries; oracle build recorded source_tree_clean $(printf '%s' "$OSTATE" | tr '\n' ' ')" \
    || ctrl C5-PROVENANCE-PIN FAIL "provenance pin" "$CC_OK/$CC_N"
# C6: label evaluation order does not change a byte
EO_N=0; EO_OK=0; EO_BAD=""
printf 'run_started_utc 2026-01-01T00:00:00Z\nrun_finished_utc 2026-01-01T00:00:00Z\n' | cat "$PROV" - > "$WORK/prov.fixed"
for o in "$OUT"/components/*.oracle; do
    [ -f "$o" ] || continue
    stem=$(basename "$o" .oracle); cls=${stem%%_*}; name=${stem#*_}; cf="$EC/$cls/$name.case"; EO_N=$((EO_N+1))
    "$MODEL" result "$cf" "$o" "$WORK/prov.fixed" > "$WORK/eo.f" 2>/dev/null; "$MODEL" result "$cf" "$o" "$WORK/prov.fixed" --reversed > "$WORK/eo.r" 2>/dev/null
    [ -s "$WORK/eo.f" ] && cmp -s "$WORK/eo.f" "$WORK/eo.r" && EO_OK=$((EO_OK+1)) || EO_BAD="$EO_BAD $stem"
done
[ $EO_N -gt 0 ] && [ $EO_OK = $EO_N ] && ctrl C6-EVALUATION-ORDER PASS "at1-model result <case> <record> <fixed provenance> with and without --reversed, cmp" "$EO_OK/$EO_N byte-identical" \
    || ctrl C6-EVALUATION-ORDER FAIL "at1-model --reversed" "identical $EO_OK/$EO_N; differ:$EO_BAD"
# C7: direct verification of each engine-written result (not the evaluator's re-splice)
DV_N=0; DV_OK=0; DV_BAD=""
mkdir -p "$OUT/results/verify"
for f in "$P1"/engine/*.result; do
    [ -f "$f" ] || continue
    stem=$(basename "$f" .result); [ "$stem" = T9-second-run ] && continue
    cls=${stem%%_*}; name=${stem#*_}; cf="$EC/$cls/$name.case"; DV_N=$((DV_N+1))
    "$EVAL" verify --case "$cf" "$f" > "$OUT/results/verify/$stem.txt" 2>&1; vrc=$?
    tv=$(awk -F'\t' -v p="$cls/$name.case" '$1==p {print $9}' "$P1/TABLE.tsv")
    [ $vrc -eq 0 ] && [ "$tv" = PASS ] && DV_OK=$((DV_OK+1)) || DV_BAD="$DV_BAD $stem(rc=$vrc,table=$tv)"
done
[ $DV_N -gt 0 ] && [ $DV_OK = $DV_N ] && ctrl C7-DIRECT-VERIFY PASS "at1-eval verify --case <case> results/pass1/engine/<case>.result" "$DV_OK/$DV_N AT1_VERIFY PASS, each matching the evaluator's TABLE.tsv case verdict" \
    || ctrl C7-DIRECT-VERIFY FAIL "at1-eval verify on engine results" "pass $DV_OK/$DV_N; others:$DV_BAD"

# ---------- 9. real-engine physics mutants (drop V, V on the wrong level) ----------
MLIB="at1_bn.c at1_case.c at1_exact.c at1_numeric.c at1_result.c at1_io.c"
mutant_model() { # name sed-script file -> $WORK/mut/<name>/at1-model
    mn=$1; ms=$2; mf=$3; md="$WORK/mut/$mn"; mkdir -p "$md"
    cp "$AT1"/model/*.c "$AT1"/model/*.h "$md/"
    sed "$ms" "$AT1/model/$mf" > "$md/$mf"
    if cmp -s "$AT1/model/$mf" "$md/$mf"; then echo "mutation $mn did not change $mf" >> "$LOG"; return 1; fi
    ( cd "$md" && $CCBIN $MCFLAGS -I"$S/src" -o at1-model $MLIB "$S/src/sha256.c" at1_main.c -lm ) > "$md/build.log" 2>&1
}
MR="$AT1/evaluator/results/MUTANT_RECEIPT.tsv"
for mn in drop_v wrong_level; do
    case $mn in
      drop_v) MS='s/mat s = kron(&Pj, &Vj); mat_addto(&H, &s);/mat s = kron(\&Pj, \&Vj);/'; AM=M1-drop-v;;
      wrong_level) MS='s/gq_set_rat(&t, &c->v\[j\]\[a\], NULL);/gq_set_rat(\&t, \&c->v[(j + N - 1) % N][a], NULL);/'; AM=M3-wrong-level;;
    esac
    mkdir -p "$OUT/mutants/$mn"
    if mutant_model $mn "$MS" at1_exact.c; then
        sed "s/^engine_sha256 .*/engine_sha256 $(sha "$WORK/mut/$mn/at1-model")/" "$PROV" > "$WORK/prov.mut.$mn"
        awk -F'\t' -v m="$AM" '$1==m {print $2 "\t" (($6 ~ /^BLIND/) ? "BLIND" : "KILLED")}' "$MR" | while IFS="$(printf '\t')" read -r c pred; do
            stem=$(printf '%s' "$c" | sed 's|/|_|; s|\.case$||'); res="$OUT/mutants/$mn/$stem.result"
            AT1_MODEL_BIN="$WORK/mut/$mn/at1-model" AT1_PROV_STATIC="$WORK/prov.mut.$mn" AT1_CC_NOTE="(MUTANT $mn control, not a qualification result)" \
              sh "$INT/engine_shim.sh" "$EC/$c" "$res" 2> "$WORK/mut.err"; rc=$?
            if [ $rc -ne 0 ]; then got=KILLED; how="no result rc=$rc $(head -1 "$WORK/mut.err" | cut -c1-60)"; rm -f "$res"
            else
                "$EVAL" verify --case "$EC/$c" "$res" > "$WORK/mut.v" 2>&1; vrc=$?
                if [ $vrc -eq 0 ] && [ "$(field "$res" expectation_met)" = YES ]; then got=BLIND; else got=KILLED; fi
                how="verify rc=$vrc outcome $(field "$res" outcome) codes $(field "$res" failure_codes) expectation_met $(field "$res" expectation_met)"
            fi
            printf '%s\t%s\t%s\t%s\n' "$c" "$pred" "$got" "$how"
        done > "$R/mutant-$mn.tsv"
    else echo "mutant $mn did not build: $(tail -2 "$WORK/mut/$mn/build.log" 2>/dev/null)" >> "$LOG"; : > "$R/mutant-$mn.tsv"; fi
    MN=$(wc -l < "$R/mutant-$mn.tsv" | tr -d ' '); MK=$(awk -F'\t' '$3=="KILLED"' "$R/mutant-$mn.tsv" | wc -l | tr -d ' ')
    MDIFF=$(awk -F'\t' '$2!=$3 {printf " %s(predicted %s, got %s)", $1, $2, $3}' "$R/mutant-$mn.tsv")
    MBL=$(awk -F'\t' '$3=="BLIND" {printf " %s", $1}' "$R/mutant-$mn.tsv")
    KATK=$(awk -F'\t' '$1 ~ /P2-kat/ {print $3}' "$R/mutant-$mn.tsv")
    [ "$MN" -gt 0 ] && [ -z "$MDIFF" ] && [ "$KATK" = KILLED ] && ctrl "C8-ENGINE-MUTANT-$(echo $mn | tr a-z_ A-Z-)" PASS "copy of model/at1_exact.c with $AM applied ($MS), built with the model flags, every case of Agent 4's $AM receipt rows through the shim and at1-eval verify" "$MK/$MN killed; blind:${MBL:- none}; every cell equals Agent 4's predicted KILLED/BLIND" \
        || ctrl "C8-ENGINE-MUTANT-$(echo $mn | tr a-z_ A-Z-)" FAIL "real-engine $AM mutant" "$MK/$MN killed; KAT $KATK; cells differing from the receipt:${MDIFF:- none}"
done

# ---------- 10. isolation (G2): Agent 4's gate on the candidates' compute objects, plus mutants ----------
ISO="$WORK/iso"; mkdir -p "$ISO/clean" "$ISO/mut"
ISOOK=1
for f in "$AT1"/model/at1_*.c "$S/src/sha256.c"; do
    b=$(basename "$f" .c)
    $CCBIN $MCFLAGS -I"$AT1/model" -I"$S/src" -c "$f" -o "$ISO/clean/$b.o" >> "$LOG" 2>&1 || ISOOK=0
done
GATE="$AT1/evaluator/gates/isolation.sh"
# allowed() obj "sym sym": run the gate on one object; PASS iff every FAIL line is "references <sym>" with sym in the list
allowed() {
    sh "$GATE" "$EVAL" "$1" > "$WORK/al.out" 2>&1
    bad=$(grep '^FAIL' "$WORK/al.out" | while read -r l; do s=$(printf '%s' "$l" | sed -n 's/^FAIL: .* references \([A-Za-z0-9_]*\)$/\1/p'); case " $2 " in *" $s "*) [ -n "$s" ] || echo "$l";; *) echo "$l";; esac; done)
    cat "$WORK/al.out"; [ -z "$bad" ]
}
STRICT="$ISO/clean/at1_case.o $ISO/clean/at1_exact.o $ISO/clean/at1_numeric.o $ISO/clean/at1_result.o $ISO/clean/sha256.o"
sh "$GATE" "$EVAL" $STRICT > "$R/isolation-engine-strict.log" 2>&1; i1=$?
allowed "$ISO/clean/at1_bn.o" "fwrite" > "$R/isolation-engine-bn.log" 2>&1; i2=$?
allowed "$ISO/clean/at1_io.o" "fopen fread" > "$R/isolation-engine-io.log" 2>&1; i3=$?
sh "$GATE" "$EVAL" "$ISO/clean/at1_main.o" > "$R/isolation-engine-main-record-only.log" 2>&1
[ $ISOOK = 1 ] && [ $i1 -eq 0 ] && [ $i2 -eq 0 ] && [ $i3 -eq 0 ] && ctrl C9-ISOLATION-ENGINE PASS "evaluator/gates/isolation.sh (nm symbol scan + at1-eval scan-clock instruction scan) on every engine object compiled with the model flags: at1_case, at1_exact, at1_numeric, at1_result, sha256 strictly; at1_bn with only fwrite exempt (D2); at1_io (the file reader) with only fopen, fread exempt; at1_main (CLI) recorded, not judged" "strict set clean ($(tail -1 "$R/isolation-engine-strict.log")); at1_bn: only fwrite (the out-of-memory diagnostic to stderr); at1_io: only fopen, fread; no instruction hit anywhere" \
    || ctrl C9-ISOLATION-ENGINE FAIL "gates/isolation.sh on engine objects" "build=$ISOOK strict rc=$i1 bn rc=$i2 io rc=$i3: $(grep '^FAIL' "$R"/isolation-engine-strict.log "$R/isolation-engine-bn.log" "$R/isolation-engine-io.log" | head -3 | tr '\n' ' ')"
# the three gate controls of Agent 4, embedded in a real engine compute object
MUTOK=1; MUTMSG=""
for m in hidden_clock_mutant counter_mutant raw_syscall_mutant; do
    cat "$AT1/model/at1_numeric.c" "$AT1/evaluator/gates/$m.c" > "$ISO/mut/at1_numeric_$m.c"
    if $CCBIN -O2 -I"$AT1/model" -I"$S/src" -c "$ISO/mut/at1_numeric_$m.c" -o "$ISO/mut/at1_numeric_$m.o" >> "$LOG" 2>&1; then
        nm "$ISO/mut/at1_numeric_$m.o" 2>/dev/null | grep -q " T _\{0,1\}at1_[a-z_]*phase$" || { MUTOK=0; MUTMSG="$MUTMSG $m:mutant-function-absent"; continue; }
        sh "$GATE" "$EVAL" "$ISO/mut/at1_numeric_$m.o" > "$R/isolation-engine-$m.log" 2>&1; g=$?
        case $m in hidden_clock_mutant) want='references clock_gettime';; *) want='^FAIL: instruction';; esac
        if [ $g -ne 0 ] && grep -q "$want" "$R/isolation-engine-$m.log"; then MUTMSG="$MUTMSG $m:caught"; else MUTOK=0; MUTMSG="$MUTMSG $m:NOT-caught(rc=$g)"; fi
    else MUTOK=0; MUTMSG="$MUTMSG $m:not-built"; fi
done
[ $MUTOK = 1 ] && ctrl C10-ISOLATION-ENGINE-MUTANTS PASS "at1_numeric.c with each of evaluator/gates/{hidden_clock,counter,raw_syscall}_mutant.c appended, compiled -O2, then gates/isolation.sh" "all three flagged:$MUTMSG (clock_gettime by the symbol scan; the inline counter read and inline system call by the instruction scan)" \
    || ctrl C10-ISOLATION-ENGINE-MUTANTS FAIL "engine-object mutants" "$MUTMSG"
# Rust oracle: the compute rlib members strictly, then three Rust mutants
RI="$WORK/riso"; mkdir -p "$RI/clean"
( cd "$AT1/oracle" && sh build.sh lib ) >> "$LOG" 2>&1
( cd "$RI/clean" && ar x "$AT1/oracle/target/libat1.rlib" ) 2>> "$LOG"
sh "$GATE" "$EVAL" "$RI"/clean/*.o > "$R/isolation-oracle.log" 2>&1; r0=$?
[ $r0 -eq 0 ] && ctrl C11-ISOLATION-ORACLE PASS "oracle/build.sh lib; ar x libat1.rlib; gates/isolation.sh on each object member" "$(tail -1 "$R/isolation-oracle.log") (compute crate src/at1 only; main.rs is the I/O layer, excluded by design)" \
    || ctrl C11-ISOLATION-ORACLE FAIL "gates/isolation.sh on oracle rlib members" "$(grep '^FAIL' "$R/isolation-oracle.log" | head -2 | tr '\n' ' ')"
cat > "$RI/std_time.rs" <<'EOF'
#[inline(never)] pub fn hidden_clock() -> u64 { std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0) }
EOF
cat > "$RI/counter.rs" <<'EOF'
#[cfg(target_arch = "aarch64")]
#[inline(never)] pub fn hidden_counter() -> u64 { let t: u64; unsafe { core::arch::asm!("mrs {}, cntvct_el0", out(reg) t); } t }
#[cfg(target_arch = "x86_64")]
#[inline(never)] pub fn hidden_counter() -> u64 { let lo: u32; let hi: u32; unsafe { core::arch::asm!("rdtsc", out("eax") lo, out("edx") hi); } ((hi as u64) << 32) | lo as u64 }
EOF
cat > "$RI/raw_syscall.rs" <<'EOF'
#[cfg(all(target_arch = "aarch64", target_os = "linux"))]
#[inline(never)] pub fn hidden_syscall() -> i64 { let mut ts = [0i64; 2]; let r: i64; unsafe { core::arch::asm!("svc #0", inout("x0") 0i64 => r, in("x1") ts.as_mut_ptr(), in("x8") 113i64); } r + ts[1] }
#[cfg(all(target_arch = "aarch64", target_os = "macos"))]
#[inline(never)] pub fn hidden_syscall() -> i64 { let mut ts = [0i64; 2]; let r: i64; unsafe { core::arch::asm!("svc #0x80", inout("x0") ts.as_mut_ptr() as i64 => r, in("x1") 0i64, in("x16") 116i64); } r + ts[1] }
#[cfg(all(target_arch = "x86_64", target_os = "linux"))]
#[inline(never)] pub fn hidden_syscall() -> i64 { let mut ts = [0i64; 2]; let r: i64; unsafe { core::arch::asm!("syscall", inout("rax") 228i64 => r, in("rdi") 0i64, in("rsi") ts.as_mut_ptr(), out("rcx") _, out("r11") _); } r + ts[1] }
EOF
RFL=$(printf '%s' "$OFLAGS" | sed 's/ -D warnings//')
RMOK=1; RMMSG=""
for m in std_time counter raw_syscall; do
    mkdir -p "$RI/$m/iso"; cp -R "$AT1/oracle/src/at1" "$RI/$m/at1"; printf '\n' >> "$RI/$m/at1/mod.rs"; cat "$RI/$m.rs" >> "$RI/$m/at1/mod.rs"
    if ( cd "$RI/$m" && rustc $RFL --crate-type lib --crate-name at1_mutant at1/mod.rs -o lib.rlib && cd iso && ar x ../lib.rlib ) >> "$LOG" 2>&1; then
        nm "$RI/$m"/iso/*.o 2>/dev/null | grep -q " T .*hidden_" || { RMOK=0; RMMSG="$RMMSG $m:mutant-function-absent"; continue; }
        sh "$GATE" "$EVAL" "$RI/$m"/iso/*.o > "$R/isolation-oracle-mutant-$m.log" 2>&1; g=$?
        case $m in std_time) want='Rust std time';; *) want='^FAIL: instruction';; esac
        if [ $g -ne 0 ] && grep -q "$want" "$R/isolation-oracle-mutant-$m.log"; then RMMSG="$RMMSG $m:caught"; else RMOK=0; RMMSG="$RMMSG $m:NOT-caught(rc=$g)"; fi
    else RMOK=0; RMMSG="$RMMSG $m:not-built"; fi
done
[ $RMOK = 1 ] && ctrl C12-ISOLATION-ORACLE-MUTANTS PASS "copy of oracle src/at1 with a std::time read, an inline asm counter read, an inline asm clock system call appended; rlib members through gates/isolation.sh" "all three flagged:$RMMSG" \
    || ctrl C12-ISOLATION-ORACLE-MUTANTS FAIL "Rust oracle mutants" "$RMMSG"
GC=$(grep -c '^AT1_ISOLATION_CONTROLS: PASS$' "$R/evaluator-isolation-controls.txt" 2>/dev/null)
[ "$GC" = 1 ] && ctrl C13-GATE-CONTROLS PASS "evaluator make test: gates/isolation.sh --controls (clean, hidden clock, counter, raw syscall)" "$(grep -c AS_INTENDED "$R/evaluator-isolation-controls.txt") rows AS_INTENDED; symbol scan alone blind to the two inline mutants, as designed" \
    || ctrl C13-GATE-CONTROLS FAIL "gates/isolation.sh --controls" "see receipts/evaluator-isolation-controls.txt"

# ---------- 11. make hygiene (G2) ----------
if [ -f "$S/mk/at1.mk" ]; then
    HY="$WORK/hyg"; mkdir -p "$HY"
    MKV="PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0"
    hy() { # tag dir
        D=$2
        for g in all test; do
            ( cd "$D" && make -n -k $g $MKV ) > "$HY/$1.$g" 2>&1; echo "exit $?" >> "$HY/$1.$g"
            ( cd "$D" && make -n -k $g ) > "$HY/$1.$g.default" 2>&1; echo "exit $?" >> "$HY/$1.$g.default"
        done
        ( cd "$D" && make -pn -k all $MKV ) 2>/dev/null | grep -v '^#' | sed -E 's/[0-9a-f]{12,40}/SHA/g; s/ at1-check at1-clean//; s| mk/at1\.mk||g' \
          | grep -v -E '^(AT1_[A-Z_]* |at1-(check|clean):|MAKEFILE_LIST|MA2_RUN_ID|R16_STAMP)' | grep -v -E "^${TAB}.*(AT1_|integration/run\\.sh|build/at1)" | LC_ALL=C sort -u > "$HY/$1.db"
    }
    # "without": a second clone with mk/at1.mk removed and committed, so both trees are clean
    S2="$WORK/src-without"
    git clone -q --no-hardlinks "$S" "$S2" && git -C "$S2" rm -q mk/at1.mk && git -C "$S2" -c user.email=a5@example.invalid -c user.name=a5 commit -q -m "without at1.mk (control)"
    # warm both trees once (first make in a fresh clone may generate files), then compare
    ( cd "$S" && make -n -k all $MKV ) > /dev/null 2>&1; ( cd "$S2" && make -n -k all $MKV ) > /dev/null 2>&1
    hy with "$S"; hy without "$S2"
    hok=1; hmsg=""
    for g in all test; do for v in "" ".default"; do cmp -s "$HY/with.$g$v" "$HY/without.$g$v" || { hok=0; hmsg="$hmsg $g$v"; }; done; done
    for g in all test; do cp "$HY/with.$g" "$R/make-n-$g.with.txt"; cp "$HY/without.$g" "$R/make-n-$g.without.txt"; done
    diff "$HY/without.db" "$HY/with.db" > "$R/make-database.diff"
    extra=$(grep '^[<>]' "$R/make-database.diff" | grep -v -i 'at1' | wc -l | tr -d ' ')
    [ $hok = 1 ] && ctrl C14-MAKE-N-HYGIENE PASS "make -n -k all|test with $MKV and with defaults, fragment present vs removed-and-committed clone (cmp of stdout+stderr+exit)" "byte-identical in all four comparisons (all: $(wc -l < "$HY/with.all" | tr -d ' ') lines, $(tail -1 "$HY/with.all"); test: $(wc -l < "$HY/with.test" | tr -d ' ') lines, $(tail -1 "$HY/with.test"))" \
        || ctrl C14-MAKE-N-HYGIENE FAIL "make -n -k all|test with and without mk/at1.mk" "differ:$hmsg"
    [ "$extra" = 0 ] && ctrl C15-MAKE-DATABASE PASS "make -pn -k all, sorted, fragment present vs removed; diff after dropping AT1_/at1 names, the mk/at1.mk entry in MAKEFILE_LIST and the .PHONY list, 12-40 hex stamps" "empty diff outside at1 names: no variable, rule, target or prerequisite of the existing build changes (SRCS, all, test, clean untouched)" \
        || ctrl C15-MAKE-DATABASE FAIL "make -pn database diff" "$extra changed lines outside at1 names; see make-database.diff"
    ( cd "$S" && make -n at1-check $MKV ) > "$R/make-n-at1-check.txt" 2>&1; pc=$?
    ( cd "$S" && make -n at1-clean $MKV ) > "$R/make-n-at1-clean.txt" 2>&1; pc2=$?
    ( cd "$S" && make -n --warn-undefined-variables at1-check at1-clean $MKV ) > "$R/make-warn-undefined.txt" 2>&1
    wu=$(grep -i 'undefined variable' "$R/make-warn-undefined.txt" | grep -c 'AT1_')
    [ $pc -eq 0 ] && [ $pc2 -eq 0 ] && [ "$wu" = 0 ] && grep -q 'integration/run.sh' "$R/make-n-at1-check.txt" && ctrl C16-MAKE-PARSES PASS "make -n at1-check; make -n at1-clean; make -n --warn-undefined-variables (all with $MKV)" "both plan: $(grep -m1 run.sh "$R/make-n-at1-check.txt" | cut -c1-90); no undefined AT1_ variable" \
        || ctrl C16-MAKE-PARSES FAIL "make -n at1-check at1-clean" "rc=$pc/$pc2 undefined-AT1-warnings=$wu"
else
    ctrl C14-MAKE-N-HYGIENE NOT_RUN "mk/at1.mk not committed at HEAD" "fragment absent"; ctrl C15-MAKE-DATABASE NOT_RUN "-" "fragment absent"; ctrl C16-MAKE-PARSES NOT_RUN "-" "fragment absent"
fi

# ---------- 12. gates ----------
cp "$P1/TABLE.tsv" "$R/TABLE.pass1.tsv" 2>/dev/null; cp "$P1/VERDICT_IDS.tsv" "$R/VERDICT_IDS.pass1.tsv" 2>/dev/null
eg() { grep "^# $1 " "$P1/TABLE.tsv" 2>/dev/null | head -1 | sed 's/.*: \([A-Z_]*\)$/\1/'; }   # read a gate verdict off the evaluator's table
cnt() { grep "^# $1: " "$P1/TABLE.tsv" 2>/dev/null | head -1 | sed 's/^# [a-z]*: //'; }
G1E=$(eg AT1-G1); G3E=$(eg AT1-G3); G4E=$(eg AT1-G4); G5E=$(eg AT1-G5)
T9L=$(grep '^# T9 ' "$P1/TABLE.tsv" 2>/dev/null | sed 's/^# T9 [^:]*: //' | cut -d' ' -f1)
G1BAD=$(awk -F'\t' '!/^#/ && $3!="AT1_CASE_OK" && $9!="PASS" {printf " %s(engine: %s; oracle: %s)", $1, $5, $6}' "$P1/TABLE.tsv" 2>/dev/null)
gate AT1-G0 REFERENCE "AT0_FREEZE.md AT-1 rows, AT1_CHARTER.md section 11; control C0" "freeze recorded by Agent 0 (aien-architecture cbe4c8e, spec 81047f5); this run: C0-CONTRACT-DIGESTS $(cst C0-CONTRACT-DIGESTS)"
gate AT1-G1 "${G1E:-NOT_RUN}" "evaluator TABLE.tsv line AT1-G1 (evaluator codec, engine and oracle refusals exact on every refuse row; KAT digests copied)" "refusal rows: $(cnt refusal); failing rows:${G1BAD:- none}"
isoG=PASS
for k in C9-ISOLATION-ENGINE C10-ISOLATION-ENGINE-MUTANTS C11-ISOLATION-ORACLE C12-ISOLATION-ORACLE-MUTANTS C13-GATE-CONTROLS C14-MAKE-N-HYGIENE C15-MAKE-DATABASE C16-MAKE-PARSES C1-ORACLE-OWN-ISOLATION; do
    st=$(cst $k); case "$st" in PASS) ;; FAIL) isoG=FAIL;; *) [ $isoG = FAIL ] || isoG=NOT_RUN;; esac
done
gate AT1-G2 "$isoG" "controls C9-C16 and C1-ORACLE-OWN-ISOLATION (Agent 4's gates/isolation.sh on engine and oracle compute objects, engine-object and Rust mutants for clock symbol, counter read and raw system call; gate controls; make hygiene)" "symbol + instruction scans of compute objects only; the evaluator is UNISOLATED (same host, same account); what the scans cannot see is listed in QUALIFICATION.md"
gate AT1-G3 "${G3E:-NOT_RUN}" "evaluator TABLE.tsv line AT1-G3 (every oracle record against the evaluator's exact shadow)" "oracle records judged on $((NPOS+NNEG)) valid cases"
gate AT1-G4 "${G4E:-NOT_RUN}" "evaluator TABLE.tsv line AT1-G4 (every POSITIVE case verifies PASS)" "positive: $(cnt positive)"
gate AT1-G5 "${G5E:-NOT_RUN}" "evaluator TABLE.tsv line AT1-G5 (every NEGATIVE case FAILs with exactly its codes) and evaluator run.sh mutant receipt cmp" "negative: $(cnt negative); $(grep -m1 '^MUTANT_RECEIPT' "$R/evaluator-run-pass1.log")"
# G6 is read off the evaluator's own lines: its self-checks (corpus-check, selftest, mutant receipt),
# every candidate result verified (G4 and G5 lines) and T9. The G1 refusal rows are G1's business: when
# only G1 fails, run.sh exits 1 for that reason alone and G6 says so instead of counting it twice.
EL="$R/evaluator-run-pass1.log"
E_CORP=$(grep -c '^corpus-check: [0-9]* files, 0 differ$' "$EL"); E_SELF=$(grep -c '^SELFTEST: PASS ' "$EL"); E_MUT=$(grep -c '^MUTANT_RECEIPT: PASS ' "$EL")
if [ -z "$T9L" ] || [ -z "$G4E" ]; then G6=NOT_RUN
elif [ "$E_CORP" = 1 ] && [ "$E_SELF" = 1 ] && [ "$E_MUT" = 1 ] && [ "$G4E" = PASS ] && [ "$G5E" = PASS ] && [ "$G3E" = PASS ] && [ "$T9L" = PASS ]; then G6=PASS
else G6=FAIL; fi
G6WHY="run.sh rc=$ev1"; [ $ev1 -ne 0 ] && [ "$G6" = PASS ] && [ "$G1E" = FAIL ] && G6WHY="run.sh rc=$ev1 caused only by the AT1-G1 row(s) above"
gate AT1-G6 "$G6" "evaluator lines on this host: corpus-check 0 differ, SELFTEST PASS, MUTANT_RECEIPT PASS (cmp with the committed receipt), AT1-G3, AT1-G4, AT1-G5 PASS, T9 PASS" "$G6WHY; $(grep -m1 '^SELFTEST' "$EL"); $(grep -m1 '^corpus-check' "$EL"); T9 $T9L; the evaluator ran on the same host and account as the candidates (UNISOLATED)"
gate AT1-G7 NOT_RUN "-" "Agent 7 (replication on the MacBook) owns G7"
gate AT1-G8 NOT_RUN "-" "Agent 6 (scientific review) owns G8"

# receipts name the temporary work directory as <work>, so two runs' receipts compare without noise
for f in "$R"/*.log "$R"/*.txt "$R"/*.tsv "$LOG"; do
    [ -f "$f" ] || continue
    sed "s|$WORK|<work>|g" "$f" > "$WORK/scrub.tmp" && cat "$WORK/scrub.tmp" > "$f"
done
# ---------- 13. result digests and summary ----------
( cd "$OUT" && find results mutants components -type f | LC_ALL=C sort | while read -r f; do printf '%s  %s\n' "$(sha "$f")" "$f"; done ) > "$R/outputs.sha256"
NRES=$(ls "$P1"/engine/*.result 2>/dev/null | grep -vc T9-second-run)
{
  echo "omega_commit $HEAD"; echo "source_tree_clean $CLEAN"; echo "contract_commit $CONTRACT_COMMIT"; echo "stamp $STAMP"; echo "host $HOSTT"
  echo "cases $NCASE: positive $NPOS, negative $NNEG, refuse $NREF; engine results written $NRES"
  echo "evaluator table: positive $(cnt positive); negative $(cnt negative); refusal $(cnt refusal)"
  echo "mutants: drop_v $(awk -F'\t' '$3=="KILLED"' "$R/mutant-drop_v.tsv" | wc -l | tr -d ' ')/$(wc -l < "$R/mutant-drop_v.tsv" | tr -d ' ') killed; wrong_level $(awk -F'\t' '$3=="KILLED"' "$R/mutant-wrong_level.tsv" | wc -l | tr -d ' ')/$(wc -l < "$R/mutant-wrong_level.tsv" | tr -d ' ') killed; evaluator receipt $(grep -m1 '^MUTANT_RECEIPT' "$R/evaluator-run-pass1.log")"
} > "$R/summary.txt"
cat "$R/summary.txt" | tee -a "$LOG"
NFAIL=$(awk -F'\t' '$2=="FAIL"' "$GATES" | wc -l | tr -d ' '); NCF=$(awk -F'\t' '$2=="FAIL"' "$CTRL" | wc -l | tr -d ' ')
say "gates: $(awk -F'\t' '{printf "%s=%s ", $1, $2}' "$GATES")"
say "controls failing: $NCF; gates failing: $NFAIL"
[ "$NFAIL" -eq 0 ] && [ "$NCF" -eq 0 ]
