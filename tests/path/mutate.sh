#!/bin/sh
# TEST HARNESS: PATH-1 mutation oracle. Each mutant edits a scratch copy of
# src/path/rx_path.c, never the source. A mutant counts as killed only when it
# compiles and the test program reports at least one FAIL line.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
scratch=$(mktemp -d "${TMPDIR:-/tmp}/omega-path-mutants.XXXXXX")
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
killed=0
total=0
run() {
    name=$1
    expression=$2
    total=$((total+1))
    cp "$root/src/path/rx_path.c" "$scratch/orig.c"
    sed "$expression" "$scratch/orig.c" > "$scratch/mutant.c"
    if cmp -s "$scratch/orig.c" "$scratch/mutant.c"; then
        echo "MUTANT $name: mutation did not apply" >&2; exit 1
    fi
    # A compiler error does not count as an invariant caught by tests.
    ${CC:-cc} -std=c11 -Wall -Wextra -Werror -O2 -I"$root/src" -I"$root/src/path" \
        "$scratch/mutant.c" "$root/src/sha256.c" "$root/tests/path/test_path_identity.c" \
        -o "$scratch/test"
    if "$scratch/test" > "$scratch/log" 2>&1; then
        echo "MUTANT $name: SURVIVED" >&2; cat "$scratch/log" >&2; exit 1
    fi
    if ! grep -q '^FAIL ' "$scratch/log"; then
        echo "MUTANT $name: crashed without assertion evidence" >&2; cat "$scratch/log" >&2; exit 1
    fi
    killed=$((killed+1))
    echo "MUTANT $name: killed"
}
run semantic-tag 's/static const char tag\[\] = RX_PATH_SEMANTIC_TAG;/static const char tag[] = "omega.path.v2";/'
run tag-terminator 's/emit(&e, tag, sizeof(tag)); \/\* sizeof includes/emit(\&e, tag, sizeof(tag) - 1); \/* sizeof includes/'
run little-endian '/^static void put_u16/,/^}/{s/b\[0\] = /b[9] = /;s/b\[1\] = /b[0] = /;s/b\[9\] = /b[1] = /};s/((uint16_t)b\[0\] << 8) | b\[1\]/((uint16_t)b[1] << 8) | b[0]/'
run step-limit 's/if (rx_path_step_count(p) >= RX_PATH_MAX_STEPS)/if (rx_path_step_count(p) > RX_PATH_MAX_STEPS)/'
run input-limit 's/input_count > RX_PATH_MAX_INPUTS_PER_STEP)/input_count > RX_PATH_MAX_INPUTS_PER_STEP + 1)/'
run size-limit 's/if (size + step_size(step) > RX_PATH_MAX_TOTAL_SERIALIZATION)/if (0)/'
run attr-sort 's/while (at < p->attr_count \&\& key_cmp(p->attributes\[at\].key, key) < 0) at++;/at = p->attr_count;/'
run constraint-sort 's/while (at < p->constraint_count \&\& constraint_cmp(\&p->constraints\[at\], \&c) < 0) at++;/at = p->constraint_count;/'
run step-index-check 's/if (!take_u16(r, \&index) || index != i)/if (!take_u16(r, \&index))/'
run steps-payload-len '/if (steps_len != r->n - r->pos) return RX_PATH_ERR_MALFORMED;/d'
run trailing-bytes '/if (r->pos != r->n) return RX_PATH_ERR_MALFORMED;/d'
run magic 's/if (!q || memcmp(q, path_magic, 4) != 0)/if (!q)/'
run family-role '/if ((role >> 8) != family) return 0;/d'
run expected-id '/rc = RX_PATH_ERR_ID_MISMATCH;/d'
run parent-frozen 's/parent->frozen = 1;/parent->frozen = 0;/'
run lineage-check '/if (memcmp(pid.bytes, q->parent_path_id.bytes, 32) != 0) return RX_PATH_ERR_ID_MISMATCH;/d'
run divergence-bound 's/if (divergence_step_index > rx_path_step_count(parent))/if (divergence_step_index > 1 + rx_path_step_count(parent))/'
run prefix-resolve 's/if (p->parent \&\& i < p->divergence_step_index)/if (p->parent \&\& i <= p->divergence_step_index)/'
run realization-machine '/emit_id(&e, &r->machine_id);/d'
run realization-evidence '/emit_id(&e, &r->argus_observation_digest);/d'
echo "PATH mutations: $killed killed, $((total-killed)) survived ($total total)"
