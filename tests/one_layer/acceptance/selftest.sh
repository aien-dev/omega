#!/usr/bin/env bash
# tests/one_layer/acceptance/selftest.sh <build/one_layer dir> -- selftest of the stage-1 harness on
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
echo
if [ "$fails" -eq 0 ]; then echo "test-one-layer-acceptance: PASS (sample corpus only; the sealed corpus is scored separately)"
else echo "test-one-layer-acceptance: FAIL ($fails)"; exit 1; fi
