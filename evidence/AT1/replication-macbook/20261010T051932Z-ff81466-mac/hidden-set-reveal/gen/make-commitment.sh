#!/bin/bash
# AT-1 Agent 7: build the hidden set and its commitment. Runs on the MacBook only.
set -euo pipefail
export LC_ALL=C
cd "$HOME/at1-hidden/gen"
rm -f at1h
rustc -O --edition 2021 at1h.rs -o at1h
./at1h selftest worked-example.case > selftest.log
tail -1 selftest.log
cd "$HOME/at1-hidden"
if [ ! -s private/SEED ]; then
  ( umask 077; od -An -N8 -tx1 /dev/urandom | tr -d ' \n' > private/SEED )
fi
chmod 600 private/SEED
echo "seed file: $(ls -l private/SEED | cut -c1-10), $(wc -c < private/SEED | tr -d ' ') bytes"
rm -rf cases
./gen/at1h gen "$(cat private/SEED)" cases
cp gen/at1h.rs gen/PROCEDURE.txt cases/
( cd cases && for f in $(ls | sort); do shasum -a 256 "$f"; done ) > MANIFEST
shasum -a 256 MANIFEST > COMMITMENT
cat COMMITMENT
wc -l < MANIFEST
