#!/bin/sh
# OSC-0B exit gate receipt (OMEGA_SYSTEMS_CORE_CODE_AUDIT.md II.11, III.8).
#  1. refuses a dirty tree (any tracked or untracked change): receipts record an exact commit
#  2. builds tests/compiler/test_osc_model.c + src/compiler/model/*.c twice, outside the tree:
#     plain -O2, and ASan/UBSan (-fno-sanitize-recover=all)
#  3. runs the plain build at the gate size (>= 10^6 seeded sequences) and the
#     sanitizer build on the same seed (full size unless OSC0B_ASAN_SEQUENCES is set)
#  4. requires OSC0B_MODEL_PASS, zero false accepts / wrong names / false rejects,
#     and the same sweep digest from both builds on the full size
#  5. writes a content-addressed receipt evidence/OSC-1/receipts/osc0b-model-<sha256>.json
# Never overwrites existing evidence. Timing is not recorded (receipt stays content-stable).
# Env: CC (default cc), OSC0B_SEED (hex, default the test's built-in seed), OSC0B_ASAN_SEQUENCES.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
cd "$ROOT"
EVD=evidence/OSC-1/receipts
CC=${CC:-cc}
SRCS="src/compiler/model/osc_model.h src/compiler/model/osc_model.c src/compiler/model/osc_model_gen.h src/compiler/model/osc_model_gen.c tests/compiler/test_osc_model.c"
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc -Isrc/compiler -Isrc/compiler/model"

die() { echo "osc0b-model-receipt: REFUSED: $*" >&2; exit 2; }

[ -z "$(git status --porcelain --untracked-files=all)" ] || die "dirty tree; commit first (receipts record an exact commit)"
COMMIT=$(git rev-parse HEAD)
case "$COMMIT" in *[!0-9a-f]*|"") die "cannot read HEAD";; esac
for f in $SRCS; do [ -f "$f" ] || die "missing $f"; done

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
nice "$CC" $CFLAGS -O2 -o "$T/plain" tests/compiler/test_osc_model.c src/compiler/model/osc_model.c src/compiler/model/osc_model_gen.c \
    >"$T/build.log" 2>&1 || { tail -20 "$T/build.log"; die "plain build failed"; }
nice "$CC" $CFLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -o "$T/asan" \
    tests/compiler/test_osc_model.c src/compiler/model/osc_model.c src/compiler/model/osc_model_gen.c \
    >"$T/build_asan.log" 2>&1 || { tail -20 "$T/build_asan.log"; die "sanitizer build failed"; }

SEED_ARG=${OSC0B_SEED:-}
if [ -n "$SEED_ARG" ]; then
    nice "$T/plain" "$SEED_ARG" >"$T/plain.out" 2>&1 || { tail -20 "$T/plain.out"; die "plain run failed"; }
else
    nice "$T/plain" >"$T/plain.out" 2>&1 || { tail -20 "$T/plain.out"; die "plain run failed"; }
fi
[ "$(tail -1 "$T/plain.out")" = OSC0B_MODEL_PASS ] || { tail -20 "$T/plain.out"; die "plain run did not print OSC0B_MODEL_PASS"; }

SUM=$(grep '^OSC0B_MODEL_SEED=' "$T/plain.out" | tail -1)
field() { echo "$SUM" | sed -n "s/.*$1=\([0-9a-fx]*\).*/\1/p"; }
SEED=$(field OSC0B_MODEL_SEED); SEQ=$(field SEQUENCES); STEPS=$(field STEPS)
FA=$(field FALSE_ACCEPTS); WN=$(field WRONG_NAME); FR=$(field FALSE_REJECTS)
for v in "$SEED" "$SEQ" "$STEPS" "$FA" "$WN" "$FR"; do [ -n "$v" ] || die "cannot parse summary line: $SUM"; done
[ "$FA" = 0 ] && [ "$WN" = 0 ] && [ "$FR" = 0 ] || die "nonzero false accepts/wrong names/false rejects"
[ "$SEQ" -ge 1000000 ] || die "fewer than 10^6 sequences ($SEQ)"
VLINE=$(grep '^OSC0B_MODEL_VALID ' "$T/plain.out" | tail -1)
VALID=$(echo "$VLINE" | sed -n 's/.* sequences=\([0-9]*\).*/\1/p')
DIGEST=$(echo "$VLINE" | sed -n 's/.* digest=\([0-9a-f]*\).*/\1/p')
UNIT=$(grep '^OSC0B_MODEL_UNIT ' "$T/plain.out" | tail -1)
UCASES=$(echo "$UNIT" | sed -n 's/.*cases=\([0-9]*\).*/\1/p')
UFAIL=$(echo "$UNIT" | sed -n 's/.*failed=\([0-9]*\).*/\1/p')
[ -n "$VALID" ] && [ -n "$DIGEST" ] && [ -n "$UCASES" ] && [ "$UFAIL" = 0 ] || die "cannot parse valid/unit lines"

ASEQ=${OSC0B_ASAN_SEQUENCES:-$SEQ}
nice "$T/asan" "$SEED" "$ASEQ" >"$T/asan.out" 2>&1 || { tail -20 "$T/asan.out"; die "sanitizer run failed"; }
ALAST=$(tail -1 "$T/asan.out")
case "$ALAST" in OSC0B_MODEL_PASS|OSC0B_MODEL_SMOKE_PASS) ;; *) tail -20 "$T/asan.out"; die "sanitizer run: $ALAST";; esac
ADIGEST=$(grep '^OSC0B_MODEL_VALID ' "$T/asan.out" | sed -n 's/.* digest=\([0-9a-f]*\).*/\1/p')
if [ "$ASEQ" = "$SEQ" ]; then
    [ "$ADIGEST" = "$DIGEST" ] || die "sanitizer sweep digest $ADIGEST != plain $DIGEST"
    DIGEST_MATCH=true
else
    DIGEST_MATCH=null
fi

R="$T/receipt.json"
{
  printf '{\n'
  printf '  "schema": "omega.osc0b.model.receipt.v1",\n'
  printf '  "verdict": "PASS",\n'
  printf '  "gate": "OSC-0B exit: II.11 model rejects every named invalid transition across >= 10^6 seeded random sequences, zero false accepts",\n'
  printf '  "repo_commit": "%s",\n' "$COMMIT"
  printf '  "tree_clean": true,\n'
  printf '  "host_arch": "%s",\n' "$(uname -m)"
  printf '  "compiler": "%s",\n' "$("$CC" --version 2>/dev/null | head -1 | sed 's/"/\\"/g')"
  printf '  "seed": "0x%s",\n' "$SEED"
  printf '  "sequences": %s,\n' "$SEQ"
  printf '  "valid_sequences": %s,\n' "$VALID"
  printf '  "steps": %s,\n' "$STEPS"
  printf '  "false_accepts": %s,\n' "$FA"
  printf '  "wrong_name": %s,\n' "$WN"
  printf '  "false_rejects": %s,\n' "$FR"
  printf '  "sweep_digest": "%s",\n' "$DIGEST"
  printf '  "unit_cases": %s,\n' "$UCASES"
  printf '  "unit_failed": 0,\n'
  printf '  "classes": {\n'
  N=$(grep -c '^OSC0B_MODEL_CLASS ' "$T/plain.out")
  [ "$N" = 13 ] || die "expected 13 class lines, got $N"
  i=0
  grep '^OSC0B_MODEL_CLASS ' "$T/plain.out" | while read -r _ name inj cor wro acc; do
      i=$((i + 1))
      sep=,; [ "$i" = 13 ] && sep=
      printf '    "%s": {"injected": %s, "rejected_correct": %s, "rejected_wrong_name": %s, "accepted": %s}%s\n' \
          "$name" "${inj#*=}" "${cor#*=}" "${wro#*=}" "${acc#*=}" "$sep"
  done
  printf '  },\n'
  printf '  "sanitizer_run": {"flags": "-fsanitize=address,undefined -fno-sanitize-recover=all", "sequences": %s, "result": "%s", "digest_matches_plain": %s},\n' \
      "$ASEQ" "$ALAST" "$DIGEST_MATCH"
  printf '  "sources_sha256": {\n'
  n=0; total=$(echo $SRCS | wc -w)
  for f in $SRCS; do
      n=$((n + 1)); sep=,; [ "$n" = "$total" ] && sep=
      printf '    "%s": "%s"%s\n' "$f" "$(sha256sum "$f" | cut -d' ' -f1)" "$sep"
  done
  printf '  },\n'
  printf '  "timing_measured": false\n'
  printf '}\n'
} >"$R"
# every class must meet the per-class floor with zero misses
grep '^OSC0B_MODEL_CLASS ' "$T/plain.out" | while read -r _ name inj cor wro acc; do
    [ "${inj#*=}" -ge 10000 ] && [ "${cor#*=}" = "${inj#*=}" ] || { echo "class $name below floor or missed" >&2; exit 1; }
done || die "per-class floor"

mkdir -p "$EVD"
RSHA=$(sha256sum "$R" | cut -d' ' -f1)
OUT="$EVD/osc0b-model-$RSHA.json"
[ -e "$OUT" ] || cp "$R" "$OUT"
echo "osc0b-model-receipt: PASS seed=0x$SEED sequences=$SEQ steps=$STEPS receipt=$OUT"
