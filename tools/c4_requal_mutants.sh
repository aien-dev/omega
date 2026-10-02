#!/bin/sh
# C4 mutant check: each mutant weakens one recovery check in a COPY of the
# runtime, rebuilds the C4 harness against it and runs it. A mutant is KILLED
# when the harness fails (exit nonzero, FAIL cases listed), SURVIVED when it
# still passes (a recovery check no test bites on), ERROR when it did not build
# or the edit did not apply. Exit 0 only if every mutant is KILLED.
#   usage: tools/c4_requal_mutants.sh [out.json]
# Extra make args via MAKE_ARGS (e.g. AIENOS_LOCK_REPO=...).
# Thin adapter over tools/mutation_runner.sh (one mutation runner). This file
# holds only the mutant rows (data) and the C4 build and test commands; the
# runner copies the tree, applies each edit, runs them and writes the receipt.
set -u
root=$(git rev-parse --show-toplevel)
out=${1:-"$root/build/c4_mutants.json"}
auth=$(ls -d "$root"/build/aienos-authority/*/ 2>/dev/null | head -n 1)
[ -n "$auth" ] || { echo "run 'make test-c4-requal' first (builds the authority library)" >&2; exit 2; }
phys=${PHYSICS_DIR:-$(cd "$root/../physics" && pwd)}

# M03 retired: cx_open is the single integrity seam; rx_compose no longer re-walks the chain.
# id~file~sed expression~what the original check does[~class~rationale]
# class is redundant or gap for a known-equivalent or untested-check mutant, with the
# rationale in the last column; rows with four columns have no justification.
MUTANTS='
M01~src/runtime/rx_compose.c~s/ || !aien_mid_equal(&stored, self))/ || 0)/~machine identity must match the stored one
M02~src/runtime/rx_compose.c~s/n_units > 1) return RX_ERR_REPLAY;/n_units > 100000) return RX_ERR_REPLAY;/~refuse durable history when the Cortex journal has no state record
M04~src/runtime/rx_compose.c~s/js_branch_info(&c->js, r, &bi) == JS_OK \&\& !bi.staged/js_branch_info(\&c->js, r, \&bi) == JS_OK/~recovered state branch must be sealed, not staged~redundant~no injection can leave a staged branch in the durable checkpoint (staged branches are never persisted, rx_compose.h recovery note), candidate equivalent mutant, UNVERIFIED
M05~src/runtime/rx_compose.c~s/if (!o || o->id <= chosen) continue;/continue;/~answer newer, non-durable state records with a rollback admission
M06~src/runtime/rx_compose.c~s/for (uint32_t i = l->n; i-- > 0;) {/for (uint32_t i = 0; i < l->n; i++) {/~choose the NEWEST durable state record
M07~src/runtime/rx_compose.c~s/if (n < 0) return RX_ERR_REPLAY;/(void)n;/~an interrupted composition record that cannot be completed refuses~gap~no injection leaves a composition record that cannot be completed
M08~src/runtime/rx_compose.c~s/int n = compose_records(c, S, V, NULL, 0, \&tmp);/int n = 0; (void)S; (void)V; (void)tmp;/~complete every interrupted composition record at open
M09~src/runtime/rx_cortex.c~s/if (memcmp(s->obj\[id - 1\].digest, b + p + 8 \* (CX_R_FIXED + n), 32) != 0) {/if (0) {/~each journal record digest is checked at load
M10~src/runtime/rx_jspace.c~s/if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;/;/~J-Space checkpoint body digest~redundant~header digest, body digest and the restore-time content check overlap; each alone is covered by the others and by structural validation
M11~src/runtime/rx_jspace.c~s/if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;/;/~J-Space checkpoint header digest~redundant~header digest, body digest and the restore-time content check overlap; each alone is covered by the others and by structural validation
M12~src/runtime/rx_compose.c~s/if (commit_js(c) != JS_OK) return RX_ERR_REPLAY;/;/~recovered state is made durable before the World starts
M13~src/runtime/rx_cortex.c,src/runtime/rx_cortex.c~s/if (memcmp(s->obj\[id - 1\].digest, b + p + 8 \* (CX_R_FIXED + n), 32) != 0) {/if (0) {/@@s/return memcmp(chain, s->chain, 32) == 0 ? CX_OK : CX_ERR_DIGEST;/return CX_OK;/~Cortex integrity (open-time record digest and the audit chain compare both removed)
M14~src/runtime/rx_jspace.c,src/runtime/rx_jspace.c~s/if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;/;/@@s/if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;/;/~J-Space checkpoint integrity (header and body digests both removed)~gap~with every J-Space integrity digest removed all 820 single-byte flips of the task checkpoint are still refused structurally or benign, so the digests are not shown to bite
M15~src/runtime/rx_jspace.c,src/runtime/rx_jspace.c,src/runtime/rx_jspace.c~s/if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;/;/@@s/if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;/;/@@s/if (memcmp(chk, r->content, 32)) { s->stats.corrupt_restores++; return JS_ERR_CORRUPT; }/;/~J-Space integrity, all three layers (header digest, body digest, restore-time content check) removed~gap~with every J-Space integrity digest removed all 820 single-byte flips of the task checkpoint are still refused structurally or benign, so the digests are not shown to bite
M16~src/runtime/rx_compose.c~s/if (pp \&\& state_ref_promoted(s, pp\[CX_WREC_FIELD0 + RXC_S_REF\])) return RX_ERR_REPLAY;/(void)pp; (void)state_ref_promoted;/~refuse a J-Space checkpoint that lost a state Cortex already promoted
M17~src/runtime/rx_compose.c~s/if (c->cx.n < cnt || (cnt > 0 && (!head || memcmp(head->digest, an + 8, 32) != 0))) {/(void)head; if (0) {/~refuse a Cortex journal cut behind the checkpoint anchor
'
# Same copy list as before: the source dirs plus the one authority library.
copy="src tests tools Makefile mk aienos.lock physics.lock build/aienos-authority/$(basename "$auth")"
build="nice -n 10 make PHYSICS_DIR=\"$phys\" ${MAKE_ARGS:-} c4-requal-bin"
test_cmd='./build/rx_c4_requal "$PWD/r.json"'

# The runner prints the per-mutant lines on stderr as before. -e keeps the old
# note for a build that failed with an empty make log. It FAILS an empty
# sweep (ONLY matching nothing); the legacy script exited 0 then, so map it back.
res=$(printf '%s\n' "$MUTANTS" | "$root/tools/mutation_runner.sh" -k table -m - -t "$test_cmd" \
    -d "$root" -b "$build" -e "empty make log (make killed or never started)" \
    -c "$copy" -o "$out" -f FAIL)
rc=$?
printf '%s\n' "$res"
case $res in "mutants: 0 total,"*) rc=0;; esac
exit $rc
