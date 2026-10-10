#!/bin/sh
# Build the AT-1 reference oracle with a direct rustc invocation (no cargo, no crates).
# Usage: ./build.sh            -> target/at1-oracle
#        ./build.sh test       -> builds and runs target/at1-oracle-test
set -eu
cd "$(dirname "$0")"
mkdir -p target
RUSTC_VERSION="$(rustc -V)"
RUSTC_HOST="$(rustc -vV | sed -n 's/^host: //p')"
FLAGS="--edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -C panic=abort -D warnings"
export AT1_RUSTC_VERSION="$RUSTC_VERSION" AT1_RUSTC_HOST="$RUSTC_HOST" AT1_BUILD_FLAGS="$FLAGS"
case "${1:-build}" in
  build)
    rustc $FLAGS --crate-name at1_oracle src/main.rs -o target/at1-oracle
    echo "built target/at1-oracle with $RUSTC_VERSION ($RUSTC_HOST)" ;;
  test)
    # tests need unwinding panics; everything else identical
    rustc --edition 2021 -C opt-level=2 -C codegen-units=1 -C debuginfo=0 -D warnings --test --crate-name at1_oracle src/main.rs -o target/at1-oracle-test
    ./target/at1-oracle-test "${2:-}" ;;
  lib)
    # compute crate alone, for the isolation gate (no main.rs, so no I/O layer)
    rustc $FLAGS --crate-type lib --crate-name at1 src/at1/mod.rs -o target/libat1.rlib
    echo "built target/libat1.rlib" ;;
  *) echo "usage: $0 [build|test|lib]" >&2; exit 1 ;;
esac
