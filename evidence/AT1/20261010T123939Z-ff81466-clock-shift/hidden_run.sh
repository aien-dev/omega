#!/bin/sh
# Agent 0 (session 85679b2b), AT-1 G8 follow-up items 2 and 4: run Agent 4's revealed 27-case withheld set.
#   sh hidden_run.sh <clone-root at ff81466> <hidden-dir (.case files only)> <out-dir>
# Mirrors integration/run.sh sections 3-4 for the provenance file, then the evaluator's own run.sh
# with the two integration shims (public corpus, then the hidden set). POSIX sh; Linux and macOS.
# AT1_CLOCK_WRAP (optional): command prefix for the qualification step only, e.g.
#   AT1_CLOCK_WRAP='faketime 2028-10-10T12:00:00'  (item 4 shifted-clock control).
# The provenance and toolchain queries stay on the real clock: `rustc --version` deadlocks under libfaketime.
set -eu
ROOT=$(cd "$1" && pwd -P); HID=$(cd "$2" && pwd -P); OUT=$3
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd -P)
AT1=$ROOT/research/atemporal/at1; INT=$AT1/integration
HEAD=$(git -C "$ROOT" rev-parse HEAD)
CLEAN=YES; [ -n "$(git -C "$ROOT" status --porcelain)" ] && CLEAN=NO
if command -v sha256sum > /dev/null 2>&1; then SHATOOL="sha256sum"; else SHATOOL="shasum -a 256"; fi
sha() { $SHATOOL "$1" | cut -c1-64; }
LOCK="$INT/contract.lock"; lock() { sed -n "s/^$1 //p" "$LOCK" | head -1; }
CONTRACT_COMMIT=$(lock contract_commit)
MODEL="$AT1/model/build/at1-model"; ORACLE="$AT1/oracle/target/at1-oracle"; EVAL="$AT1/evaluator/build/at1-eval"
CCBIN=${CC:-cc}; CC_V=$($CCBIN --version 2>&1 | head -1); RUSTC_V=$(rustc --version 2>&1)
HOSTN=$(cat /etc/hostname 2>/dev/null || uname -n)
OSDESC=$(uname -srm); command -v sw_vers > /dev/null 2>&1 && OSDESC="$(sw_vers -productName) $(sw_vers -productVersion) $(uname -m)"
MCFLAGS=$(sed -n 's/^CFLAGS = //p' "$AT1/model/Makefile" | head -1)
OFLAGS=$(sed -n 's/^FLAGS="\(.*\)"$/\1/p' "$AT1/oracle/build.sh" | head -1)
ptext() { printf '%s' "$1" | tr -cd ' -~' | tr -s ' ' | sed 's/^ //; s/ $//' | cut -c1-200 | sed 's/ $//'; }
ENGINE_SHA=$(sha "$MODEL"); ORACLE_SHA=$(sha "$ORACLE")
BCC=$(ptext "$CC_V (engine); $RUSTC_V (oracle)"); BFL=$(ptext "engine: $MCFLAGS -lm; oracle: rustc $OFLAGS"); HOSTT=$(ptext "$HOSTN $OSDESC")
PROV="$OUT/provenance.static"
{ echo "source_repo aien-dev/omega"; echo "source_commit $HEAD"; echo "source_tree_clean $CLEAN"; echo "contract_commit $CONTRACT_COMMIT"
  echo "engine_sha256 $ENGINE_SHA"; echo "oracle_repo aien-dev/omega"; echo "oracle_commit $HEAD"; echo "oracle_sha256 $ORACLE_SHA"
  echo "build_cc $BCC"; echo "build_flags $BFL"; echo "host $HOSTT"; } > "$PROV"
{ echo "date_utc $(date -u +%Y-%m-%dT%H:%M:%SZ)"; echo "uname $(uname -srm)"; echo "host $HOSTN"; echo "os $OSDESC"
  echo "cc ($CCBIN) $CC_V"; echo "make $(make --version 2>&1 | head -1)"; echo "sh $(ls -l /bin/sh 2>&1)"; echo "sha tool $SHATOOL"
  echo "git $(git --version)"; [ -n "${AT1_CLOCK_WRAP:-}" ] && echo "clock_wrap $AT1_CLOCK_WRAP"; echo "--- rustc -vV"; rustc -vV 2>&1; } > "$OUT/toolchain.txt"
{ echo "$ENGINE_SHA  at1-model"; echo "$ORACLE_SHA  at1-oracle"; echo "$(sha "$EVAL")  at1-eval"; } > "$OUT/binaries.sha256"
( cd "$HID" && ls *.case | LC_ALL=C sort | while read -r f; do printf '%s  %s\n' "$(sha "$f")" "$f"; done ) > "$OUT/HIDDEN_MANIFEST.txt"
echo "commitment $($SHATOOL "$OUT/HIDDEN_MANIFEST.txt" | cut -c1-64)" > "$OUT/HIDDEN_COMMITMENT_RECOMPUTED.txt"
cat "$OUT/HIDDEN_COMMITMENT_RECOMPUTED.txt"
export AT1_MODEL_BIN="$MODEL" AT1_ORACLE_BIN="$ORACLE" AT1_PROV_STATIC="$PROV" AT1_COMP_DIR="$OUT/components"
cd "$AT1/evaluator"
rc=0
${AT1_CLOCK_WRAP:-} sh run.sh "sh $INT/engine_shim.sh {case} {out}" "sh $INT/oracle_shim.sh {case} {out}" cases "$OUT/qualify" "$HID" > "$OUT/run.console.log" 2>&1 || rc=$?
echo "$rc" > "$OUT/run.exit"
echo "evaluator run.sh rc=$rc (omega $HEAD, host $HOSTN${AT1_CLOCK_WRAP:+, clock wrap: $AT1_CLOCK_WRAP})"
