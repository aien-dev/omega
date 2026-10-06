#!/bin/bash
# R16 G6 operator control: every link from the outside request to the world's
# stop is load-bearing. Each mutant changes one place in a temporary copy of the
# PRODUCTION sources; the production host program is rebuilt from that copy with
# the production flags, and tests/runtime/rx_operator_host.sh must FAIL against it
# (exit non-zero with at least one failed check). The unmutated copy must PASS.
# A pattern that does not match exactly once fails this script, never a skip.
# Every run writes its receipt and failed logs under its own directory in the
# scratch area (RX_OP_OUT), never over build/r16-operator.
#
#   rx_operator_mutants.sh <rx_operator client>
# Environment from make: CC CFLAGS RX_PROD_ARGUS_FLAGS RX_R13_SRCS RX_PROD_ARGUS_SRCS AIENOS_CAP_LIB
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
CLI=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
W=$(mktemp -d "${TMPDIR:-/tmp}/rx-op-mut.XXXXXX")
trap 'rm -rf "$W"' EXIT
JOBS=${RX_OP_MUT_JOBS:-4}

# name|file|phases the test runs against it|perl substitution (applied with -0, must match once)
MUTANTS='O1 wiring missing: no durable halt directory|tests/runtime/rx_r13_living.c|control|s/int halt_mark = rx_world_set_halt_dir\(&r->w, r->generation_dir\);/int halt_mark = 0;/
O2 wiring only in a comment|tests/runtime/rx_r13_living.c|control|s/int halt_mark = rx_world_set_halt_dir\(&r->w, r->generation_dir\);/int halt_mark = 0; \/* rx_world_set_halt_dir(\&r->w, r->generation_dir); *\//
O3 entry point referenced, never called|tests/runtime/rx_r13_living.c|control|s/    if \(operator_open\(r\) != 0\) return -1;/    if (0 \&\& operator_open(r) != 0) return -1;/
O4 command handler disconnected (stop never routed)|src/runtime/rx_operator.c|control|s/int stop = strcmp\(q->cmd, "stop"\) == 0,/int stop = 0,/
O5 success reply without a state transition|src/runtime/rx_operator.c|control|s/rc = rx_world_emergency_stop\(w, q->subject, &q->cred, q->cap, q->reason, &h\);/rc = RX_OK; h.seq = 1; h.durable = 1;/
O6 unauthorized resume accepted (check called, result unused)|src/runtime/rx_world.c|control|s/int rc = halt_authorize\(w, subject, cred, cap\);\n    if \(rc == RX_OK && !w->halted\) rc = RX_HALT_NOT_STOPPED;/int rc = (halt_authorize(w, subject, cred, cap), RX_OK);\n    if (rc == RX_OK \&\& !w->halted) rc = RX_HALT_NOT_STOPPED;/
O7 peer uid check removed|src/runtime/rx_operator.c|control|s/pl != sizeof pc \|\|\n        pc\.uid != geteuid\(\)\) \{/pl != sizeof pc) {/
O8 status without the authority check|src/runtime/rx_operator.c|control|s/rc = rx_world_operator_authorize\(w, q->subject, &q->cred, q->cap\);/rc = RX_OK;/
O9 revoke-cap does not revoke|src/runtime/rx_operator.c|control|s/aienos_cap_revoke\(op->cfg\.admin, office,\n\s+\(AienosCapRef\)\{q->cap\.cap_id, q->cap\.generation\}\) != 0/0/
O10 restart does not wait for the resume|tests/runtime/rx_r13_living.c|restart|s/    if \(world_halted\(r\)\) \{\n        r->op_restored = 1;/    if (0) {\n        r->op_restored = 1;/
O11 startup accepts a group-readable directory|src/runtime/rx_operator.c|startup|s/if \(st\.st_mode & 077\) \{/if (0) {/
O12 audit does not count stops|tests/runtime/rx_r13_living.c|control|s/            stops\+\+;\n//
O13 shutdown accepted while running|src/runtime/rx_operator.c|control|s/if \(!h\.halted\) \{ snprintf\(out, n, "NOT_STOPPED state=running"\); return; \}//
O14 stop reply before the world call (handler answers from the request)|src/runtime/rx_operator.c|control|s/        rc = rx_world_emergency_stop\(w, q->subject, &q->cred, q->cap, q->reason, &h\);\n        if \(rc == RX_OK\) \{/        rc = RX_OK; rx_world_halt_status(w, \&h);\n        if (rc == RX_OK) {/
O15 end-of-episode wait turned off (G5 M-a)|tests/runtime/rx_r13_living.c|control|s/if \(rc == 0 && r->ready && world_halted\(r\)\) \{/if (0 \&\& rc == 0 \&\& r->ready \&\& world_halted(r)) {/
O16 episode shape ignores halted seat claims (G5 M-b)|tests/runtime/rx_r13_living.c|control|s/r->w\.stats\.resident_claims - r->w\.stats\.resident_halted != RX_LIVING_TRIALS/r->w.stats.resident_claims != RX_LIVING_TRIALS/
O17 control directory lock not taken (S1)|src/runtime/rx_operator.c|control|s/if \(flock\(fd, LOCK_EX \| LOCK_NB\) != 0\) \{/if (0) {/
O18 state directory not locked (S1)|tests/runtime/rx_r13_living.c|control|s/rx_operator_lock_dir\(g_state_dir, 1, &g_state_lock, why, sizeof why\) != 0/(g_state_lock < -1 || rx_operator_dir_check(g_state_dir, 1, why, sizeof why) != 0)/
O19 generation directory not locked (S1)|tests/runtime/rx_r13_living.c|control|s/\(g_gen_lock\[mode\] < 0 &&\n\s+rx_operator_lock_dir\(r->generation_dir, 1, &g_gen_lock\[mode\], why, sizeof why\) != 0\)/(g_gen_lock[mode] < -1 || rx_operator_dir_check(r->generation_dir, 1, why, sizeof why) != 0)/
O20 close removes files it did not create (S1)|src/runtime/rx_operator.c|control|s/    if \(fstatat\(op->dir_fd, name, &st, AT_SYMLINK_NOFOLLOW\) == 0 &&\n        st\.st_dev == made->st_dev && st\.st_ino == made->st_ino\)\n        unlinkat/    (void)st; (void)made;\n        unlinkat/
O21 a stop that is not durable replies OK (S2)|src/runtime/rx_operator.c|control|s/if \(h\.durable == 1\)\n/if (1)\n/
O22 unknown commands fall through to the authorized commands (S3)|src/runtime/rx_operator.c|control|s/    if \(!status && !shutdown && !revoke && !revoke_cap\) \{ snprintf\(out, n, "BAD_REQUEST"\); return; \}\n/    (void)revoke_cap;\n/
O23 an expired resume still runs (S4)|src/runtime/rx_operator.c|control|s/if \(strcmp\(q\.cmd, "stop"\) != 0 && monotonic_ns\(\) > q\.deadline_ns\)/if (0)/
O24 a request without a deadline accepted (S4)|src/runtime/rx_operator.c|control|s/ \|\| !q->have_deadline\)\n        return -1;/)\n        return -1;/
O25 any deadline accepted (S4)|src/runtime/rx_operator.c|control|s/    if \(q->deadline_ns > monotonic_ns\(\) \+ RX_OPERATOR_DEADLINE_MAX_NS\) return -1;\n//
'

copy() { mkdir -p "$1"; (cd "$HERE" && tar -cf - src tests/runtime tests/fabric tools) | tar -xf - -C "$1"; ln -s "$HERE/build" "$1/build"; }
build() { # dir: the production host program, production flags and sources
    (cd "$1" && $CC $CFLAGS $RX_PROD_ARGUS_FLAGS -pthread -o prog $RX_R13_SRCS $RX_PROD_ARGUS_SRCS \
        $AIENOS_CAP_LIB -lm) > "$1/build.log" 2>&1
}

fails=0; n_mut=0
copy "$W/base"
names=""
while IFS='|' read -r name file phases sub; do
    [ -n "$name" ] || continue
    id=${name%% *}; d="$W/$id"; copy "$d"
    n=$(perl -0 -e 'my $s = do { local $/; <STDIN> }; my $c = eval "\$s =~ $ARGV[0]g"; die $@ if $@; print $c ? $c + 0 : 0' "$sub" < "$d/$file")
    if [ "$n" != 1 ]; then echo "FAIL $name: pattern matched ${n:-?} times (must be 1)"; fails=$((fails + 1)); continue; fi
    perl -0pi -e "$sub" "$d/$file"
    if cmp -s "$d/$file" "$HERE/$file"; then echo "FAIL $name: substitution changed nothing"; fails=$((fails + 1)); continue; fi
    echo "$phases" > "$d/phases"; echo "$name" > "$d/name"
    names="$names $id"
done <<< "$MUTANTS"

# Builds in parallel, tests one at a time (each test starts the production program).
export CC CFLAGS RX_PROD_ARGUS_FLAGS RX_R13_SRCS RX_PROD_ARGUS_SRCS AIENOS_CAP_LIB
export -f build
printf '%s\n' base $names | xargs -P "$JOBS" -I{} bash -c 'build "$0/{}" || echo "build failed: {}"' "$W"

if [ -x "$W/base/prog" ] && RX_OP_OUT="$W/base/out" bash "$HERE/tests/runtime/rx_operator_host.sh" host "$W/base/prog" "$CLI" > "$W/base/run.log" 2>&1; then
    echo "ok   unmutated: PASS ($(grep -c '^\[+\]' "$W/base/run.log") checks)"
else
    echo "FAIL unmutated: did not pass"; grep -E '^\[-\]|SKIPPED|build failed' "$W/base/run.log" "$W/base/build.log" | head -5
    fails=$((fails + 1))
fi
skipped=$(grep -c '^\[SKIPPED\]' "$W/base/run.log" 2>/dev/null); skipped=${skipped:-0}
for id in $names; do
    d="$W/$id"; name=$(cat "$d/name"); n_mut=$((n_mut + 1))
    if [ ! -x "$d/prog" ]; then echo "FAIL $name: mutant did not build"; tail -3 "$d/build.log"; fails=$((fails + 1)); continue; fi
    RX_OP_OUT="$d/out" RX_OP_PHASES=$(cat "$d/phases") RX_OP_FAILFAST=1 bash "$HERE/tests/runtime/rx_operator_host.sh" host "$d/prog" "$CLI" > "$d/run.log" 2>&1; rc=$?
    nf=$(grep -c '^\[-\]' "$d/run.log" 2>/dev/null); nf=${nf:-0}
    first=$(grep -m1 '^\[-\]' "$d/run.log" | cut -c1-110)
    if [ $rc != 0 ] && [ "$nf" -gt 0 ]; then echo "ok   $name: killed by: ${first#\[-\] }"
    elif grep -q '^\[SKIPPED\]' "$d/run.log"; then echo "SKIPPED $name: the test could not run the case that kills it"; skipped=$((skipped + 1))
    else echo "FAIL $name: SURVIVED (exit $rc)"; fails=$((fails + 1)); fi
done
echo "RX_OPERATOR_MUTANTS: $n_mut mutants, $fails failures, $skipped skipped"
if [ "$fails" = 0 ] && [ "$skipped" = 0 ]; then echo "RX_OPERATOR_MUTANTS: PASS"; exit 0; fi
if [ "$fails" = 0 ]; then echo "RX_OPERATOR_MUTANTS: NOT_RUN (SKIPPED cases)"; exit 3; fi
echo "RX_OPERATOR_MUTANTS: FAIL"; exit 1
