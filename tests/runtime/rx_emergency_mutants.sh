#!/bin/bash
# R16 G6 operator emergency stop: each guard is load-bearing. Every mutant below
# removes one guard from a temporary copy of the runtime; tests/runtime/rx_emergency.c
# must FAIL against each one (and PASS against the unmutated copy). A mutant whose
# pattern no longer matches is a failure of this script, never a silent skip.
set -u
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
W=$(mktemp -d "${TMPDIR:-/tmp}/rx-estop-mut.XXXXXX")
trap 'rm -rf "$W"' EXIT
CC=${CC:-cc}
SRCS="src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c src/runtime/rx_generation.c src/sha256.c tests/runtime/rx_emergency.c"

# name|file|perl substitution (applied with -0, must match exactly once)
MUTANTS='M1 run_one publishes under a stop|src/runtime/rx_world.c|s/if \(w->halted\) \{\n        set_state\(w, r, RX_CANCELLED\);\n        k\.kind = RX_CRUMB_CANCELLED;\n        k\.reason = RX_ERR_HALTED;\n        k\.t_end_ns = now_ns\(\);\n        crumb_append\(w, &k\);\n        w->halt\.refused\+\+;\n        r->rearm = true;\n        end_activation\(w, rid\);/if (0) {\n        set_state(w, r, RX_CANCELLED);\n        k.kind = RX_CRUMB_CANCELLED;\n        k.reason = RX_ERR_HALTED;\n        k.t_end_ns = now_ns();\n        crumb_append(w, &k);\n        w->halt.refused++;\n        r->rearm = true;\n        end_activation(w, rid);/
M2 workers take work under a stop|src/runtime/rx_world.c|s/\(w->halted \|\| !pop_ready\(w, &rid\)\)/(!pop_ready(w, \&rid))/
M3 outside publication under a stop|src/runtime/rx_world.c|s/if \(w->halted\) \{( +)\/\* R16 G6: nothing outside publishes \*\//if (0) {$1\/* R16 G6: nothing outside publishes *\//
M4 caller check ignores the stop|src/runtime/rx_world.c|s/if \(w->halted\) \{\n        pthread_mutex_unlock\(&w->callers_mu\);\n        return RX_CALLER_ERR_HALTED;/if (0) {\n        pthread_mutex_unlock(\&w->callers_mu);\n        return RX_CALLER_ERR_HALTED;/
M5 a reaction subject may stop the world|src/runtime/rx_world.c|s/if \(w->reactions\[i\]\.desc\.subject == subject\) return RX_ERR_AUTHORITY;/(void)i;/
M6 store ignores the durable mark|src/runtime/rx_generation.c|s/(static int halt_marked\(const RxGenStore \*store\) \{\n)/$1    if (store) return 0;\n/
M7 a restarted world ignores the mark|src/runtime/rx_world.c|s/\} else if \(rc != 0 && !w->halted\) \{/} else if (0) {/
M8 resume deletes the mark without keeping the record|src/runtime/rx_world.c|s/if \(rc == RX_OK && durable_write\(w->halt_dir, name, buf, \(size_t\)n, true\) != 0\)/if (0)/
M9 the stop is not in force until the disk write returns|src/runtime/rx_world.c|s/(        w->halted = true;               \/\* in force before any disk work \*\/\n)(        w->halt\.durable = halt_mark_write\(w\);\n)/$2        if (w->halt.durable != 1) w->halted = false; else w->halted = true;\n/'

build_run() { # dir -> exit status of the test
    (cd "$1" && $CC -std=gnu11 -Wall -Wextra -D_GNU_SOURCE -O2 -Isrc -pthread -o t $SRCS) >"$1/build.log" 2>&1 || { echo "build failed:"; tail -5 "$1/build.log"; return 99; }
    (cd "$1" && timeout 300 ./t) >"$1/run.log" 2>&1
}

copy() { mkdir -p "$1"; (cd "$HERE" && tar -cf - src tests/runtime/rx_emergency.c) | tar -xf - -C "$1"; }

fails=0
copy "$W/base"
build_run "$W/base"; rc=$?
if [ $rc = 0 ]; then echo "ok   unmutated: PASS"; else echo "FAIL unmutated: exit $rc"; tail -5 "$W/base/run.log"; fails=$((fails + 1)); fi
while IFS='|' read -r name file sub; do
    [ -n "$name" ] || continue
    d="$W/m${name%% *}"
    copy "$d"
    n=$(perl -0 -e 'my $s = do { local $/; <STDIN> }; my $c = () = eval "\$s =~ $ARGV[0]"; print $c' "$sub" < "$d/$file")
    perl -0pi -e "$sub" "$d/$file"
    if cmp -s "$d/$file" "$HERE/$file" || [ "$n" != 1 ]; then
        echo "FAIL $name: pattern matched $n times (must be 1)"; fails=$((fails + 1)); continue
    fi
    build_run "$d"; rc=$?
    if [ $rc = 99 ]; then echo "FAIL $name: mutant did not build"; fails=$((fails + 1))
    elif [ $rc != 0 ]; then echo "ok   $name: killed ($(grep -c '^\[-\]' "$d/run.log") checks failed)"
    else echo "FAIL $name: SURVIVED"; fails=$((fails + 1)); fi
done <<< "$MUTANTS"
if [ $fails = 0 ]; then echo "RX_EMERGENCY_MUTANTS: PASS"; else echo "RX_EMERGENCY_MUTANTS: FAIL ($fails)"; exit 1; fi
