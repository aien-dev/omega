#!/usr/bin/env bash
# tools/verified_program/gate.sh: the gate entry for tests/gates/omega-verified-program.gate (omega#505).
# Same command as `make test-verified-program`, with the physics sibling switched off like the other
# host steps of tests/gates/run.sh. It lives here, not as a run.sh step, because run.sh is being
# changed by open pull requests at the time of writing; it can move into run.sh once those land.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
make PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-verified-program
