#!/bin/sh
# require_physics_pin.sh PHYSICS_DIR
# omega#456: the chip runners build against a Physics checkout given on their command line (default
# ~/workspace/physics-own30b-k). They build single targets, so the Makefile's check-physics-lock (run by `all`)
# never ran for them. Each runner calls this first, before it writes evidence or builds anything: it refuses
# (exit 2) when PHYSICS_DIR's HEAD is not the commit in this repo's physics.lock, or when HEAD cannot be read.
# PHYSICS_LOCK_CHECK=0 skips the check, the same override the Makefile honours, and says so on stderr.
set -u
PHYS="${1:?usage: require_physics_pin.sh PHYSICS_DIR}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 2
if [ "${PHYSICS_LOCK_CHECK:-1}" = "0" ]; then
    echo "require_physics_pin: PHYSICS_LOCK_CHECK=0, Physics pin NOT checked for $PHYS" >&2
    exit 0
fi
[ -f "$ROOT/physics.lock" ] || { echo "require_physics_pin: REFUSED: $ROOT/physics.lock not found" >&2; exit 2; }
locked=$(tr -d '[:space:]' < "$ROOT/physics.lock")
actual=$(git -C "$PHYS" rev-parse HEAD 2>/dev/null) || actual=""
if [ -z "$actual" ]; then
    echo "require_physics_pin: REFUSED: cannot read HEAD of PHYSICS_DIR=$PHYS" >&2
    exit 2
fi
if [ "$actual" != "$locked" ]; then
    echo "require_physics_pin: REFUSED: PHYSICS_DIR=$PHYS is at $actual but physics.lock pins $locked" >&2
    echo "  check out the pinned Physics commit there, pass a checkout at the pin, or set PHYSICS_LOCK_CHECK=0" >&2
    exit 2
fi
exit 0
