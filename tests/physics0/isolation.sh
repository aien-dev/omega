#!/bin/sh
# PD-0 G5 isolation check (spec section 11 step 7, invariant I10).
# The learner-side objects (everything a learner process may link: the channel,
# wire records, randomness, relation algebra) must contain no reference to any
# generator or world symbol, and must open no file. Usage:
#   sh tests/physics0/isolation.sh <obj>...   (learner-side .o files)
# Prints PHYSICS0_ISOLATION: PASS|FAIL.
fail=0
for o in "$@"; do
    [ -f "$o" ] || { echo "missing $o"; fail=1; continue; }
    syms=$(nm "$o" | awk '{print $NF}')
    for bad in pd0_gen_ pd0_world_ fopen open openat fdopen; do
        if echo "$syms" | grep -q "^_\{0,1\}${bad}"; then
            echo "FAIL: $o references $bad"; fail=1
        fi
    done
done
# and the generator objects really do carry the generators (so the check bites)
if [ -n "$PD0_GEN_OBJ" ]; then
    nm "$PD0_GEN_OBJ" | grep -q " T pd0_gen_step" || { echo "FAIL: $PD0_GEN_OBJ has no pd0_gen_step"; fail=1; }
fi
if [ $fail = 0 ]; then echo "PHYSICS0_ISOLATION: PASS ($# learner-side objects clean)"; else echo "PHYSICS0_ISOLATION: FAIL"; fi
exit $fail
