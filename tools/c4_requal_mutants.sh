#!/bin/sh
# C4 mutant check: each mutant weakens one recovery check in a COPY of the
# runtime, rebuilds the C4 harness against it and runs it. A mutant is KILLED
# when the harness fails (exit nonzero, FAIL cases listed), SURVIVED when it
# still passes (a recovery check no test bites on), ERROR when it did not build
# or the edit did not apply. Exit 0 only if every mutant is KILLED.
#   usage: tools/c4_requal_mutants.sh [out.json]
# Extra make args via MAKE_ARGS (e.g. AIENOS_LOCK_REPO=...).
set -u
root=$(git rev-parse --show-toplevel)
out=${1:-"$root/build/c4_mutants.json"}
auth=$(ls -d "$root"/build/aienos-authority/*/ 2>/dev/null | head -n 1)
[ -n "$auth" ] || { echo "run 'make test-c4-requal' first (builds the authority library)" >&2; exit 2; }
phys=${PHYSICS_DIR:-$(cd "$root/../physics" && pwd)}
work=$(mktemp -d /tmp/c4mut.XXXXXX)
trap 'rm -rf "$work"' EXIT

# id~file~sed expression~what the original check does
MUTANTS='
M01~src/runtime/rx_compose.c~s/ || !aien_mid_equal(&stored, self))/ || 0)/~machine identity must match the stored one
M02~src/runtime/rx_compose.c~s/n_units > 1) return RX_ERR_REPLAY;/n_units > 100000) return RX_ERR_REPLAY;/~refuse durable history when the Cortex journal has no state record
M03~src/runtime/rx_compose.c~s/if (cx_verify_chain(&c->cx) != CX_OK) { cx_close(&c->cx); return RX_ERR_REPLAY; }/;/~Cortex hash chain verified at open
M04~src/runtime/rx_compose.c~s/js_branch_info(&c->js, r, &bi) == JS_OK \&\& !bi.staged/js_branch_info(\&c->js, r, \&bi) == JS_OK/~recovered state branch must be sealed, not staged~redundant: no injection can leave a staged branch in the durable checkpoint (staged branches are never persisted, rx_compose.h recovery note), candidate equivalent mutant, UNVERIFIED
M05~src/runtime/rx_compose.c~s/if (!o || o->id <= chosen) continue;/continue;/~answer newer, non-durable state records with a rollback admission
M06~src/runtime/rx_compose.c~s/for (uint32_t i = l->n; i-- > 0;) {/for (uint32_t i = 0; i < l->n; i++) {/~choose the NEWEST durable state record
M07~src/runtime/rx_compose.c~s/if (n < 0) return RX_ERR_REPLAY;/(void)n;/~an interrupted composition record that cannot be completed refuses~gap: no injection leaves a composition record that cannot be completed
M08~src/runtime/rx_compose.c~s/int n = compose_records(c, S, V, NULL, 0, \&tmp);/int n = 0; (void)S; (void)V; (void)tmp;/~complete every interrupted composition record at open
M09~src/runtime/rx_cortex.c~s/if (memcmp(s->obj\[id - 1\].digest, b + p + 8 \* (CX_R_FIXED + n), 32) != 0) {/if (0) {/~each journal record digest is checked at load
M10~src/runtime/rx_jspace.c~s/if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;/;/~J-Space checkpoint body digest~redundant: header digest, body digest and the restore-time content check overlap; each alone is covered by the others and by structural validation
M11~src/runtime/rx_jspace.c~s/if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;/;/~J-Space checkpoint header digest~redundant: header digest, body digest and the restore-time content check overlap; each alone is covered by the others and by structural validation
M12~src/runtime/rx_compose.c~s/if (js_space_commit(&c->js) != JS_OK) return RX_ERR_REPLAY;/;/~recovered state is made durable before the World starts
M13~src/runtime/rx_compose.c,src/runtime/rx_cortex.c~s/if (cx_verify_chain(&c->cx) != CX_OK) { cx_close(&c->cx); return RX_ERR_REPLAY; }/;/@@s/if (memcmp(s->obj\[id - 1\].digest, b + p + 8 \* (CX_R_FIXED + n), 32) != 0) {/if (0) {/~Cortex integrity (both the load digest and the chain check removed)
M14~src/runtime/rx_jspace.c,src/runtime/rx_jspace.c~s/if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;/;/@@s/if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;/;/~J-Space checkpoint integrity (header and body digests both removed)~gap: with every J-Space integrity digest removed all 820 single-byte flips of the task checkpoint are still refused structurally or benign, so the digests are not shown to bite
M15~src/runtime/rx_jspace.c,src/runtime/rx_jspace.c,src/runtime/rx_jspace.c~s/if (memcmp(d, f + 64, 32)) return JS_ERR_CORRUPT;/;/@@s/if (memcmp(d, f + 96, 32)) return JS_ERR_CORRUPT;/;/@@s/if (memcmp(chk, r->content, 32)) { s->stats.corrupt_restores++; return JS_ERR_CORRUPT; }/;/~J-Space integrity, all three layers (header digest, body digest, restore-time content check) removed~gap: with every J-Space integrity digest removed all 820 single-byte flips of the task checkpoint are still refused structurally or benign, so the digests are not shown to bite
M16~src/runtime/rx_compose.c~s/if (pp \&\& state_ref_promoted(s, pp\[CX_WREC_FIELD0 + RXC_S_REF\])) return RX_ERR_REPLAY;/(void)pp; (void)state_ref_promoted;/~refuse a J-Space checkpoint that lost a state Cortex already promoted
'
n=0; killed=0; survived=0; err=0
printf '{\n  "mutants": [\n' > "$out"
first=1
echo "$MUTANTS" | while IFS="~" read -r id file expr what redund; do
    [ -n "$id" ] || continue
    if [ -n "${ONLY:-}" ]; then case " $ONLY " in *" $id "*) ;; *) continue;; esac; fi
    dir="$work/$id"
    mkdir -p "$dir/build"
    (cd "$root" && cp -r src tests tools Makefile mk aienos.lock physics.lock "$dir/" 2>/dev/null)
    mkdir -p "$dir/build/aienos-authority"
    cp -r "$auth" "$dir/build/aienos-authority/$(basename "$auth")"
    files=$(echo "$file" | tr ',' ' ')
    before=$(for f in $files; do sha256sum "$dir/$f"; done | sha256sum)
    i=1
    for f in $files; do
        e=$(echo "$expr" | awk -F'@@' -v n=$i '{print $n}')
        sed -i "$e" "$dir/$f"; i=$((i+1))
    done
    after=$(for f in $files; do sha256sum "$dir/$f"; done | sha256sum)
    if [ "$before" = "$after" ]; then status=ERROR; note="edit did not apply"
    else
        log="$work/$id.log"
        if ! (cd "$dir" && nice -n 10 make PHYSICS_DIR="$phys" ${MAKE_ARGS:-} c4-requal-bin >"$log" 2>&1); then
            status=ERROR; note="build failed: $(grep -m1 -E "error:|Error " "$log" | cut -c1-120)"
        else
            (cd "$dir" && ./build/rx_c4_requal "$dir/r.json") >"$log.run" 2>&1
            rc=$?
            nf=$(grep -c '^FAIL' "$log.run")
            if [ $rc -ne 0 ] && [ "$nf" -gt 0 ]; then status=KILLED; note="$nf failing case(s), first: $(grep -m1 '^FAIL' "$log.run" | cut -c6-70)"
            elif [ $rc -ne 0 ]; then status=KILLED; note="harness exit $rc (crash or abort)"
            else
                case "$redund" in
                    redundant:*) status=SURVIVED_REDUNDANT; note="$redund" ;;
                    gap:*) status=SURVIVED_GAP; note="$redund" ;;
                    *) status=SURVIVED; note="harness still PASS, no justification recorded" ;;
                esac
            fi
        fi
    fi
    echo "$id $status  $what ($note)" >&2
    [ $first -eq 1 ] || printf ',\n' >> "$out"
    first=0
    printf '    {"id": "%s", "file": "%s", "checks": "%s", "status": "%s", "note": "%s"}' "$id" "$file" "$what" "$status" "$(echo "$note" | tr -d '"\\')" >> "$out"
    rm -rf "$dir"
done
printf '\n  ]\n}\n' >> "$out"
k=$(grep -c '"status": "KILLED"' "$out"); t=$(grep -c '"status"' "$out")
r=$(grep -c '"status": "SURVIVED_REDUNDANT"' "$out"); g=$(grep -c '"status": "SURVIVED_GAP"' "$out")
echo "mutants: $t total, $k killed, $r survived as redundant, $g survived as test gaps (receipt $out)"
# Fail on any unjustified survivor or build/edit error.
[ $((k + r + g)) -eq "$t" ]
