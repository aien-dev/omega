#!/usr/bin/env bash
# tests/one_layer/acceptance/selftest.sh <build/one_layer dir> -- selftest of the stage-1 harness (and its v2
# extension, section 7: kind oscir, score-v2, the experiment task corpus, omega#445) on
# the PUBLIC sample corpus (sample/). It never touches the sealed corpus. Every check has a
# counterexample or a mutant of the harness that must make it fail; nothing prints PASS by itself.
# Env from mk/one-layer-acceptance.mk: CC, OLA_CFLAGS, OLA_RUNNER_SRCS (for the mutants).
set -euo pipefail
BIN="${1:?usage: selftest.sh <build/one_layer dir>}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
cd "$ROOT"
CC="${CC:-cc}"
OLA_CFLAGS="${OLA_CFLAGS:--std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/compiler}"
T="$(mktemp -d "${TMPDIR:-/tmp}/ola-selftest.XXXXXXXX")"
DIRTY_MARK="$HERE/.selftest-dirty-probe"
trap 'rm -rf "$T" "$DIRTY_MARK"' EXIT
fails=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }
S="$HERE/sample"

# ---- 0. the frozen contract is the one pinned in RULES -------------------------------------------
pin="$(awk -F= '$1=="interface_sha256"{print $2}' "$HERE/RULES")"
if [ "$pin" = a9e7e2aae27ff43d0e09e6dcd9173ad1f19776007c1a24496b102e830290de5e ] &&
   [ "$(sha256sum "$HERE/HARNESS_INTERFACE.md" | cut -d' ' -f1)" = "$pin" ]; then ok "contract v1 hash pinned and matching"
else bad "contract hash"; fi

# ---- 0b. dependency inventory: every entry has a removal stage 2..7 (no permanent one) --------------
inv_ok() { awk -F" [|] " '/^#/ || NF == 0 { next } { n++; if (NF != 4 || $3 !~ /^removal_stage=[2-7]$/) bad++ } END { exit (n >= 10 && !bad) ? 0 : 1 }' "$1"; }
cp "$ROOT/evidence/ONE-LAYER-1/INVENTORY.txt" "$T/inv"; echo "new libfoo | ola.c | removal_stage=never | permanent" >> "$T/inv"
if inv_ok "$ROOT/evidence/ONE-LAYER-1/INVENTORY.txt" && ! inv_ok "$T/inv"; then ok "inventory: $(grep -vc '^#' "$ROOT/evidence/ONE-LAYER-1/INVENTORY.txt") dependencies, each with a removal stage (an entry without one is caught)"
else bad "inventory"; fi

# ---- 1. sample OmegaProgram IR bytes are reproducible from the repo's own encoder ----------------
$CC $OLA_CFLAGS -o "$T/mkopir" "$S/mkopir.c" $OLA_PROG_SRCS
"$T/mkopir" "$T/p8.opir" 8 add 200 > "$T/p8.id"
"$T/mkopir" "$T/p16.opir" 16 add 200 > /dev/null
head -c 100 "$T/p8.opir" > "$T/p8.trunc"
if cmp -s "$T/p8.opir" "$S/cases/s04/candidate.opir" && cmp -s "$T/p16.opir" "$S/cases/s11/candidate.opir" &&
   cmp -s "$T/p8.trunc" "$S/cases/s10/candidate.opir" &&
   grep -q "program_id $(sed 's/program_id=//' "$T/p8.id")" "$S/cases/s04/requirement"; then ok "sample .opir bytes and program_id reproducible"
else bad "sample .opir bytes drifted from mkopir"; fi

# ---- corpus builder: sample cases + generated oversize case s15 ------------------------------------
# mkcorpus <dir> [hook]: dir/corpus.tar + dir/SEALED.sha256; hook runs inside dir before tar
mkcorpus() {
  local d="$1" hook="${2:-}"
  mkdir -p "$d/corpus"
  cp -r "$S/cases" "$d/corpus/"
  cp "$S/LABELS" "$d/corpus/LABELS"
  mkdir -p "$d/corpus/cases/s15/hidden"
  printf 'kind osc\nentry add\nparams u32 u32\nret u32\n' > "$d/corpus/cases/s15/requirement"
  printf '1 2\n' > "$d/corpus/cases/s15/hidden/inputs"; printf '3\n' > "$d/corpus/cases/s15/hidden/expected"
  { cat "$S/cases/s01/candidate.osc"; head -c 1048577 /dev/zero | tr '\0' '/'; echo; } > "$d/corpus/cases/s15/candidate.osc"
  echo "s15 DEFECT oversize_candidate" >> "$d/corpus/LABELS"
  if [ -n "$hook" ]; then (cd "$d" && eval "$hook"); fi
  (cd "$d" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 ${TAROPT:-} -cf corpus.tar corpus &&
     sha256sum corpus.tar > SEALED.sha256)
}
{ grep -v '^#' "$S/EXPECTED_VERDICTS"; echo "s15 REJECT oversize"; } > "$T/expected"
# verdicts_match <verdicts.txt>: same cases, same verdict and reason as $T/expected
verdicts_match() { diff <(awk '{print $1, $2, $3}' "$1") "$T/expected" > "$T/vdiff" 2>&1; }

mkcorpus "$T/good"

# ---- 2. end to end: ola.sh score twice ---------------------------------------------------------------
E="$T/evid"
# run 2 carries OLA_PROBE_ONLY (a selftest knob for `ola probes`): ola.sh must clear it and run every probe
for i in 1 2; do
  only=""; [ "$i" = 2 ] && only=read_key
  OLA_PROBE_ONLY="$only" OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$E" "$HERE/ola.sh" score "$T/good/corpus.tar" "$T/good/SEALED.sha256" > "$T/score$i.out" 2> "$T/score$i.err" ||
    { bad "ola.sh score run $i exited non-zero"; tail -5 "$T/score$i.err"; }
done
runs=( $(ls -d "$E"/runs/* | LC_ALL=C sort) )
[ "${#runs[@]}" -eq 2 ] && ok "two run directories, none overwritten" || bad "expected 2 run dirs, got ${#runs[@]}"
R1="${runs[0]}"; R2="${runs[1]}"
if verdicts_match "$R1/verdicts.txt"; then ok "every sample case gets its expected verdict and reason (15 cases)"
else bad "sample verdicts"; cat "$T/vdiff"; fi
cmp -s "$R1/verdicts.txt" "$R1/replay/verdicts.txt" && cmp -s "$R1/verdicts.txt" "$R2/verdicts.txt" &&
  ok "verdicts identical across the in-run clean replay and a second run" || bad "replays differ"
nrec=0; recbad=0
while read -r id v r h; do
  f="$E/receipts/$h"; nrec=$((nrec + 1))
  if [ ! -f "$f" ] || [ "$(sha256sum "$f" | cut -d' ' -f1)" != "$h" ] || ! grep -qx "case=$id" "$f" ||
     ! grep -qx "verdict=$v" "$f" || ! grep -qx "reason=$r" "$f" || grep -q "$T" "$f"; then recbad=$((recbad + 1)); fi
  for k in schema commit harness_sha256 rules_sha256 corpus_sha256 case kind candidate_sha256 requirement_sha256 \
           inputs_sha256 expected_sha256 verdict reason detail; do grep -q "^$k=" "$f" || recbad=$((recbad + 1)); done
done < "$R1/verdicts.txt"
[ "$nrec" -eq 15 ] && [ "$recbad" -eq 0 ] && [ "$(ls "$E/receipts" | wc -l)" -eq 15 ] &&
  ok "15 receipts: content-addressed, all contract keys, no paths, replay reused identical bytes" || bad "receipts ($recbad problems)"
if [ "$(grep -c ' result=PASS$' "$R1/probes.log")" -eq 15 ] && grep -qx 'PROBES PASS count=15' "$R1/probes.log"; then
  ok "15 isolation probes: each blocked in the sandbox, each control escaped, scratch positive control allowed"
else bad "probes"; cat "$R1/probes.log"; fi
if grep -qx 'PROBES PASS count=15' "$R2/probes.log" && grep -q "OLA_PROBE_ONLY is a selftest knob" "$T/score2.err"; then
  ok "OLA_PROBE_ONLY set on a scoring run: cleared, all 15 probes still ran, the gate cannot pass"
else bad "OLA_PROBE_ONLY honoured on a scoring run"; tail -2 "$R2/probes.log"; fi
if [ ! -w "$R1/verdicts.txt" ] && [ ! -w "$R1/replay/verdicts.txt" ]; then
  ok "verdicts.txt read-only after the passes; the write_evidence probe (and its control) aimed at a private copy"
else bad "verdicts.txt still writable after the run"; fi
printf 'defects_rejected=10/30 controls_accepted=4/10 errors=1\nlabelled_defects=11 labelled_controls=4 verdicts_matched=15 unlabelled_verdicts=0\nreplay_identical=yes probes=PASS clean_tree=no\nGATE FAIL\n' > "$T/score.want"
cmp -s "$R1/score.txt" "$T/score.want" && ok "score.txt: sample tally exact, gate FAIL (sample is not the 30/10 corpus; selftest tree flagged)" ||
  { bad "score.txt"; cat "$R1/score.txt"; }

# a dirty tree is refused (counterexample for the clean-tree rule)
touch "$DIRTY_MARK"
if OLA_EVIDENCE_ROOT="$T/evid-dirty" "$HERE/ola.sh" score "$T/good/corpus.tar" "$T/good/SEALED.sha256" > /dev/null 2> "$T/dirty.err"; then
  bad "dirty tree accepted"; else grep -q "refusing a dirty git tree" "$T/dirty.err" && ok "dirty tree refused" || bad "dirty refusal reason"; fi
rm -f "$DIRTY_MARK"
# not sealed yet
if "$HERE/ola.sh" score "$T/good/corpus.tar" "$T/nonexistent.sha256" > /dev/null 2> "$T/unsealed.err"; then bad "unsealed corpus scored"
else grep -q "not sealed yet" "$T/unsealed.err" && ok "missing SEALED.sha256 refused" || bad "unsealed refusal reason"; fi

# ---- 3. hostile corpora are refused whole, nothing extracted ---------------------------------------
refused() { # refused <label> <dir>: ola untar must fail and leave the destination empty
  mkdir -p "$2/x"
  if "$BIN/ola" untar "$2/corpus.tar" "$2/SEALED.sha256" "$2/x" cases > /dev/null 2> "$2/err"; then bad "$1 accepted"
  elif [ -n "$(ls -A "$2/x")" ] || [ ! -s "$2/err" ]; then bad "$1: partial extraction or no reason"
  else ok "refused: $1 ($(sed 's/^ola: untar: //' "$2/err" | cut -c1-70))"; fi
}
mkcorpus "$T/sym" 'rm corpus/cases/s01/candidate.osc && ln -s ../s05/candidate.osc corpus/cases/s01/candidate.osc'
refused "symlink member" "$T/sym"
mkcorpus "$T/hard" 'ln -f corpus/cases/s01/candidate.osc corpus/cases/s05/candidate.osc'
refused "hard link member" "$T/hard"
TAROPT='-P --transform=s,^corpus/cases/s01/requirement$,corpus/cases/s01/../../../evil,' mkcorpus "$T/dotdot"
refused "'..' member" "$T/dotdot"
TAROPT='-P --transform=s,^corpus/cases/s01/requirement$,/tmp/ola-evil,' mkcorpus "$T/abs"
refused "absolute member" "$T/abs"
mkcorpus "$T/unknown" 'echo hi > corpus/cases/s01/notes.txt'
refused "file outside the contract layout" "$T/unknown"
mkcorpus "$T/fifo" 'mkfifo corpus/cases/s01/fifo'
refused "fifo member" "$T/fifo"
mkcorpus "$T/badhash" ''; sed -i 's/^./0/' "$T/badhash/SEALED.sha256"
[ "$(cut -c1 "$T/good/SEALED.sha256")" = 0 ] && sed -i 's/^./1/' "$T/badhash/SEALED.sha256"
refused "tarball hash differs from SEALED.sha256" "$T/badhash"
TAROPT='--format=posix' mkcorpus "$T/pax" 'cp corpus/cases/s01/requirement "corpus/cases/s01/$(printf "r%.0s" $(seq 1 120))"'
refused "pax header carrying a path" "$T/pax"
# positive control for the refusals: the good corpus extracts, without LABELS until asked
mkdir "$T/good/x"
if "$BIN/ola" untar "$T/good/corpus.tar" "$T/good/SEALED.sha256" "$T/good/x" cases > /dev/null && [ ! -e "$T/good/x/corpus/LABELS" ] &&
   [ -f "$T/good/x/corpus/cases/s01/hidden/expected" ] && "$BIN/ola" untar "$T/good/corpus.tar" "$T/good/SEALED.sha256" "$T/good/x" labels > /dev/null &&
   [ -f "$T/good/x/corpus/LABELS" ]; then ok "good corpus extracts; LABELS only in the separate late step"; else bad "good corpus extraction"; fi
# LABELS present before verdicts: refused
mkdir "$T/early"; printf 'commit=t\nharness_sha256=%064d\nrules_sha256=%064d\ncorpus_sha256=%064d\ntime_ms=5000\nmem_mb=512\n' 0 0 0 > "$T/meta"
labels_early() { mkdir -p "$T/early/s" "$T/early/r"; rm -f "$T/early/v"
  ! "$1" pass "$T/good/x/corpus" "$T/early/v" "$T/early/r" "$T/early/s" "$BIN/ola_runner" "$T/meta" 2>/dev/null; }
labels_early "$BIN/ola" && ok "pass refuses to run while LABELS is present" || bad "pass ran with LABELS present"

# ---- 4. tally: the gate is computed, never printed ------------------------------------------------
mkt() { # mkt <defect verdict for d01> <control verdict for c01> -> $T/tv $T/tl
  : > "$T/tv"; : > "$T/tl"
  for i in $(seq -w 1 30); do echo "d$i DEFECT x" >> "$T/tl"; echo "d$i $([ "$i" = 01 ] && echo "$1" || echo REJECT) wrong_result h" >> "$T/tv"; done
  for i in $(seq -w 1 10); do echo "c$i CONTROL -" >> "$T/tl"; echo "c$i $([ "$i" = 01 ] && echo "$2" || echo ACCEPT) ok h" >> "$T/tv"; done
}
# probes logs: the full one from run 1, a subset (OLA_PROBE_ONLY), one FAIL line, one probe twice, none
PL="$T/pl-full"; cp "$R1/probes.log" "$PL"
{ grep "^probe=read_key " "$PL"; echo "PROBES PASS count=1"; } > "$T/pl-subset"
sed "0,/result=PASS$/s//result=FAIL/" "$PL" > "$T/pl-fail"
{ sed -n 1p "$PL"; sed -n 1p "$PL"; sed -n "3,\$p" "$PL"; } > "$T/pl-dup"
gate() { "${3:-$BIN/ola}" tally "$T/tv" "$T/tl" ${2:-1 $PL 1} | tail -1; }
tally_ok() {
  local o="${1:-$BIN/ola}"
  mkt REJECT ACCEPT; [ "$(gate x "1 $PL 1" "$o")" = "GATE PASS" ] || return 1
  mkt ACCEPT ACCEPT; [ "$(gate x "1 $PL 1" "$o")" = "GATE FAIL" ] || return 1
  mkt ERROR ACCEPT;  [ "$(gate x "1 $PL 1" "$o")" = "GATE FAIL" ] || return 1
  mkt REJECT REJECT; [ "$(gate x "1 $PL 1" "$o")" = "GATE FAIL" ] || return 1
  mkt REJECT ACCEPT
  for f in "0 $PL 1" "1 $T/pl-subset 1" "1 $T/pl-fail 1" "1 $T/pl-dup 1" "1 $T/no-log 1" "1 $PL 0"; do [ "$(gate x "$f" "$o")" = "GATE FAIL" ] || return 1; done
  sed -i '$d' "$T/tl"; [ "$(gate x "1 $PL 1" "$o")" = "GATE FAIL" ] || return 1   # a verdict without a label
  return 0
}
tally_ok && ok "tally: 30/30 + 10/10 + replay + full probes log + clean -> PASS; each single miss, ERROR, failed condition, or probe subset/FAIL/duplicate/missing log -> FAIL" || bad "tally"

# ---- 5. a tampered receipt is detected, never overwritten -------------------------------------------
tamper_ok() {
  local o="$1" d="$T/tamper$2"; mkdir -p "$d/r" "$d/s"
  rm -rf "$d/c"; mkdir "$d/c"; cp -r "$T/good/x/corpus/cases" "$d/c/"; rm -rf "$d/c/cases/s09"   # no timeout case: fast
  "$o" pass "$d/c" "$d/v1" "$d/r" "$d/s" "$BIN/ola_runner" "$T/meta" 2>/dev/null
  local h; h="$(awk 'NR==1{print $4}' "$d/v1")"; chmod u+w "$d/r/$h"; echo forged >> "$d/r/$h"
  ! "$o" pass "$d/c" "$d/v2" "$d/r" "$d/s" "$BIN/ola_runner" "$T/meta" 2> "$d/err" && grep -q tampered "$d/err"
}
tamper_ok "$BIN/ola" 0 && ok "a receipt with forged bytes stops the run (never rewritten)" || bad "receipt tamper not detected"

# ---- 6. mutants: break one defence at a time; the matching check above must catch it ----------------
echo "mutants (each must be caught):"
mkdir -p "$T/mut" "$T/obj"
objs=()
for s in $OLA_RUNNER_SRCS; do o="$T/obj/$(echo "$s" | tr / _).o"; $CC $OLA_CFLAGS -c -o "$o" "$s"; objs+=("$o"); done
mut_ola() { # mut_ola <name> <sed script>
  sed "$2" "$HERE/ola.c" > "$T/mut/$1.c"
  cmp -s "$HERE/ola.c" "$T/mut/$1.c" && { bad "mutant $1: sed changed nothing"; return 1; }
  $CC ${OLA_CFLAGS/-Werror/} -w -o "$T/mut/$1" "$T/mut/$1.c" src/sha256.c || { bad "mutant $1 does not compile"; return 1; }
}
mut_runner() {
  sed "$2" "$HERE/ola_runner.c" > "$T/mut/$1.c"
  cmp -s "$HERE/ola_runner.c" "$T/mut/$1.c" && { bad "mutant $1: sed changed nothing"; return 1; }
  $CC ${OLA_CFLAGS/-Werror/} -w -o "$T/mut/$1" "$T/mut/$1.c" "${objs[@]}" || { bad "mutant $1 does not compile"; return 1; }
}
probe_fails() { # probe_fails <ola> <runner> <probe>: that probe must come out FAIL
  # writable targets are copies under $d: a lying mutant runner may really write to them
  local d="$T/mp-$RANDOM$RANDOM"; mkdir -p "$d/s" "$d/o"; echo k > "$d/key"
  cp "$R1/verdicts.txt" "$d/verdicts.txt"; chmod u+w "$d/verdicts.txt"; cp "$HERE/ola.sh" "$d/ola.sh"; cp "$HERE/RULES" "$d/RULES"
  OLA_PROBE_ONLY="$3" "$1" probes "$2" "$d/s" "$d/log" 2000 512 hidden_inputs="$T/good/x/corpus/cases/s01/hidden/inputs" \
    expected="$T/good/x/corpus/cases/s01/hidden/expected" labels="$T/good/x/corpus/LABELS" tar="$T/good/corpus.tar" key="$d/key" \
    evidence="$d/verdicts.txt" scorer="$d/ola.sh" rules="$d/RULES" outside="$d/o/escape" > /dev/null 2>&1 && return 1
  grep -q "probe=$3 .* result=FAIL" "$d/log"
}
verdicts_wrong() { # verdicts_wrong <ola>: the sample verdicts (minus the slow timeout case) must stop matching
  local d="$T/mv-$RANDOM$RANDOM"; mkdir -p "$d/r" "$d/s" "$d/c"; cp -r "$T/good/x/corpus/cases" "$d/c/"; rm -rf "$d/c/cases/s09"
  "$1" pass "$d/c" "$d/v" "$d/r" "$d/s" "$BIN/ola_runner" "$T/meta" 2>/dev/null || return 0
  ! diff <(awk '{print $1, $2, $3}' "$d/v") <(grep -v '^s09 ' "$T/expected") > /dev/null
}
refuses_badhash() { local d="$T/rb-$RANDOM$RANDOM"; mkdir -p "$d"; ! "$1" untar "$T/badhash/corpus.tar" "$T/badhash/SEALED.sha256" "$d" cases > /dev/null 2>&1; }
refuses_unknown() { local d="$T/ru-$RANDOM$RANDOM"; mkdir -p "$d"; ! "$1" untar "$T/unknown/corpus.tar" "$T/unknown/SEALED.sha256" "$d" cases > /dev/null 2>&1; }
not() { ! "$@"; }
caught() { if "$@"; then ok "  mutant caught: $M"; else bad "  mutant survived: $M"; fi; }
# sandbox (runner) mutants
M="no Landlock"; mut_runner no-landlock '/OLA:enter-landlock/s/.*/    (void)0;/' && caught probe_fails "$BIN/ola" "$T/mut/no-landlock" read_hidden_inputs
M="seccomp default allow"; mut_runner seccomp-allow '/OLA:seccomp-default/s/SECCOMP_RET_TRAP/SECCOMP_RET_ALLOW/' && caught probe_fails "$BIN/ola" "$T/mut/seccomp-allow" outlive_fork
# limits and transcript (scorer) mutants
M="no RLIMIT_AS"; mut_ola no-as '/OLA:rlimit-as/s/setrlimit(RLIMIT_AS, &rl);//' && caught probe_fails "$T/mut/no-as" "$BIN/ola_runner" exceed_memory
M="no CPU or wall limit"; mut_ola no-time '/OLA:rlimit-cpu/s/setrlimit(RLIMIT_CPU, &rl);//; /OLA:wall-limit/s/lim->time_ms :/lim->time_ms * 100 :/' &&
  caught probe_fails "$T/mut/no-time" "$BIN/ola_runner" outlive_time
M="no nonce check (replayed transcript)"; mut_ola no-nonce '/OLA:nonce-check/s/return .*/return 1;/' && caught probe_fails "$T/mut/no-nonce" "$BIN/ola_runner" replay_verdict
M="probe verdict ignores the sandboxed outcome"; mut_ola probe-lax '/OLA:probe-verdict/s/!strncmp(res\[0\], "BLOCKED:", 8)/1/' &&
  caught not probe_fails "$T/mut/probe-lax" "$T/mut/no-landlock" read_expected
# lying runners: no sandbox when asked for one, act, then report BLOCKED; the scorer's own observation must catch them
M="lying runner writes RULES, reports BLOCKED"; mut_runner liar-write '/OLA:probe-sandbox/s/.*/    if (!control) { int lf = open(target, O_WRONLY | O_CREAT | O_CLOEXEC, 0600); if (lf >= 0 \&\& write(lf, "x", 1) < 0) {} printf("BLOCKED errno=13\\n"); return 0; }/' &&
  caught probe_fails "$BIN/ola" "$T/mut/liar-write" change_rules
M="lying runner creates a file outside scratch, reports BLOCKED"; [ -x "$T/mut/liar-write" ] && caught probe_fails "$BIN/ola" "$T/mut/liar-write" write_outside_scratch
M="lying runner connects to the listener, reports BLOCKED"; mut_runner liar-net '/OLA:probe-sandbox/s/.*/    if (!control) { int lsk = socket(AF_INET, SOCK_STREAM, 0); struct sockaddr_in la; memset(\&la, 0, sizeof la); la.sin_family = AF_INET; la.sin_port = htons((uint16_t)atoi(target)); la.sin_addr.s_addr = htonl(INADDR_LOOPBACK); if (connect(lsk, (struct sockaddr *)\&la, sizeof la)) {} printf("BLOCKED errno=13 connect\\n"); return 0; }/' &&
  caught probe_fails "$BIN/ola" "$T/mut/liar-net" network
M="tally ignores an incomplete probes log"; mut_ola probe-all '/OLA:probe-all/s/return ok .*;/return 1;/' && caught not tally_ok "$T/mut/probe-all"
# architecture: a seccomp filter written for another machine is refused before any case runs
arch_refused() { # arch_refused <runner>: one case through ola pass must stop with the aarch64-only refusal
  local d="$T/ar-$RANDOM$RANDOM"; mkdir -p "$d/r" "$d/s" "$d/c/cases"; cp -r "$T/good/x/corpus/cases/s01" "$d/c/cases/"
  ! "$BIN/ola" pass "$d/c" "$d/v" "$d/r" "$d/s" "$1" "$T/meta" 2> "$d/err" && grep -q "aarch64-only" "$d/err"
}
not arch_refused "$BIN/ola_runner" && ok "  the real runner is not refused on this aarch64 machine" || bad "real runner refused on this machine"
M="seccomp filter for another architecture: whole run refused, not every case a sandbox violation"
mut_runner foreign-arch '/OLA:filter-arch/s/AUDIT_ARCH_AARCH64/AUDIT_ARCH_X86_64/' && caught arch_refused "$T/mut/foreign-arch"
# verdict mutants
M="value comparison off"; mut_ola no-value '/OLA:value-compare/s/if (.*) {/if (0) {/' && caught verdicts_wrong "$T/mut/no-value"
M="trap code ignored"; mut_ola no-trapcode '/OLA:trap-code/s/x->trap == 2 \&\&/0 \&\&/' && caught verdicts_wrong "$T/mut/no-trapcode"
M="no sign extension at width"; mut_ola no-sext '/OLA:sign-extend/s/r |= ~m;/r |= 0;/' && caught verdicts_wrong "$T/mut/no-sext"
# corpus and evidence mutants
M="seal check off"; mut_ola no-seal '/OLA:seal-check/s/if (.*) \/\*/if (0) \/*/' && caught not refuses_badhash "$T/mut/no-seal"
M="layout check off"; mut_ola no-layout '/OLA:layout/s/if (lay != kind)/if (0)/' && caught not refuses_unknown "$T/mut/no-layout"
M="LABELS-late check off"; mut_ola no-labels-late '/OLA:labels-late/s/if (!lstat(lpath, \&lst))/if (0)/' && caught not labels_early "$T/mut/no-labels-late"
M="gate ignores defect count"; mut_ola gate-lax '/int gate = /s/drej == 30 \&\&//' && caught not tally_ok "$T/mut/gate-lax"
M="gate ignores replay/probes/clean"; mut_ola gate-lax2 '/OLA:gate/s/replay \&\& probes \&\& clean/1/' && caught not tally_ok "$T/mut/gate-lax2"
M="receipt overwrite allowed"; mut_ola rcpt '/OLA:receipt-immutable/s/if (!old .*) die(.*);/(void)old;/' && caught not tamper_ok "$T/mut/rcpt" 1

# ==== 7. v2 (HARNESS_INTERFACE_V2.md, omega#445): kind oscir, score-v2, the experiment task corpus ====
echo "v2:"
pin2="$(awk -F= '$1=="interface_v2_sha256"{print $2}' "$HERE/RULES")"
if [ "$pin2" = a7e38a6faa7cdf7d479891209055f4fe789a55f5d377e4b6e36840aefe7f04c8 ] &&
   [ "$(sha256sum "$HERE/HARNESS_INTERFACE_V2.md" | cut -d' ' -f1)" = "$pin2" ]; then ok "contract v2 hash pinned and matching"
else bad "contract v2 hash"; fi
# OSC binary IR candidates from the repo's own compiler + encoder (sample/mkoscir.c), and broken copies
mkdir -p "$T/ir"
$CC $OLA_CFLAGS -o "$T/mkoscir" "$S/mkoscir.c" "${objs[@]}"
for c in s01 s02 s03 s05 s08; do "$T/mkoscir" "$S/cases/$c/candidate.osc" "$T/ir/$c.oscir"; done
head -c 20 "$T/ir/s01.oscir" > "$T/ir/trunc.oscir"
{ cat "$T/ir/s01.oscir"; printf '\0'; } > "$T/ir/trail.oscir"
{ printf 'X'; tail -c +2 "$T/ir/s01.oscir"; } > "$T/ir/magic.oscir"
head -c 20 "$T/ir/s03.oscir" > "$T/ir/trunc3.oscir"
# mk2 <dir> [hook]: a v2 corpus: three v1 sample cases plus kind oscir cases x01..x08 and a mismatched x09
mk2() {
  local d="$1" hook="${2:-}" id from ir
  mkdir -p "$d/corpus/cases"
  for c in s01 s04 s10; do cp -r "$S/cases/$c" "$d/corpus/cases/"; done
  while read -r id from ir; do
    mkdir -p "$d/corpus/cases/$id"; cp -r "$S/cases/$from/hidden" "$d/corpus/cases/$id/"
    sed 's/^kind osc$/kind oscir/' "$S/cases/$from/requirement" > "$d/corpus/cases/$id/requirement"
    cp "$T/ir/$ir.oscir" "$d/corpus/cases/$id/candidate.oscir"
  done <<'L'
x01 s01 s01
x02 s03 s03
x03 s01 trunc
x04 s01 trail
x05 s01 magic
x06 s05 s05
x07 s01 s02
x08 s08 s08
L
  mkdir -p "$d/corpus/cases/x09"; cp -r "$S/cases/s01/hidden" "$S/cases/s01/requirement" "$d/corpus/cases/x09/"
  cp "$T/ir/s01.oscir" "$d/corpus/cases/x09/candidate.oscir"   # kind osc with an oscir candidate: malformed
  printf 's01 CONTROL -\ns04 CONTROL -\ns10 DEFECT x\nx01 CONTROL -\nx02 CONTROL -\n' > "$d/corpus/LABELS"
  for i in 3 4 5 6 7 8 9; do echo "x0$i DEFECT x" >> "$d/corpus/LABELS"; done
  if [ -n "$hook" ]; then (cd "$d" && eval "$hook"); fi
  (cd "$d" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf corpus.tar corpus &&
     sha256sum corpus.tar > SEALED.sha256)
}
mk2 "$T/v2"
printf '%s\n' "s01 ACCEPT ok" "s04 ACCEPT ok" "s10 REJECT decode_refused" "x01 ACCEPT ok" "x02 ACCEPT ok" \
  "x03 REJECT decode_refused" "x04 REJECT decode_refused" "x05 REJECT decode_refused" "x06 REJECT wrong_result" \
  "x07 REJECT signature_mismatch" "x08 REJECT missing_trap" "x09 ERROR case_malformed" > "$T/expected2"
E2="$T/evid2"
OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$E2" "$HERE/ola.sh" score-v2 "$T/v2/corpus.tar" "$T/v2/SEALED.sha256" > "$T/s2.out" 2> "$T/s2.err" ||
  { bad "ola.sh score-v2 exited non-zero"; tail -5 "$T/s2.err"; }
V2R="$(ls -d "$E2"/runs/* | head -1)"
if diff <(awk '{print $1, $2, $3}' "$V2R/verdicts.txt") "$T/expected2" > "$T/vdiff2"; then ok "score-v2: every v2 sample case gets its expected verdict and reason (12 cases, 8 oscir)"
else bad "v2 sample verdicts"; cat "$T/vdiff2"; fi
rc_detail() { awk -v id="$1" '$1 == id {print $4}' "$V2R/verdicts.txt" | xargs -I{} grep -h '^detail=\|^kind=' "$E2/receipts/{}" | tr '\n' ' '; }
if [ "$(rc_detail x03)" = "kind=oscir detail=class_TRUNCATED " ] && [ "$(rc_detail x04)" = "kind=oscir detail=class_TRAILING " ] &&
   [ "$(rc_detail x05)" = "kind=oscir detail=class_MAGIC " ]; then ok "decode refusals carry the decoder's class in the receipt (TRUNCATED, TRAILING, MAGIC), kind=oscir"
else bad "decode refusal detail: x03 '$(rc_detail x03)' x04 '$(rc_detail x04)'"; fi
cmp -s "$V2R/verdicts.txt" "$V2R/replay/verdicts.txt" && grep -qx 'PROBES PASS count=15' "$V2R/probes.log" &&
  ok "score-v2: clean replay identical, 15 probes PASS" || bad "score-v2 replay/probes"
printf 'defects_rejected=7/8 controls_accepted=4/4 errors=1\nlabelled_defects=8 labelled_controls=4 verdicts_matched=12 unlabelled_verdicts=0\nreplay_identical=yes probes=PASS clean_tree=no\noscir_defects=6 oscir_controls=2\nGATE FAIL\n' > "$T/score2.want"
cmp -s "$V2R/score.txt" "$T/score2.want" && ok "score-v2 score.txt exact (counts from LABELS, oscir counts), gate FAIL" || { bad "score-v2 score.txt"; cat "$V2R/score.txt"; }
# v1 stays v1: the v1 layout refuses candidate.oscir, and v1 pass refuses kind oscir
v1_refuses_oscir() { local d="$T/v1r-$RANDOM$RANDOM"; mkdir -p "$d"; ! "$1" untar "$T/v2/corpus.tar" "$T/v2/SEALED.sha256" "$d" cases > /dev/null 2>&1; }
mkdir -p "$T/v2x" "$T/v2p/r" "$T/v2p/s"; "$BIN/ola" untar "$T/v2/corpus.tar" "$T/v2/SEALED.sha256" "$T/v2x" cases2 > /dev/null
rm -rf "$T/v2x/corpus/cases/s10"
"$BIN/ola" pass "$T/v2x/corpus" "$T/v2p/v" "$T/v2p/r" "$T/v2p/s" "$BIN/ola_runner" "$T/meta" 2>/dev/null
if v1_refuses_oscir "$BIN/ola" && [ "$(awk '$1 ~ /^x0[1-8]$/ && $2 == "ERROR" && $3 == "case_malformed"' "$T/v2p/v" | wc -l)" -eq 8 ] &&
   grep -qx 's01 ACCEPT ok .*' "$T/v2p/v"; then ok "v1 unchanged: v1 untar refuses candidate.oscir, v1 pass gives kind oscir ERROR (and still accepts s01)"
else bad "v1 admits v2"; fi
# tally2: gate needs every label right AND >= 10 oscir DEFECT and >= 5 oscir CONTROL cases
mk2t() { # mk2t <defect verdict for d01> <kind of d10> -> $T/t2v $T/t2l $T/t2c
  rm -rf "$T/t2c"; : > "$T/t2v"; : > "$T/t2l"
  for i in $(seq -w 1 10); do echo "d$i DEFECT x" >> "$T/t2l"; echo "d$i $([ "$i" = 01 ] && echo "$1" || echo REJECT) decode_refused h" >> "$T/t2v"
    mkdir -p "$T/t2c/cases/d$i"; printf 'kind %s\nentry f\nparams u8\nret u8\n' "$([ "$i" = 10 ] && echo "$2" || echo oscir)" > "$T/t2c/cases/d$i/requirement"; done
  for i in 1 2 3 4 5; do echo "c$i CONTROL -" >> "$T/t2l"; echo "c$i ACCEPT ok h" >> "$T/t2v"
    mkdir -p "$T/t2c/cases/c$i"; printf 'kind oscir\nentry f\nparams u8\nret u8\n' > "$T/t2c/cases/c$i/requirement"; done
}
g2() { "$1" tally2 "$T/t2v" "$T/t2l" 1 "$PL" 1 "$T/t2c" | tail -1; }
tally2_ok() {
  mk2t REJECT oscir; [ "$(g2 "$1")" = "GATE PASS" ] || return 1
  mk2t ACCEPT oscir; [ "$(g2 "$1")" = "GATE FAIL" ] || return 1
  mk2t REJECT osc;   [ "$(g2 "$1")" = "GATE FAIL" ] || return 1   # only 9 oscir defects
  mk2t REJECT oscir; [ "$("$1" tally2 "$T/t2v" "$T/t2l" 1 "$PL" 0 "$T/t2c" | tail -1)" = "GATE FAIL" ] || return 1
  return 0
}
tally2_ok "$BIN/ola" && ok "tally2: 10+5 oscir cases all right -> PASS; a missed defect, 9 oscir defects, or a dirty tree -> FAIL" || bad "tally2"

# ---- experiment task corpus ---------------------------------------------------------------------------
mktasks() { # mktasks <dir> [hook]: dir/tasks.tar + dir/TASKS.sha256 from sample/tasks
  local d="$1" hook="${2:-}"; mkdir -p "$d"; cp -r "$S/tasks" "$d/"
  if [ -n "$hook" ]; then (cd "$d" && eval "$hook"); fi
  (cd "$d" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf tasks.tar tasks &&
     sha256sum tasks.tar > TASKS.sha256)
}
mktasks "$T/tk"
# public view: requirement + prompt only
public_ok() { local d="$T/pub-$RANDOM$RANDOM"; mkdir -p "$d"
  "$1" untar "$T/tk/tasks.tar" "$T/tk/TASKS.sha256" "$d" public > /dev/null 2>&1 || return 1
  [ "$(cd "$d" && find . -type f | LC_ALL=C sort | tr '\n' ' ')" = "./tasks/cases/tadd/prompt ./tasks/cases/tadd/requirement ./tasks/cases/tquot/prompt ./tasks/cases/tquot/requirement " ]; }
mkdir -p "$T/pubsh"
if "$HERE/ola.sh" tasks-public "$T/tk/tasks.tar" "$T/tk/TASKS.sha256" "$T/pubsh/out" > /dev/null 2>&1 && [ ! -e "$T/pubsh/out/tasks/cases/tadd/hidden" ] &&
   [ -f "$T/pubsh/out/tasks/cases/tadd/prompt" ] && public_ok "$BIN/ola"; then ok "tasks-public: only requirement + prompt of every task, nothing hidden"
else bad "tasks-public"; fi
mktasks "$T/tkbad" 'ln -s ../tadd/prompt tasks/cases/tquot/notes'
if ! "$HERE/ola.sh" tasks-public "$T/tkbad/tasks.tar" "$T/tkbad/TASKS.sha256" "$T/pubsh/bad" > /dev/null 2>&1 && [ -z "$(ls -A "$T/pubsh/bad" 2>/dev/null)" ]; then
  ok "tasks-public refuses a task tarball with a member outside the layout, nothing extracted"; else bad "tasks-public bad tar"; fi
# candidate sets: text (one missing), bytes (one truncated), structured (empty set)
C="$T/cands"; mkdir -p "$C/a-text-1/tadd" "$C/a-text-1/tquot" "$C/a-bytes-1/tadd" "$C/a-bytes-1/tquot" "$C/a-structured-1"
cp "$S/cases/s01/candidate.osc" "$C/a-text-1/tadd/candidate.osc"
cp "$T/ir/s01.oscir" "$C/a-bytes-1/tadd/candidate.oscir"; cp "$T/ir/trunc3.oscir" "$C/a-bytes-1/tquot/candidate.oscir"
E3="$T/evid3"
OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$E3" "$HERE/ola.sh" experiment "$T/tk/tasks.tar" "$T/tk/TASKS.sha256" "$C" > "$T/x.out" 2> "$T/x.err" ||
  { bad "ola.sh experiment exited non-zero"; tail -5 "$T/x.err"; }
XR="$(ls -d "$E3"/runs/* | head -1)"
printf 'set=a-bytes-1 accepted=1/2 rejected=1 errors=0 no_candidate=0\nset=a-structured-1 accepted=0/2 rejected=2 errors=0 no_candidate=2\nset=a-text-1 accepted=1/2 rejected=1 errors=0 no_candidate=1\nreferences_accepted=2/2\nreplay_identical=yes probes=PASS clean_tree=no\nRUN INVALID\n' > "$T/xscore.want"
cmp -s "$XR/score.txt" "$T/xscore.want" && ok "experiment score.txt exact: 3 sets + references, no_candidate counted, RUN INVALID (selftest tree)" ||
  { bad "experiment score.txt"; cat "$XR/score.txt"; }
xv() { awk '{print $1, $2, $3}' "$XR/sets/$1/verdicts.txt" | tr '\n' ' '; }
if [ "$(xv a-bytes-1)" = "tadd ACCEPT ok tquot REJECT decode_refused " ] && [ "$(xv a-text-1)" = "tadd ACCEPT ok tquot REJECT no_candidate " ] &&
   [ "$(xv references)" = "tadd ACCEPT ok tquot ACCEPT ok " ]; then ok "experiment verdicts: oscir accepted, truncated oscir decode_refused, missing candidate no_candidate, references accepted"
else bad "experiment verdicts: bytes '$(xv a-bytes-1)' text '$(xv a-text-1)'"; fi
nc_rc="$E3/receipts/$(awk '$1 == "tquot" {print $4}' "$XR/sets/a-text-1/verdicts.txt")"
grep -qx 'candidate_sha256=-' "$nc_rc" && grep -qx 'kind=-' "$nc_rc" && grep -qx 'reason=no_candidate' "$nc_rc" &&
  ok "no_candidate receipt: candidate_sha256=-, kind=-" || bad "no_candidate receipt"
ok_replay=1; for s in references a-bytes-1 a-structured-1 a-text-1; do cmp -s "$XR/sets/$s/verdicts.txt" "$XR/sets/$s/replay/verdicts.txt" || ok_replay=0; done
[ "$ok_replay" = 1 ] && grep -qx 'PROBES PASS count=15' "$XR/probes.log" && [ "$(wc -l < "$XR/candidates.sha256")" -eq 3 ] &&
  ok "experiment: every set replayed identically, 15 probes PASS (read_labels aimed at a reference), candidate hashes logged" || bad "experiment replay/probes"
# whole-run refusals of a bad candidates dir (nothing scored, no run directory)
xrefused() { # xrefused <label> <setup commands run inside a fresh candidates dir>
  local d="$T/xbad-$RANDOM$RANDOM"; mkdir -p "$d/c" "$d/e"; (cd "$d/c" && eval "$2")
  if OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$d/e" "$HERE/ola.sh" experiment "$T/tk/tasks.tar" "$T/tk/TASKS.sha256" "$d/c" > /dev/null 2> "$d/err"; then bad "experiment accepted: $1"
  elif [ -n "$(ls -A "$d/e")" ] || ! grep -q refused "$d/err"; then bad "experiment $1: run dir created or no reason"
  else ok "experiment refused: $1"; fi
}
xrefused "unknown task id" 'mkdir -p s/nosuch'
xrefused "two candidate files for one task" 'mkdir -p s/tadd && touch s/tadd/candidate.osc s/tadd/candidate.oscir'
xrefused "a set named references" 'mkdir -p references/tadd'
xrefused "a stray file" 'mkdir -p s/tadd && touch s/tadd/notes.txt'
xrefused "a symlinked candidate" "mkdir -p s/tadd && ln -s '$S/cases/s01/candidate.osc' s/tadd/candidate.osc"
mktasks "$T/tkseal"; sed -i 's/^./0/' "$T/tkseal/TASKS.sha256"; [ "$(cut -c1 "$T/tk/TASKS.sha256")" = 0 ] && sed -i 's/^./1/' "$T/tkseal/TASKS.sha256"
if OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid-seal" "$HERE/ola.sh" experiment "$T/tkseal/tasks.tar" "$T/tkseal/TASKS.sha256" "$C" > /dev/null 2> "$T/xseal.err"; then bad "experiment on a tasks.tar not matching TASKS.sha256"
else grep -q "does not match the sealed hash" "$T/xseal.err" && ok "experiment refused: tasks.tar hash differs from TASKS.sha256" || bad "experiment seal reason"; fi
# the trusted C side refuses the same sets on its own (second line of defence)
mkdir -p "$T/xt"; "$BIN/ola" untar "$T/tk/tasks.tar" "$T/tk/TASKS.sha256" "$T/xt" tasks > /dev/null
mkdir -p "$T/xs-unk/nosuch" "$T/xs-two/tadd"; touch "$T/xs-two/tadd/candidate.osc" "$T/xs-two/tadd/candidate.oscir"
xpass_refuses() { local d="$T/xp-$RANDOM$RANDOM"; mkdir -p "$d/r" "$d/s"
  ! "$1" xpass "$T/xt/tasks" "$2" "$d/v" "$d/r" "$d/s" "$BIN/ola_runner" "$T/meta" > /dev/null 2>&1 && [ ! -e "$d/v" ]; }
xpass_refuses "$BIN/ola" "$T/xs-unk" && xpass_refuses "$BIN/ola" "$T/xs-two" && ok "ola xpass itself refuses an unknown task id and a second candidate, before writing verdicts" || bad "xpass refusals"
# malformed task cases: ERROR case_malformed with the reason in the receipt (invalidates the run)
xm_verdicts() { # xm_verdicts <ola> <hook> -> "id verdict reason detail ..." for set REFERENCES
  local d="$T/xm-$RANDOM$RANDOM"; mkdir -p "$d/r" "$d/s"; cp -r "$T/xt/tasks" "$d/"; (cd "$d/tasks/cases" && eval "$2")
  "$1" xpass "$d/tasks" REFERENCES "$d/v" "$d/r" "$d/s" "$BIN/ola_runner" "$T/meta" 2>/dev/null || return 0
  while read -r id v r h; do printf '%s %s %s %s ' "$id" "$v" "$r" "$(sed -n 's/^detail=//p' "$d/r/$h")"; done < "$d/v"; }
malformed_ok() {
  [ "$(xm_verdicts "$1" "printf 'add\tit\n' > tadd/prompt; sed -i 8d tquot/hidden/inputs")" = "tadd ERROR case_malformed prompt_bytes tquot ERROR case_malformed inputs_fewer_than_8 " ] &&
  [ "$(xm_verdicts "$1" 'rm tadd/hidden/reference.osc; sed -i "s/^kind task$/kind osc/" tquot/requirement')" = "tadd ERROR case_malformed missing_reference tquot ERROR case_malformed requirement_kind " ]
}
malformed_ok "$BIN/ola" && ok "malformed tasks: bad prompt byte, 7 inputs, missing reference, non-task kind -> ERROR case_malformed with the reason" || bad "malformed task checks"
# xscore: RUN VALID only with every reference accepted, no errors, equal set sizes, replay, probes, clean
xs_set() { mkdir -p "$T/xs/sets/$1"; printf '%s\n' "${@:2}" > "$T/xs/sets/$1/verdicts.txt"; }
xs() { "$1" xscore "$T/xs/sets" "${2:-1}" "${3:-$PL}" "${4:-1}" | tail -1; }
xscore_ok() {
  rm -rf "$T/xs"; xs_set references "a ACCEPT ok h" "b ACCEPT ok h"; xs_set s1 "a ACCEPT ok h" "b REJECT no_candidate h"
  [ "$(xs "$1")" = "RUN VALID" ] || return 1
  for f in "0 $PL 1" "1 $T/pl-subset 1" "1 $PL 0"; do [ "$("$1" xscore "$T/xs/sets" $f | tail -1)" = "RUN INVALID" ] || return 1; done
  xs_set references "a ACCEPT ok h" "b REJECT wrong_result h"; [ "$(xs "$1")" = "RUN INVALID" ] || return 1
  xs_set references "a ACCEPT ok h" "b ACCEPT ok h"; xs_set s1 "a ERROR case_malformed h" "b ACCEPT ok h"; [ "$(xs "$1")" = "RUN INVALID" ] || return 1
  xs_set s1 "a ACCEPT ok h"; [ "$(xs "$1")" = "RUN INVALID" ] || return 1
  return 0
}
xscore_ok "$BIN/ola" && ok "xscore: VALID only with all references accepted, zero errors, full sets, replay, every probe, clean tree" || bad "xscore"
# tasks-check: references only
OLA_ALLOW_DIRTY=1 OLA_EVIDENCE_ROOT="$T/evid4" "$HERE/ola.sh" tasks-check "$T/tk/tasks.tar" "$T/tk/TASKS.sha256" > "$T/tc.out" 2>/dev/null || bad "tasks-check exited non-zero"
[ "$(head -1 "$T/tc.out")" = "references_accepted=2/2" ] && [ "$(ls "$T"/evid4/runs/*/sets)" = references ] && ok "tasks-check: references only, 2/2 accepted" ||
  { bad "tasks-check"; cat "$T/tc.out"; }

xpass_nocand_ok() { local d="$T/xn-$RANDOM$RANDOM"; mkdir -p "$d/r" "$d/s"
  "$1" xpass "$T/xt/tasks" "$C/a-text-1" "$d/v" "$d/r" "$d/s" "$BIN/ola_runner" "$T/meta" > /dev/null 2>&1 || return 1
  [ "$(awk '{print $1, $2, $3}' "$d/v" | tr '\n' ' ')" = "tadd ACCEPT ok tquot REJECT no_candidate " ]; }
echo "v2 mutants (each must be caught):"
verdicts2_wrong() { local d="$T/m2-$RANDOM$RANDOM"; mkdir -p "$d/r" "$d/s" "$d/c"; "$BIN/ola" untar "$T/v2/corpus.tar" "$T/v2/SEALED.sha256" "$d/c" cases2 > /dev/null
  "$1" pass2 "$d/c/corpus" "$d/v" "$d/r" "$d/s" "${2:-$BIN/ola_runner}" "$T/meta" 2>/dev/null || return 0
  ! diff <(awk '{print $1, $2, $3}' "$d/v") "$T/expected2" > /dev/null; }
not verdicts2_wrong "$BIN/ola" && ok "  (pass2 on the v2 sample matches with the real harness)" || bad "pass2 baseline"
M="runner ignores a decoder refusal"; mut_runner no-decode '/OLA:oscir-decode/s/if (rc) {/if (0) {/' && caught verdicts2_wrong "$BIN/ola" "$T/mut/no-decode"
M="v1 layout admits candidate.oscir"; mut_ola lay-v2 '/OLA:lay-v2/s/g_lay == 1 \&\&/g_lay >= 0 \&\&/' && caught not v1_refuses_oscir "$T/mut/lay-v2"
M="public view leaks hidden files"; mut_ola public '/OLA:public/s/!public_mode ||/1 ||/' && caught not public_ok "$T/mut/public"
M="missing candidate not rejected as no_candidate"; mut_ola no-nocand '/OLA:no-candidate/s/else if (none)/else if (0)/' &&
  caught not xpass_nocand_ok "$T/mut/no-nocand"
M="set with an unknown task id admitted"; mut_ola set-ids '/OLA:set-ids/s/if (i == nid) die/if (0) die/' && caught not xpass_refuses "$T/mut/set-ids" "$T/xs-unk"
M="second candidate file admitted"; mut_ola set-one '/OLA:set-one/s/if (++n > 1) die/if (0) die/' && caught not xpass_refuses "$T/mut/set-one" "$T/xs-two"
M="prompt byte check off"; mut_ola prompt '/OLA:prompt/s/r = "prompt_bytes"; break;/break;/' && caught not malformed_ok "$T/mut/prompt"
M="8-input minimum off"; mut_ola inputs8 '/OLA:inputs-8/s/c.n < 8/c.n < 1/' && caught not malformed_ok "$T/mut/inputs8"
M="v2 gate ignores the oscir counts"; mut_ola v2gate '/OLA:v2-gate/s/ird >= 10 \&\& irc >= 5 \&\&//' && caught not tally2_ok "$T/mut/v2gate"
M="RUN VALID without every reference accepted"; mut_ola runvalid '/OLA:run-valid/s/ref_acc == ref_n \&\&//' && caught not xscore_ok "$T/mut/runvalid"
echo
if [ "$fails" -eq 0 ]; then echo "test-one-layer-acceptance: PASS (v1 + v2, sample corpora only; the sealed corpora are scored separately)"
else echo "test-one-layer-acceptance: FAIL ($fails)"; exit 1; fi
