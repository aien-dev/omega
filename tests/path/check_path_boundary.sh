#!/bin/sh
# TEST HARNESS: PATH-1 boundary check (spec 8.2 and 17.2).
# A path is evidence, never authority: the PATH module must not include runtime
# or capability headers, call capability validation, or offer any execute,
# dispatch, promote or authorize entry point. It must also stay heap free and
# thread free. Comments are stripped before matching.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
bad=0
for f in "$root/src/path/rx_path.h" "$root/src/path/rx_path.c"; do
    code=$(sed -e 's#/\*.*\*/##g' -e '/\/\*/,/\*\//d' "$f")
    if printf '%s\n' "$code" | grep -nE '#include *"(runtime/|rx_world|rx_argus|rx_cortex|rx_jspace|aienos_cap)' ; then
        echo "BOUNDARY $f: includes a runtime or capability header" >&2; bad=1
    fi
    if printf '%s\n' "$code" | grep -nE 'aienos_cap_|RX_ERR_CAP|rx_world_' ; then
        echo "BOUNDARY $f: references capability or runtime symbols" >&2; bad=1
    fi
    if printf '%s\n' "$code" | grep -nE 'rx_path_[a-z_]*(exec|run|dispatch|authori|grant|promot|permit|effect)' ; then
        echo "BOUNDARY $f: declares an authority-bearing entry point" >&2; bad=1
    fi
    if printf '%s\n' "$code" | grep -nE '\b(malloc|calloc|realloc|free|pthread_[a-z_]*|fork|mmap)[[:space:]]*\(' ; then
        echo "BOUNDARY $f: uses heap, threads or system calls" >&2; bad=1
    fi
done
if [ "$bad" -ne 0 ]; then exit 1; fi
echo "PATH boundary: PASS (no runtime links, no authority entry points, no heap or threads)"
