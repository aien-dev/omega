#!/bin/sh
# DIRAC-0 oracle corpus digest check. STATUS: NOT_RUN. POSIX sh plus sha256sum only.
# usage: oracle_kat_verify.sh dirac-kat-<sha256>.txt
set -eu
f="$1"
want=$(sed -n 's/^corpus_digest //p' "$f")
name=$(basename "$f" .txt)
name=${name#dirac-kat-}
got=$( { printf 'omega.dirac.kat.v1\000'; sed '/^corpus_digest /,$d' "$f"; } | sha256sum | cut -d' ' -f1 )
[ "$got" = "$want" ] || { echo "FAIL digest line mismatch"; exit 1; }
[ "$got" = "$name" ] || { echo "FAIL file name mismatch"; exit 1; }
echo PASS
