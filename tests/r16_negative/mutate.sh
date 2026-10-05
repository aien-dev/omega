#!/bin/sh
# R16-G4 guard mutants (spec/r16-orchestrator-retirement.md §5 R16-G4:
# "removing any one guard turns the test red").
#
# Usage: tests/r16_negative/mutate.sh <cc> <cflags> <aienos-r7-dir> <sources...>
#
# For each mutant, one guard is removed from a scratch copy of one file (the
# tree is never edited), the G4 test is rebuilt with that copy in place of the
# original, and run. A mutant is KILLED when the test does not print its gate
# PASS line ("R16 gate: R16_G4_LEGACY_REFUSED=PASS"). Since spec C5 is closed
# (runtime-issued caller credentials, src/runtime/rx_caller.h) the gate line
# covers the six acts, the promoter-subject exploit probes, the C5 identity
# probes, the promotion control and the C7 review probes (spec C7), so it is
# judged rather than the core line.
# Every mutant must be killed.
# The control build (no mutation) must pass, and every edit must apply exactly
# where named, or the run fails.
#
# Host only; no chip. Each run is capped at 120 s (allowed only because this is a
# host test: never copy the cap into a silicon target) because a removed guard can
# let a legacy loop spin; the G4 test itself finishes in under a second.
set -u
[ $# -ge 4 ] || { echo "usage: $0 <cc> <cflags> <aienos-r7-dir> <sources...>" >&2; exit 2; }
cc=$1; cflags=$2; r7=$3; shift 3
srcs="$*"
lib=${R16_CAP_LIB:-$r7/native/capability/out/libaienos_capability.a}   # make passes $(AIENOS_CAP_LIB)
[ -f "$lib" ] || { echo "R16-G4 mutants: missing $lib" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/r16-mutants.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT INT TERM
killed=0; survived=0; broken=0; total=0

# replace FILE OUT OCCURRENCE: literal OLD (env R16_OLD) -> NEW (env R16_NEW).
# OCCURRENCE is a number (only that match) or "all". Fails unless it applied.
replace() {
    awk -v nth="$3" '
        BEGIN { old = ENVIRON["R16_OLD"]; new = ENVIRON["R16_NEW"]; seen = 0; done = 0 }
        {
            line = $0
            p = index(line, old)
            if (p > 0) {
                seen++
                if (nth == "all" || seen == nth + 0) {
                    line = substr(line, 1, p - 1) new substr(line, p + length(old))
                    done++
                }
            }
            print line
        }
        END { if (done == 0) exit 3 }' "$1" > "$2"
}

build_run() {  # name out-dir sources lib
    name=$1; d=$2
    # shellcheck disable=SC2086
    if ! $cc $cflags -iquote src/runtime -iquote src -pthread -o "$d/g4" $3 "$4" -lm \
        > "$d/build.log" 2>&1; then
        return 2
    fi
    timeout 120 "$d/g4" > "$d/run.log" 2>&1
    if grep -q "^R16 gate: R16_G4_LEGACY_REFUSED=PASS" "$d/run.log"; then
        return 0
    fi
    return 1
}

# mutant NAME FILE OCCURRENCE OLD NEW   (FILE is an omega source, or "aienos")
mutant() {
    name=$1; file=$2; nth=$3
    total=$((total + 1))
    d="$tmp/$name"; mkdir -p "$d"
    use_srcs=$srcs; use_lib=$lib
    if [ "$file" = aienos ]; then
        cp -r "$r7/native/capability" "$d/cap"
        rm -rf "$d/cap/out"
        if ! R16_OLD=$4 R16_NEW=$5 replace "$r7/native/capability/aienos_capability.c" \
            "$d/cap/aienos_capability.c" "$nth"; then
            echo "R16-G4 mutant $name: edit did not apply  BROKEN"; broken=$((broken + 1)); return
        fi
        make -s -C "$d/cap" CC="$cc" > "$d/lib.log" 2>&1 \
            || { echo "R16-G4 mutant $name: library build failed  BROKEN"; broken=$((broken + 1)); return; }
        use_lib="$d/cap/out/libaienos_capability.a"
    else
        base=$(basename "$file")
        if ! R16_OLD=$4 R16_NEW=$5 replace "$file" "$d/$base" "$nth"; then
            echo "R16-G4 mutant $name: edit did not apply  BROKEN"; broken=$((broken + 1)); return
        fi
        use_srcs=""
        for s in $srcs; do
            if [ "$s" = "$file" ]; then use_srcs="$use_srcs $d/$base"; else use_srcs="$use_srcs $s"; fi
        done
    fi
    build_run "$name" "$d" "$use_srcs" "$use_lib"
    case $? in
        0) echo "R16-G4 mutant $name: test still PASSES  SURVIVED"; survived=$((survived + 1)) ;;
        1) why=$(grep -E 'FAIL|not refused|ACCEPTED|changed' "$d/run.log" | head -1)
           [ -n "$why" ] || why="exit or missing gate line: $(tail -1 "$d/run.log")"
           echo "R16-G4 mutant $name: test red  KILLED  ($why)"; killed=$((killed + 1)) ;;
        *) echo "R16-G4 mutant $name: build failed  BROKEN ($(grep -m1 error "$d/build.log"))"
           broken=$((broken + 1)) ;;
    esac
}

W=src/runtime/rx_world.c
G=src/runtime/rx_generation.c
C=src/runtime/rx_coherent.c
S=src/runtime/rx_seq_reference.c

# control: the unmutated build must pass, or nothing below means anything
mkdir -p "$tmp/control"
if ! build_run control "$tmp/control" "$srcs" "$lib"; then
    echo "R16-G4 mutants: control build does not pass (see run below)"
    tail -5 "$tmp/control/run.log" "$tmp/control/build.log" 2>/dev/null
    echo "R16 G4 mutants: CONTROL FAIL"; exit 1
fi
echo "R16-G4 control (no guard removed): PASS"

# acts 1, 2, 6: external writes and reaction admission / activation
mutant publish_external_cap $W 1 \
    'if (rc != RX_CAP_OK) { pthread_mutex_unlock(&w->mu); return RX_ERR_AUTHORITY; }' '(void)rc;'
mutant add_reaction_write_cover $W 1 \
    'if (!covered(w, d, d->writes[i].obj, RX_RIGHT_WRITE)) { rc = RX_ERR_AUTHORITY; goto out; }' '(void)0;'
mutant activation_cap_check $W 1 \
    'if (validate_caps(w, d, &cap_err) != 0) {' 'if (0 && validate_caps(w, d, &cap_err) != 0) {'
mutant commit_cap_recheck $W 2 \
    'if (validate_caps(w, d, &cap_err) != 0) {' 'if (0 && validate_caps(w, d, &cap_err) != 0) {'
mutant both_cap_checks $W all \
    'if (validate_caps(w, d, &cap_err) != 0) {' 'if (0 && validate_caps(w, d, &cap_err) != 0) {'
# act 3: cognition mint / admin in the native AIENOS authority
mutant cognition_mint aienos 1 '(void)token;' '(void)token; if (view) return 0;'
mutant cognition_admin aienos 1 '(void)target;' '(void)target; if (view) return 0;'
# act 4: promotion
mutant promote_self $G 1 \
    'if (request->subject == c->proposer) {' 'if (0 && request->subject == c->proposer) {'
mutant promote_rights $G 1 \
    'if (request->resource != RX_GEN_RES_PROMOTION || request->rights != RX_GEN_RIGHT_PROMOTE) {' \
    'if (0 && (request->resource != RX_GEN_RES_PROMOTION || request->rights != RX_GEN_RIGHT_PROMOTE)) {'
mutant promote_auth $G 1 'if (auth_rc != 0) {' 'if (0 && auth_rc != 0) {'
# act 5: coherent boundary
mutant boundary_cap_identity $C 1 \
    'if (cap.cap_id != o->cap.cap_id || cap.generation != o->cap.generation)' \
    'if (0 && (cap.cap_id != o->cap.cap_id || cap.generation != o->cap.generation))'
mutant boundary_cap_validate $C 1 \
    'if (vrc != RX_CAP_OK) fault = RX_FAULT_CAP;' 'if (0 && vrc != RX_CAP_OK) fault = RX_FAULT_CAP;'
mutant boundary_diverged $C 1 \
    'else if (p->object_id != o->id || p->generation != o->generation)' \
    'else if (0 && (p->object_id != o->id || p->generation != o->generation))'
# extra: the SEQ reference must refuse a production world
mutant seq_pulse_production $S 1 \
    'if (!w || !w->sequential || !plan) return RX_ERR_ARG;' 'if (!w || !plan) return RX_ERR_ARG;'
mutant seq_activate_production $W 1 \
    'if (!w || !w->sequential || rid >= w->n_reactions) return RX_ERR_ARG;' \
    'if (!w || rid >= w->n_reactions) return RX_ERR_ARG;'

# R16 C5: caller identity (runtime-issued credentials). Each removes one check.
mutant id_register $W 1 \
    'if (irc != RX_CALLER_OK) { rc = RX_ERR_IDENTITY; goto out; }' '(void)irc;'
mutant id_activation_commit_recheck $W 1 \
    'if (irc != RX_CALLER_OK) { if (first_err) *first_err = RX_ERR_IDENTITY; return -1; }' '(void)irc;'
mutant id_secret_digest $W 1 \
    'return diff ? RX_CALLER_ERR_FORGED : RX_CALLER_OK;' 'return (void)diff, RX_CALLER_OK;'
mutant id_generation $W 1 \
    'if (cred->generation != w->callers[s].generation) return RX_CALLER_ERR_STALE;' '(void)0;'
mutant id_revoked_check $W 1 \
    'if (!w->callers[s].live) return RX_CALLER_ERR_REVOKED;' '(void)0;'
mutant id_revoked_still_live $W 1 \
    ': !w->callers[s].live ? RX_CALLER_ERR_REVOKED' ': 0 ? RX_CALLER_ERR_REVOKED'
mutant id_revoke_takes_effect $W 1 'w->callers[s].live = false;' '(void)0;'
mutant id_revoke_needs_credential $W 1 'if (rc == RX_CALLER_OK) {' 'if (rc == RX_CALLER_OK || 1) {'
mutant id_enroll_closed $W 1 \
    'if (w->callers_bound) { rc = RX_CALLER_ERR_CLOSED; goto out; }' '(void)0;'
mutant id_promote $G 1 \
    'if (store->caller(store->caller_ctx, request->subject, &request->caller, RX_CALLER_OP_CHECK) != 0)' 'if (0)'
mutant id_propose $G 1 \
    'if (store_bound(store) && store->caller(store->caller_ctx, proposer, cred, RX_CALLER_OP_CHECK) != 0)' \
    'if (0 && store_bound(store) && store->caller(store->caller_ctx, proposer, cred, RX_CALLER_OP_CHECK) != 0)'
mutant id_bound_authority $G 1 'auth = store->bound_auth;' '(void)0;'

# R16 C7: outside review of C5/C6. Each removes one fix; spec C7 names the probe.
mutant c7_flip_recheck $G 1 'int recheck = store_bound(store);' 'int recheck = 0;'
mutant c7_revoke_serialized $W 1 'const int serialize = 1;' 'const int serialize = 0;'
mutant c7_prebind_check $W 1 \
    'if (w->callers_bound || d->caller.generation != 0) {' 'if (w->callers_bound) {'
mutant c7_bind_refuses_unauthenticated $W 1 \
    'if (!w->reactions[i].removed && w->reactions[i].desc.caller.generation == 0)' \
    'if (0 && !w->reactions[i].removed && w->reactions[i].desc.caller.generation == 0)'
mutant c7_mutate_object_bound $G 1 \
    'if (store_bound(store)) return RX_GEN_ERR_IDENTITY;' '(void)0;'
mutant c7_set_evidence_bound $G 2 \
    'if (store_bound(store)) return RX_GEN_ERR_IDENTITY;' '(void)0;'
mutant c7_add_work_credential $G 1 \
    'if (store_bound(store) && store->caller(store->caller_ctx, subject, cred, RX_CALLER_OP_CHECK) != 0)' \
    'if (0 && store_bound(store) && store->caller(store->caller_ctx, subject, cred, RX_CALLER_OP_CHECK) != 0)'
mutant c7_finish_work_credential $G 2 \
    'if (store_bound(store) && store->caller(store->caller_ctx, subject, cred, RX_CALLER_OP_CHECK) != 0)' \
    'if (0 && store_bound(store) && store->caller(store->caller_ctx, subject, cred, RX_CALLER_OP_CHECK) != 0)'
mutant c7_add_work_state $G 1 'if (!starts_open) return RX_GEN_ERR_ARG;' '(void)starts_open;'

echo "R16 G4 mutants: $total total, $killed killed, $survived survived, $broken broken"
if [ $survived -eq 0 ] && [ $broken -eq 0 ]; then
    echo "R16 gate: R16_G4_GUARDS_LOAD_BEARING=PASS"; exit 0
fi
echo "R16 gate: R16_G4_GUARDS_LOAD_BEARING=FAIL"; exit 1
