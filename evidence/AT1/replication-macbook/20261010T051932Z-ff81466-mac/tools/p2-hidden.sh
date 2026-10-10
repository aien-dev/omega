#!/bin/bash
# AT-1 Agent 7 phase 2: hidden cases through engine, oracle and evaluator (detached; never killed)
export LC_ALL=C TZ=UTC
W=$HOME/at1-replication/run-ff814662a19c
H=$HOME/at1-hidden
LOG=$W/receipt-commands.log
cd "$W"
r() { echo "\$ $*" >> "$LOG"; "$@"; rc=$?; echo "exit $rc" >> "$LOG"; return $rc; }
# 0. the hidden set still equals the posted commitment
( cd "$H/cases" && for f in $(ls | sort); do shasum -a 256 "$f"; done ) > hidden-manifest.recomputed
cmp hidden-manifest.recomputed "$H/MANIFEST" && echo "manifest-unchanged" >> "$LOG"
echo "commitment $(shasum -a 256 "$H/MANIFEST" | cut -c1-64)" >> "$LOG"
# 1. separate fresh clone of the freeze commit for the hidden-run binaries
rm -rf hsrc
r git clone -q --no-hardlinks omega hsrc
r git -C hsrc checkout -q --detach ff814662a19cf7da6b183f3e667c6c921e0ead21
AT1=$W/hsrc/research/atemporal/at1
( cd "$AT1/oracle" && r sh build.sh ) > hidden-build-oracle.log 2>&1
( cd "$AT1/model" && r make CC=cc ) > hidden-build-model.log 2>&1
( cd "$AT1/evaluator" && r make all ) > hidden-build-evaluator.log 2>&1
echo "hsrc status lines $(git -C hsrc status --porcelain | wc -l | tr -d ' ')" >> "$LOG"
MODEL=$AT1/model/build/at1-model; ORACLE=$AT1/oracle/target/at1-oracle; EVAL=$AT1/evaluator/build/at1-eval
shasum -a 256 "$MODEL" "$ORACLE" "$EVAL" > hidden-binaries.sha256
# 2. case folder: the 21 hidden files by class, plus the public worked example (KAT, T9 probe)
C=$W/hidden-run/cases; rm -rf "$W/hidden-run"; mkdir -p "$C/positive" "$C/negative" "$C/refuse"
printf '# path\tclass\texpected tool response\n' > "$C/MANIFEST.tsv"
cp "$AT1/evaluator/cases/positive/P2-kat-rotated-level-n4.case" "$C/positive/"
printf 'positive/P2-kat-rotated-level-n4.case\tP2\tAT1_CASE_OK\n' >> "$C/MANIFEST.tsv"
tail -n +2 "$H/cases/EXPECTED.tsv" | while IFS="$(printf '\t')" read -r f cls kind out codes chk cid aid att; do
  case "$cls" in P*) sub=positive;; N*) sub=negative;; *) sub=refuse;; esac
  cp "$H/cases/$f" "$C/$sub/$f"
  if [ "$kind" = REFUSED ]; then exp="AT1_CASE_REFUSED $out"; else exp=AT1_CASE_OK; fi
  printf '%s/%s\t%s\t%s\n' "$sub" "$f" "$cls" "$exp" >> "$C/MANIFEST.tsv"
done
# 3. provenance for the engine shim (same lines run.sh writes; host names the Mac)
CLEAN=YES; [ -n "$(git -C hsrc status --porcelain)" ] && CLEAN=NO
P=$W/hidden-run/prov.static
{ echo "source_repo aien-dev/omega"; echo "source_commit ff814662a19cf7da6b183f3e667c6c921e0ead21"; echo "source_tree_clean $CLEAN"
  echo "contract_commit cbe4c8ed88d28bb96d5209327ecd30aa9cddaf7a"; echo "engine_sha256 $(shasum -a 256 "$MODEL" | cut -c1-64)"
  echo "oracle_repo aien-dev/omega"; echo "oracle_commit ff814662a19cf7da6b183f3e667c6c921e0ead21"; echo "oracle_sha256 $(shasum -a 256 "$ORACLE" | cut -c1-64)"
  echo "build_cc $(cc --version | head -1) (engine); $(rustc --version) (oracle)"
  echo "build_flags engine: model/Makefile CFLAGS -lm; oracle: oracle/build.sh"
  echo "host $(hostname) $(sw_vers -productName) $(sw_vers -productVersion) $(uname -m)"; } > "$P"
export AT1_MODEL_BIN=$MODEL AT1_ORACLE_BIN=$ORACLE AT1_PROV_STATIC=$P
INT=$AT1/integration
# 4. the evaluator's own qualify over the hidden folder
cd "$AT1/evaluator"
r ./build/at1-eval qualify --cases "$C" --engine "sh $INT/engine_shim.sh {case} {out}" --oracle "sh $INT/oracle_shim.sh {case} {out}" --out "$W/hidden-run/out" > "$W/hidden-run/qualify.console.log" 2>&1
echo $? > "$W/hidden-run/qualify.exit"
# 5. values against the committed exact tables
cd "$W"
r "$H/phase2/cmpvals" "$H/cases/DETAILS.txt" "$W/hidden-run/out/spliced" 1e-12 > "$W/hidden-run/VALUES_VS_DETAILS.tsv" 2>&1
echo $? > "$W/hidden-run/cmpvals.exit"
echo done > "$W/hidden-run/DONE"
