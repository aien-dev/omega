#!/bin/sh
# Research-only: regenerates tests/ternary/a64_encoder_expected.txt from the
# GNU assembler. The gate itself compares against the committed file and
# needs no assembler.
set -eu
here=$(dirname "$0")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
as -o "$tmp/s.o" "$here/a64_encoder_samples.s"
objcopy -O binary -j .text "$tmp/s.o" "$tmp/s.bin"
od -An -tx4 -v "$tmp/s.bin" | tr -s ' ' '\n' | grep -v '^$' > "$here/a64_encoder_expected.txt"
wc -l < "$here/a64_encoder_expected.txt"
