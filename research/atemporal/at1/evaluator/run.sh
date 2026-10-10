#!/bin/sh
# AT-1 Agent 4 qualification run (POSIX sh; Linux and macOS).
#   sh run.sh '<engine command>' '<oracle command>' [cases-dir] [out-dir] [hidden-dir]
# Each command is a template: {case} is replaced by the case file path and
# {out} by the result file to write (without {out}, standard output is used).
# Example: sh run.sh '../model/build/at1-engine {case} {out}' '../oracle/build/at1-oracle {case} {out}'
# out-dir defaults to build/qualify (git-ignored), so a run leaves the tree clean.
# hidden-dir (only after the candidate freeze): the revealed hidden .case files and
# nothing else; their commitment must equal HIDDEN_COMMITMENT.txt before they are run.
# Exit 0 only when every step passes.
set -eu
cd "$(dirname "$0")"
ENGINE=${1:?engine command template required}
ORACLE=${2:?oracle command template required}
CASES=${3:-cases}
OUT=${4:-build/qualify}
HIDDEN=${5:-}
make -s all
mkdir -p build
./build/at1-eval corpus-check cases
./build/at1-eval selftest > build/selftest.txt || { cat build/selftest.txt; echo "SELFTEST FAILED" >&2; exit 1; }
tail -n 1 build/selftest.txt
./build/at1-eval mutants build/MUTANT_RECEIPT.tsv
cmp build/MUTANT_RECEIPT.tsv results/MUTANT_RECEIPT.tsv
status=0
./build/at1-eval qualify --cases "$CASES" --engine "$ENGINE" --oracle "$ORACLE" --out "$OUT" || status=1
if [ -n "$HIDDEN" ]; then
  if [ -e "$HIDDEN/MANIFEST.tsv" ]; then
    echo "HIDDEN SET REFUSED: $HIDDEN/MANIFEST.tsv exists; the commitment covers only the .case files" >&2
    exit 1
  fi
  want=$(sed -n 's/^commitment //p' HIDDEN_COMMITMENT.txt)
  got=$(./build/at1-eval hidden-commit "$HIDDEN" | sed -n 's/^commitment //p')
  if [ -z "$got" ] || [ "$want" != "$got" ]; then
    echo "HIDDEN COMMITMENT MISMATCH: committed $want, revealed ${got:-none}" >&2
    exit 1
  fi
  echo "hidden commitment matches: $got"
  ./build/at1-eval qualify --cases "$HIDDEN" --engine "$ENGINE" --oracle "$ORACLE" --out "$OUT-hidden" || status=1
fi
exit $status
