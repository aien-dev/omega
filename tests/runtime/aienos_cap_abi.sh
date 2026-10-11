#!/bin/bash
# OMEGA_AIENOS_CAP_ABI: src/runtime/aienos_cap.h is a hand-kept copy of aienos
# native/capability/aienos_capability.h. The runtime compiles against the copy and
# links libaienos_capability.a built from the aienos.lock commit, so the copy must
# match the pinned header: struct layouts field by field, macro values, prototypes
# (tests/runtime/aienos_cap_abi_test.c says how each is checked).
#
# usage: aienos_cap_abi.sh <dir holding the pinned aienos_capability.h>
#
# The copy must PASS; each mutant below (a temporary copy of aienos_cap.h with one
# change) must be refused with a diagnostic naming what changed. A mutant whose
# pattern no longer matches is a failure of this script, never a silent skip.
# Shell, sed, awk and the C compiler only.
set -u
PIN=${1:?usage: aienos_cap_abi.sh <pinned aienos native/capability dir>}
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
COPY=$HERE/src/runtime/aienos_cap.h
TEST=$HERE/tests/runtime/aienos_cap_abi_test.c
CC=${CC:-cc}
CFLAGS=${CFLAGS:--std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2}
export LC_ALL=C
[ -f "$PIN/aienos_capability.h" ] || { echo "error: no aienos_capability.h in $PIN"; exit 1; }
W=$(mktemp -d "${TMPDIR:-/tmp}/aienos-cap-abi.XXXXXX")
trap 'rm -rf "$W"' EXIT

# gen <header> <dir>: the files aienos_cap_abi_test.c includes (see its comment).
gen() {
    mkdir -p "$2" && cp "$1" "$2/omega_view.h" || return 1
    sed -e '/^typedef struct {/,/^}/d' -e '/^#ifndef /d' -e '/^#endif/d' \
        -e '/^#define [A-Za-z0-9_]*[ \t]*$/d' -e '/^#include /d' "$1" >"$2/omega_protos.h" || return 1
    awk -v D="$2" '
    function bad(m) { printf "aienos_cap_abi: %s:%d: %s: %s\n", FILENAME, FNR, m, $0 > "/dev/stderr"; err = 1; exit 1 }
    function ident_before(s, re) { if (!match(s, re)) bad("no name"); s = substr(s, 1, RSTART - 1); match(s, /[A-Za-z_][A-Za-z0-9_]*$/); return substr(s, RSTART, RLENGTH) }
    /^typedef struct \{[ \t]*$/ { ins = 1; nf = 0; next }
    ins && /^\}/ {
        if (!match($0, /^\}[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*;[ \t]*$/)) bad("struct end")
        t = $0; sub(/^\}[ \t]*/, "", t); sub(/[ \t]*;.*$/, "", t)
        structs = structs " X(" t ", " (++ns) ")"; ren[++nr] = t; pre[nr] = "O_"
        for (i = 1; i <= nf; i++) fields = fields " X(" t ", " f[i] ")"
        ins = 0; next
    }
    ins && /^[ \t]*$/ { next }
    ins && /^[ \t]*(\/\*|\*)/ { next }
    ins {
        if ($0 !~ /^[ \t]+[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*][A-Za-z_][A-Za-z0-9_]*[ \t]*(\[[^]]*\])?[ \t]*;[ \t]*$/) bad("field")
        s = $0; sub(/[ \t]*;[ \t]*$/, "", s); sub(/[ \t]*\[[^]]*\]$/, "", s)
        match(s, /[A-Za-z_][A-Za-z0-9_]*$/); f[++nf] = substr(s, RSTART, RLENGTH); next
    }
    /^typedef[^;]*\(\*[ \t]*[A-Za-z_][A-Za-z0-9_]*[ \t]*\)/ {
        ren[++nr] = ident_before($0, "[ \t]*\\)[ \t]*\\("); pre[nr] = "O_"; next
    }
    /^#define[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]+[^ \t]/ { macros[++nm] = $2; next }
    /^[A-Za-z_][A-Za-z0-9_ \t*]*[ \t*][A-Za-z_][A-Za-z0-9_]*[ \t]*\(/ && $1 != "typedef" {
        fn = ident_before($0, "[ \t]*\\("); funcs = funcs " X(" fn ")"; ren[++nr] = fn; pre[nr] = "o_"; ++nfn; next
    }
    END {
        if (err) exit 1
        if (ins) { print "aienos_cap_abi: unterminated struct" > "/dev/stderr"; exit 1 }
        if (!ns || !nfn) { print "aienos_cap_abi: no struct or no function found" > "/dev/stderr"; exit 1 }
        L = D "/aca_list.h"
        print "#define ACA_STRUCTS(X)" structs > L; print "#define ACA_FIELDS(X)" fields > L
        print "#define ACA_FUNCS(X)" funcs > L
        print "#define ACA_FUNC_COUNT " nfn > L; print "#define ACA_MACRO_COUNT " nm > L
        for (i = 1; i <= nm; i++)
            printf "#ifndef %s\n#error \"%s is not defined by the pinned header\"\n#endif\n", macros[i], macros[i] > (D "/aca_exist.h")
        printf "" > (D "/aca_exist.h")
        for (i = 1; i <= nr; i++) {
            printf "#define %s %s%s\n", ren[i], pre[i], ren[i] > (D "/aca_rename.h")
            printf "#undef %s\n", ren[i] > (D "/aca_unrename.h")
        }
    }' "$1"
}

# check <header> <dir>: 0 when the header matches the pinned one; output in <dir>/log.
check() {
    gen "$1" "$2" >"$2.gen.log" 2>&1 || { mkdir -p "$2"; cat "$2.gen.log" >"$2/log"; return 1; }
    # shellcheck disable=SC2086
    $CC $CFLAGS -Werror -I"$2" -I"$PIN" -o "$2/t" "$TEST" >"$2/log" 2>&1 </dev/null || return 1
    "$2/t" >>"$2/log" 2>&1 </dev/null
}

if ! check "$COPY" "$W/copy"; then
    echo "FAIL: src/runtime/aienos_cap.h does not match $PIN/aienos_capability.h:"
    grep -E "error|FAIL|aienos_cap_abi:" "$W/copy/log" | head -20
    echo "GATE OMEGA_AIENOS_CAP_ABI_PASS FAIL"
    exit 1
fi
grep '^GATE ' "$W/copy/log"

# name|sed program applied to the copy|text the refusal must name
MUTANTS='M1 two same-size AienosCapEntry fields swapped (issuer, subject)|0,/^    uint32_t issuer;$/{/^    uint32_t issuer;$/{N;s/^\(.*\)\n\(.*\)$/\2\n\1/}}|offsetof AienosCapEntry.issuer
M2 AienosCapEntry.rights signed, same size and offset|0,/^    uint32_t rights;$/s//    int32_t rights;/|kind AienosCapEntry.rights
M3 aienos_cap_reclaim cap_id widened to 64 bits|s/^\(int aienos_cap_reclaim(.*\)uint32_t cap_id);$/\1uint64_t cap_id);/|aienos_cap_reclaim
M4 aienos_cap_clock returns 32 bits|s/^uint64_t aienos_cap_clock(/uint32_t aienos_cap_clock(/|aienos_cap_clock
M5 observer op parameter widened|s/^\(typedef void (\*AienosCapObserver)(void \*ctx, \)uint32_t op,/\1uint64_t op,/|AienosCapObserver
M6 AIENOS_CAP_OBS_KILL value changed|s/^#define AIENOS_CAP_OBS_KILL 6u$/#define AIENOS_CAP_OBS_KILL 9u/|AIENOS_CAP_OBS_KILL
M7 a function the pinned header does not declare|/^int aienos_cap_kill(/a int aienos_cap_unpinned(AienosCapAdmin *admin);|aienos_cap_unpinned
M8 a macro the pinned header does not define|/^#define AIENOS_CAP_OBS_KILL /a #define AIENOS_CAP_OBS_UNPINNED 9u|AIENOS_CAP_OBS_UNPINNED'

total=0 caught=0
while IFS='|' read -r name prog expect; do
    total=$((total + 1)) id=${name%% *}
    mkdir -p "$W/$id"
    sed -e "$prog" "$COPY" >"$W/$id/aienos_cap.h"
    if cmp -s "$COPY" "$W/$id/aienos_cap.h"; then
        echo "MUTANT $name: FAIL (pattern no longer matches src/runtime/aienos_cap.h)"
    elif check "$W/$id/aienos_cap.h" "$W/$id/case"; then
        echo "MUTANT $name: FAIL (accepted)"
    elif ! grep -q -- "$expect" "$W/$id/case/log"; then
        echo "MUTANT $name: FAIL (refused, but not for \"$expect\"):"
        grep -E 'error' "$W/$id/case/log" | head -3
    else
        caught=$((caught + 1))
        echo "MUTANT $name: refused ($(grep -m1 -- "$expect" "$W/$id/case/log" | grep -o 'error: .*' | cut -c1-90))"
    fi
done <<<"$MUTANTS"
echo "GATE OMEGA_AIENOS_CAP_ABI_MUTANTS_PASS $([ "$caught" = "$total" ] && echo PASS || echo FAIL) $caught/$total"
[ "$caught" = "$total" ] || exit 1
echo "OMEGA_AIENOS_CAP_ABI_PASS"
