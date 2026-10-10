#!/bin/sh
# Build the AT-0 candidate engine and its tests (plain C11, -lm only).
#   ./build.sh            plain build into ./build
#   ./build.sh asan       address + undefined sanitizer build into ./build-asan
# Links omega's src/sha256.c (the only shared source); never touches omega's Makefile.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../.." && pwd)
MODE=${1:-plain}
CFLAGS="-std=c11 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -I$HERE -I$ROOT/src"
if [ "$MODE" = asan ]; then OUT="$HERE/build-asan"; CFLAGS="$CFLAGS -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"
else OUT="$HERE/build"; CFLAGS="$CFLAGS -O2"; fi
CC=${CC:-gcc}
mkdir -p "$OUT"
LIB="$HERE/at0_exact.c $HERE/at0_case.c $HERE/at0_io.c $HERE/at0_state.c $HERE/at0_hamiltonian.c $HERE/at0_constraint.c \
     $HERE/at0_povm.c $HERE/at0_conditional.c $HERE/at0_observable.c $HERE/at0_engine.c $HERE/at0_output.c $ROOT/src/sha256.c"
# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/at0-model" $HERE/at0_main.c $LIB -lm
# shellcheck disable=SC2086
$CC $CFLAGS -o "$OUT/at0-tests" $HERE/tests/test_model.c $LIB -lm
echo "built $OUT/at0-model and $OUT/at0-tests ($MODE)"
