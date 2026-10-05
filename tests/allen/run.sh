#!/bin/sh
# ALLEN v0 host gates (spec/allen.md; ARCH-0035 PROPOSED, OS-0018 PROPOSED).
# Usage (from `make test-allen`): ALLEN_TOOL=<allen> ALLEN_BIND_OBJ=<allen_bind.o>
#   ALLEN_MAIN_OBJ=<allen.o> ALLEN_OUT=<scratch dir> sh tests/allen/run.sh
#
# Every gate is a host test: processes, files, the resident World. None of
# this is hardware qualification, an OS reboot, or a machine migration; the
# receipt says so. The negative control (G6) is the falsifier: if a standing
# intent already survived a restart through the Cortex journal alone, ALLEN
# would be redundant and this script would FAIL it.
#
# Exit 0 = every gate PASS. Writes $ALLEN_OUT/receipt.json (host facts only;
# the evidence receipt under evidence/ALLEN/ is written from it by the
# qualifying run, with the commit bound).
set -u
T=${ALLEN_TOOL:?ALLEN_TOOL}
BIND=${ALLEN_BIND_OBJ:?ALLEN_BIND_OBJ}
MAIN=${ALLEN_MAIN_OBJ:?ALLEN_MAIN_OBJ}
OUT=${ALLEN_OUT:?ALLEN_OUT}
case $T in /*) ;; *) T=$(pwd)/$T ;; esac
rm -rf "$OUT" && mkdir -p "$OUT" || exit 2
LOG=$OUT/log.txt
: > "$LOG"
FAILS=0
GATES=""

AGENT_A=a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1a1
AGENT_B=b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2b2
ROOT_A=0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a0a
PROV=c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3c3
MODEL_X=1111111111111111111111111111111111111111111111111111111111111111
MODEL_Y=2222222222222222222222222222222222222222222222222222222222222222

say() { printf '%s\n' "$*" | tee -a "$LOG"; }
run() { # run NAME cmd... : stdout+stderr to $OUT/NAME.txt, rc in $RC
    n=$1; shift
    "$@" > "$OUT/$n.txt" 2>&1
    RC=$?
    { echo "## $n (rc=$RC)"; cat "$OUT/$n.txt"; } >> "$LOG"
    return 0
}
field() { # field FILE KEY -> value after 'KEY ' on the first matching line
    sed -n "s/^.*$2 \([^ ]*\).*$/\1/p" "$1" | head -n 1
}
gate() { # gate ID ok(0/1) "what"
    if [ "$2" -eq 0 ]; then say "ALLEN-$1 PASS: $3"; GATES="$GATES$1=PASS "
    else say "ALLEN-$1 FAIL: $3"; FAILS=$((FAILS + 1)); GATES="$GATES$1=FAIL "; fi
}
patch_byte() { # patch_byte IN OUT OFFSET HEXBYTE
    cp "$1" "$2" && printf "\\$(printf '%03o' "0x$4")" | dd of="$2" bs=1 seek="$3" conv=notrunc 2>/dev/null
}
sha() { sha256sum "$1" | cut -c1-64; }

# ---- fixtures ---------------------------------------------------------------
run seed1 "$T" seed-journal "$OUT/j1.cx" 1
run seed2 "$T" seed-journal "$OUT/j2.cx" 2
L1=$(field "$OUT/seed1.txt" lineage); L2=$(field "$OUT/seed2.txt" lineage)
[ -n "$L1" ] && [ -n "$L2" ] && [ "$L1" != "$L2" ] || { say "fixture: two distinct journal lineages needed"; exit 2; }
run make1 "$T" make -o "$OUT/s1.bin" --agent $AGENT_A --root $ROOT_A --prov $PROV --intent 7,1000 --cortex "$OUT/j1.cx"
run make1b "$T" make -o "$OUT/s1b.bin" --agent $AGENT_A --root $ROOT_A --prov $PROV --intent 7,1000 --cortex "$OUT/j1.cx"
run make2 "$T" make -o "$OUT/s2.bin" --agent $AGENT_B --root $ROOT_A --prov $PROV --intent 7,1000 --cortex "$OUT/j1.cx"
run makeU "$T" make -o "$OUT/su.bin" --agent $AGENT_A --root $ROOT_A --prov $PROV --intent 7,1000
run makeS "$T" make -o "$OUT/ss.bin" --agent $AGENT_A --root $ROOT_A --prov $PROV --intent 7,1000 --intent 7,2000 --cortex "$OUT/j1.cx"
S1=$(field "$OUT/make1.txt" subject); S1B=$(field "$OUT/make1b.txt" subject); S2=$(field "$OUT/make2.txt" subject)
I1=$(sed -n 's/^ALLEN intent \([0-9a-f]*\) .*/\1/p' "$OUT/make1.txt" | head -n 1)

# ---- G1 identity binding ----------------------------------------------------
# The subject object carries the LogicalAgentId; its id is content-addressed
# (same content, same id; another agent, another id); the rig binds the
# World's external subject number to that agent, not to a model or a process.
run g1 "$T" publish "$OUT/s1.bin" "$OUT/j1.cx"
ok=1
[ "$RC" -eq 0 ] && [ -n "$S1" ] && [ "$S1" = "$S1B" ] && [ "$S1" != "$S2" ] &&
    [ "$(field "$OUT/g1.txt" 'ALLEN agent')" = "$AGENT_A" ] &&
    grep -q "^ALLEN binding external_subject [0-9]* agent $AGENT_A sequence 1$" "$OUT/g1.txt" &&
    [ "$(field "$OUT/g1.txt" 'ALLEN subject')" = "$S1" ] && ok=0
gate G1 $ok "subject id content-addressed ($S1), bound to agent $AGENT_A; another agent gives another id"
# Foreign-agent and foreign-root refusal at the Store resolver is the aienos
# half of G1 (test_continuity_subject: t_resolve); recorded, not re-run here.

# ---- G2 restart persistence (process level) ---------------------------------
# Publish, let the process exit, publish again from a fresh process: the same
# subject id, the same intent id and the same goal are restored, and the
# organism's AIEN faculty wakes again on its own (assess activations 1).
run g2 "$T" publish "$OUT/s1.bin" "$OUT/j1.cx"
ok=1
[ "$RC" -eq 0 ] && [ "$(field "$OUT/g2.txt" 'ALLEN subject')" = "$S1" ] &&
    grep -q "^ALLEN publish intent $I1 -> EXTERNAL crumb [0-9]*$" "$OUT/g2.txt" &&
    grep -q "^ALLEN goal object seq 1 regime 7 target_ns 1000 " "$OUT/g2.txt" &&
    grep -q "^AIEN assess activations 1 in_episode_of_last_publish 1 assessment goal_seq 1 " "$OUT/g2.txt" &&
    grep -q "^ALLEN goal object seq 1 regime 7 target_ns 1000 " "$OUT/g1.txt" && ok=0
gate G2 $ok "second process restores subject $S1, intent $I1, goal regime 7 / 1000 ns; AIEN assess woke once"
# Store-level restart (two processes over one sealed Store image) is the
# aienos gate CK_CONTINUITY_SUBJECT_RESTART; recorded, not re-run here.

# ---- G3 memory binding ------------------------------------------------------
# Fail closed: a subject only acts with the Cortex journal whose genesis record
# it names. Another journal, an unbound subject, and an empty journal are refused
# before anything is published.
run g3a "$T" publish "$OUT/s1.bin" "$OUT/j2.cx"; a=$RC
run g3b "$T" publish "$OUT/su.bin" "$OUT/j1.cx"; b=$RC
run g3c "$T" publish "$OUT/s1.bin" "$OUT/j3-empty.cx"; c=$RC
ok=1
[ "$a" -eq 3 ] && grep -q "Cortex lineage mismatch" "$OUT/g3a.txt" && ! grep -q "EXTERNAL crumb" "$OUT/g3a.txt" &&
    [ "$b" -eq 3 ] && grep -q "no Cortex lineage binding" "$OUT/g3b.txt" &&
    [ "$c" -eq 3 ] && grep -q "holds no genesis record" "$OUT/g3c.txt" &&
    grep -q "^ALLEN memory BOUND lineage $L1 " "$OUT/g1.txt" && ok=0
gate G3 $ok "bound journal accepted (lineage $L1); other journal, unbound subject, empty journal refused before publish"

# ---- G4 model independence --------------------------------------------------
# Two different model digests and no model at all: identical subject bytes,
# identical subject id, identical intent id, identical goal. The object has no
# model field (continuity_subject.h); the model is provenance printed by the
# host tool and stored nowhere.
h0=$(sha "$OUT/s1.bin")
run g4x "$T" publish "$OUT/s1.bin" "$OUT/j1.cx" --model $MODEL_X; x=$RC
run g4y "$T" publish "$OUT/s1.bin" "$OUT/j1.cx" --model $MODEL_Y; y=$RC
h1=$(sha "$OUT/s1.bin")
ok=1
[ "$x" -eq 0 ] && [ "$y" -eq 0 ] && [ "$h0" = "$h1" ] &&
    [ "$(field "$OUT/g4x.txt" 'ALLEN subject')" = "$S1" ] && [ "$(field "$OUT/g4y.txt" 'ALLEN subject')" = "$S1" ] &&
    grep -q "^ALLEN publish intent $I1 " "$OUT/g4x.txt" && grep -q "^ALLEN publish intent $I1 " "$OUT/g4y.txt" &&
    grep -q "^ALLEN model $MODEL_X (provenance only" "$OUT/g4x.txt" &&
    grep -q "^ALLEN model none (no model required)" "$OUT/g2.txt" &&
    ! grep -q "$MODEL_X" "$OUT/s1.bin" && ok=0
gate G4 $ok "models X, Y and none give the same subject, intent and goal; object bytes unchanged ($h0)"

# ---- G5 no authority, no orchestrator (R16) ---------------------------------
# allen_bind.o: no thread, wait, clock, signal, reaction registration or
# capability mint symbols. allen.o (the rig) may build the World and mint for
# the AIEN faculty like R11's test does, but has no thread, timer, signal or
# reaction of its own: it publishes and observes.
need_absent_bind='pthread_create|^poll$|epoll|^select$|^sleep$|nanosleep|usleep|clock_|timer_|^signal$|sigaction|alarm|rx_world_add_reaction|rx_world_resume|aienos_cap_mint|rx_aien_|rx_world_publish'
need_absent_main='pthread_create|^poll$|epoll|^select$|^sleep$|nanosleep|usleep|clock_|timer_|^signal$|sigaction|alarm|rx_world_add_reaction|rx_world_resume|fork|execv|system'
ub=$(nm -u "$BIND" | awk '{print $2}' | grep -E "$need_absent_bind" || true)
um=$(nm -u "$MAIN" | awk '{print $2}' | grep -E "$need_absent_main" || true)
# Source: no loop in allen_bind.c except the fixed byte loops; no run_until/max_turns anywhere.
lp=$(grep -nE 'while|for *\(' src/allen/allen_bind.c | grep -vE 'i < n|i >= 0|2 \* i' || true)
mt=$(grep -nEi 'max_turns|max_steps|run_until|orchestrat|heartbeat|dispatch' src/allen/allen_bind.c src/allen/allen_bind.h tools/allen.c || true)
ok=1
[ -z "$ub" ] && [ -z "$um" ] && [ -z "$lp" ] && [ -z "$mt" ] && ok=0
[ -n "$ub$um$lp$mt" ] && say "G5 offenders: bind[$ub] main[$um] loops[$lp] words[$mt]"
gate G5 $ok "allen_bind.o and allen.o carry no thread/wait/timer/signal/reaction/mint symbols; no loop words in source"
# The cross-repo loop inventory (R16-G2, make test-r16-inventory) is run by
# the qualifying session on the committed tree; its result is in the receipt.

# ---- G6 negative control (the falsifier) ------------------------------------
# A fresh journal; publish once (the journal records the EXTERNAL crumb and
# the AIEN activations); start the rig again WITHOUT ALLEN: if the World holds
# a goal, the intent survived through memory alone and ALLEN is redundant ->
# FAIL ALLEN. Then publish again: ALLEN restores it.
run seed6 "$T" seed-journal "$OUT/j6.cx" 6
run make6 "$T" make -o "$OUT/s6.bin" --agent $AGENT_A --root $ROOT_A --prov $PROV --intent 7,1000 --cortex "$OUT/j6.cx"
run g6a "$T" publish "$OUT/s6.bin" "$OUT/j6.cx"; a=$RC
run g6b "$T" probe "$OUT/j6.cx"; b=$RC
run g6c "$T" publish "$OUT/s6.bin" "$OUT/j6.cx"; c=$RC
rec_a=$(field "$OUT/g6a.txt" journal_records); rec_b=$(field "$OUT/g6b.txt" journal_records)
goals_b=$(field "$OUT/g6b.txt" goals_in_world)
ok=1
[ "$a" -eq 0 ] && [ "$b" -eq 0 ] && [ "$c" -eq 0 ] &&
    [ "${rec_a:-0}" -gt 1 ] && [ "$rec_b" = "$rec_a" ] && [ "$goals_b" = "0" ] &&
    grep -q "^ALLEN probe journal_records [0-9]* goals_in_world 0 goal_seq 0 assessment_goal_seq 0$" "$OUT/g6b.txt" &&
    grep -q "^ALLEN goal object seq 1 regime 7 target_ns 1000 " "$OUT/g6c.txt" &&
    grep -q "^ALLEN published 1 goals_in_world 1 " "$OUT/g6c.txt" && ok=0
if [ "$goals_b" != "0" ]; then say "NEGATIVE CONTROL BROKEN: intent survived without ALLEN (goals_in_world=$goals_b); ALLEN would be redundant"; fi
gate G6 $ok "without ALLEN the restarted World holds 0 goals although the journal holds $rec_b records; with ALLEN the goal is back"

# ---- format refusals: unknown version, corruption, truncation, trailing ------
patch_byte "$OUT/s1.bin" "$OUT/r-ver.bin" 8 01          # format_version u16 -> 1
patch_byte "$OUT/s1.bin" "$OUT/r-agent.bin" 48 ff       # agent byte -> intent id no longer derives
patch_byte "$OUT/s1.bin" "$OUT/r-intent.bin" 200 00     # intent id byte
head -c 150 "$OUT/s1.bin" > "$OUT/r-trunc.bin"
{ cat "$OUT/s1.bin"; printf 'x'; } > "$OUT/r-trail.bin"
patch_byte "$OUT/s1.bin" "$OUT/r-origin.bin" 184 09     # unknown origin
run rv "$T" inspect "$OUT/r-ver.bin"; v=$RC
run ra "$T" inspect "$OUT/r-agent.bin"; a=$RC
run ri "$T" inspect "$OUT/r-intent.bin"; i=$RC
run rt "$T" inspect "$OUT/r-trunc.bin"; t=$RC
run rl "$T" inspect "$OUT/r-trail.bin"; l=$RC
run ro "$T" inspect "$OUT/r-origin.bin"; o=$RC
run rp "$T" publish "$OUT/r-ver.bin" "$OUT/j1.cx"; p=$RC
ok=1
[ "$v" -eq 3 ] && grep -q "unsupported continuity format version" "$OUT/rv.txt" &&
    [ "$a" -eq 3 ] && grep -q "intent id does not derive from its content" "$OUT/ra.txt" &&
    [ "$i" -eq 3 ] && grep -q "intent id does not derive from its content" "$OUT/ri.txt" &&
    [ "$t" -eq 3 ] && grep -q "truncated object\|body length mismatch" "$OUT/rt.txt" &&
    [ "$l" -eq 3 ] && grep -q "trailing bytes\|body length mismatch" "$OUT/rl.txt" &&
    [ "$o" -eq 3 ] && grep -q "unknown subject origin" "$OUT/ro.txt" &&
    [ "$p" -eq 3 ] && ! grep -q "EXTERNAL crumb" "$OUT/rp.txt" && ok=0
gate FORMAT $ok "unknown version, corrupted agent/intent, truncation, trailing bytes, unknown origin: all refused, nothing published"

# ---- replay: the same bytes are the same subject, never a second one --------
# (The Store refuses a second genesis for one root: aienos t_sealed_store.)
ok=1
[ "$(field "$OUT/g1.txt" 'ALLEN subject')" = "$(field "$OUT/g2.txt" 'ALLEN subject')" ] &&
    [ "$(field "$OUT/g1.txt" 'ALLEN subject')" = "$(field "$OUT/g4x.txt" 'ALLEN subject')" ] &&
    [ "$($T digest "$OUT/s1.bin")" = "$S1" ] && [ "$($T digest "$OUT/s1b.bin")" = "$S1" ] && ok=0
gate REPLAY $ok "replaying the object yields the same subject id $S1 every time; no second subject appears"

# ---- supersession: one ACTIVE intent per slot; the older one is SUPERSEDED --
run sup "$T" publish "$OUT/ss.bin" "$OUT/j1.cx"; s=$RC
ok=1
[ "$s" -eq 0 ] && [ "$(grep -c '^ALLEN intent ' "$OUT/sup.txt")" = "2" ] &&
    grep -q "^ALLEN intent [0-9a-f]* kind 1 state 3 since 1 regime 7 target_ns 1000$" "$OUT/sup.txt" &&
    grep -q "^ALLEN intent [0-9a-f]* kind 1 state 1 since 1 regime 7 target_ns 2000$" "$OUT/sup.txt" &&
    grep -q "^ALLEN published 1 goals_in_world 1 " "$OUT/sup.txt" &&
    grep -q "^ALLEN goal object seq 1 regime 7 target_ns 2000 " "$OUT/sup.txt" && ok=0
gate SUPERSEDE $ok "two intents in one slot: the old one is SUPERSEDED, only the new one (2000 ns) is published"

# ---- subject mismatch: another agent's object is another subject ------------
run mm "$T" publish "$OUT/s2.bin" "$OUT/j1.cx"; m=$RC
ok=1
[ "$m" -eq 0 ] && [ "$(field "$OUT/mm.txt" 'ALLEN subject')" = "$S2" ] && [ "$S2" != "$S1" ] &&
    grep -q "^ALLEN binding external_subject [0-9]* agent $AGENT_B " "$OUT/mm.txt" && ok=0
gate MISMATCH $ok "agent B's object binds as subject $S2, never as $S1 (Store-level foreign-agent refusal: aienos t_resolve)"

# ---- receipt (host facts; the evidence receipt binds the commit) ------------
{
    printf '{\n  "schema": "AIEN_ALLEN_HOST_GATES_V0",\n'
    printf '  "outcome": "%s",\n' "$([ $FAILS -eq 0 ] && echo PASS || echo FAIL)"
    printf '  "gates": "%s",\n' "$(echo $GATES)"
    printf '  "subject_id": "%s",\n  "intent_id": "%s",\n  "lineage": "%s",\n' "$S1" "$I1" "$L1"
    printf '  "negative_control": {"journal_records_after_publish": %s, "goals_without_allen": %s},\n' "${rec_b:-null}" "${goals_b:-null}"
    printf '  "scope": "host processes and files on one machine; not hardware qualification, not an OS reboot, not a machine migration",\n'
    printf '  "tool_sha256": "%s"\n}\n' "$(sha "$T")"
} > "$OUT/receipt.json"
if [ $FAILS -eq 0 ]; then say "test-allen: PASS (G1 identity, G2 restart, G3 memory, G4 model independence, G5 no authority, G6 negative control, format, replay, supersession, mismatch)"; exit 0; fi
say "test-allen: FAIL ($FAILS gate(s)); see $LOG"
exit 1
