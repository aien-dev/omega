#!/usr/bin/env bash
# tools/gen/runner/test_gen_runner.sh -- CPU-only tests of the GEN driver and reducer (omega#477, GEN-2).
# Run by `make test-gen-runner` (builds build/gen/{genstat,fakegen}, x445, omega_emit, omegese, ola).
# No model, no GPU: the endpoint is fakegen on 127.0.0.1, the provider is stub_genret.sh, recheck and famsig
# are test stand-ins written below. Fixtures: 8 of the exp445 worked examples as a fake task pack
# (devgen01..devgen08, hidden data copied from the examples, a canary planted in every hidden file).
#   1 genstat: every row of Agent A's stats_vectors.tsv (sha256 pinned) and of stats_vectors_own.tsv
#   2 driver: every condition, manifests, prefix shared and byte-identical, suffix only in C and D,
#     greedy gate, G0, attempts 2-3, transport retry (500 once), timeout; replay sends nothing
#   3 reducer: harness experiment on the run's sets, decision receipt, identical rerun = identical aggregate
#   4 negatives: broken seeding -> pass@3 UNAVAILABLE; non-deterministic greedy -> no primary; interruption
#     and resume; refusals (hidden files in the task dir, mask on, changed manifest / model / prompt / params,
#     a stub provider or placeholders outside test mode, a dirty tree); mutants of prompt source, split,
#     seed, mask, model pin and verdict detected by the reducer; hidden-leak canary with positive control
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
B="$ROOT/build"
GS="$B/gen/genstat" FAKE="$B/gen/fakegen" X="$B/omega_emit/x445"
RUN="$HERE/run.sh" RED="$ROOT/tools/gen/reducer/reduce.sh" OLA="$ROOT/tests/one_layer/acceptance/ola.sh"
EXD="$ROOT/tools/exp445/arms/examples"
PASS=0 FAIL=0
ok() { PASS=$((PASS + 1)); echo "ok   $*"; }
bad() { FAIL=$((FAIL + 1)); echo "FAIL $*"; }
for f in "$GS" "$FAKE" "$X" "$B/omegese/omegese" "$B/one_layer/ola"; do [ -x "$f" ] || { echo "missing $f: run make test-gen-runner" >&2; exit 2; }; done
T="$(mktemp -d "${TMPDIR:-/tmp}/test-gen-runner.XXXXXXXX")"
FPIDS=""
cleanup() { for p in $FPIDS; do kill "$p" 2> /dev/null; done; if [ "${GEN_TEST_KEEP:-0}" = 1 ]; then echo "kept $T"; else rm -rf "$T"; fi; }
trap cleanup EXIT
sha() { sha256sum < "$1" | cut -d' ' -f1; }
export GEN_TEST=1 EXP445_TEST_BACKOFF_S=0 OLA_EVIDENCE_ROOT="$T/evid" OLA_ALLOW_DIRTY=1 # test evidence: clean_tree=no either way
# refuse <label> <expected stderr fragment> <command...>: the command must exit non-zero with that message
refuse() { local l="$1" m="$2"; shift 2; if "$@" > "$T/ref.out" 2> "$T/ref.err"; then bad "$l: accepted"; elif grep -qF -- "$m" "$T/ref.err"; then ok "$l: refused"; else bad "$l: refused for another reason: $(head -c 300 "$T/ref.err")"; fi; }

# ---- 1. statistics ---------------------------------------------------------------------------------------
SV="$ROOT/tools/gen/reducer/testdata/stats_vectors.tsv"
[ "$(sha "$SV")" = b5a9347c477d4930070579fbb7472784219593bb5c8ea512098f09380b1e4400 ] &&
  ok "Agent A's stats_vectors.tsv is the frozen file v2 (gen/protocol 7eb8534c, sha256 b5a9347c...)" || bad "stats_vectors.tsv sha256"
o="$("$GS" vectors "$SV" 2>&1)"; [[ "$o" == "vectors PASS: 114 rows "*", 0 failures" ]] && ok "genstat: $o" || bad "genstat vectors on Agent A's file: $o"
nown=0 fown=0
while IFS=$'\t' read -r cmd args want src; do
  case "$cmd" in ""|"#"*) continue ;; esac
  nown=$((nown + 1)); got="$("$GS" "$cmd" $args 2>&1)"; [ "$got" = "$want" ] || { fown=$((fown + 1)); echo "  own vector $cmd $args: got '$got', want '$want'"; }
done < "$ROOT/tools/gen/reducer/stats_vectors_own.tsv"
[ "$fown" = 0 ] && [ "$nown" -ge 10 ] && ok "genstat own vectors (exact McNemar fractions, x445 Wilson): $nown rows, 0 failures" || bad "own vectors: $fown of $nown failed"
sed "0,/0.0601718521/s//0.0611718521/" "$SV" > "$T/sv.mut"
if cmp -s "$SV" "$T/sv.mut"; then bad "vector mutant was not made"
elif "$GS" vectors "$T/sv.mut" > /dev/null 2>&1; then bad "genstat accepted a changed vector"; else ok "  (negative control: one changed Wilson vector value fails)"; fi
sed "s/^lenmatch\tl03\t\(.*\)\tmatched$/lenmatch\tl03\t\1\tunmatched/" "$SV" > "$T/sv.mut2"
if cmp -s "$SV" "$T/sv.mut2"; then bad "lenmatch mutant was not made"
elif "$GS" vectors "$T/sv.mut2" > /dev/null 2>&1; then bad "genstat accepted a changed lenmatch class"; else ok "  (negative control: lenmatch l03 matched -> unmatched fails)"; fi
mism=0; for n in 1 7 20 41 60; do for k in $(seq 0 "$n"); do [ "$("$GS" wilson4 "$k" "$n")" = "$("$X" wilson "$k" "$n")" ] || mism=$((mism + 1)); done; done
[ "$mism" = 0 ] && ok "genstat wilson4 = x445 wilson on every k for n in 1 7 20 41 60" || bad "wilson4 vs x445: $mism mismatches"

# ---- fixtures ---------------------------------------------------------------------------------------------
EXS=(clamp_sub count_digits dot3 has_digit narrow_sum rotl8 sum_range tri_kind)
CANARY="CANARY-$(head -c 8 /dev/urandom | od -An -tx1 | tr -d ' \n')"
mkdir -p "$T/src/tasks/cases"
i=0
for e in "${EXS[@]}"; do
  i=$((i + 1)); id="$(printf 'devgen%02d' "$i")"; d="$T/src/tasks/cases/$id"; mkdir -p "$d/hidden"
  sed '1s/.*/# GEN runner test task (omega#477), from an exp445 worked example/' "$EXD/$e/requirement" > "$d/requirement"
  cp "$EXD/$e/prompt" "$d/prompt"
  cp "$EXD/$e/inputs" "$d/hidden/inputs"; cp "$EXD/$e/expected" "$d/hidden/expected"
  { cat "$EXD/$e/program.osc"; printf '// %s\n' "$CANARY"; } > "$d/hidden/reference.osc"
done
pack() { mkdir -p "$2"; (cd "$1" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf "$2/tasks.tar" tasks); (cd "$2" && sha256sum tasks.tar > TASKS.sha256); }
pack "$T/src" "$T/pack"
if ! "$OLA" tasks-public "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$T/pub" > "$T/pub.log" 2>&1; then
  echo "ola.sh tasks-public failed: $(tail -n 3 "$T/pub.log")" >&2; exit 2; fi
# corpus of records (gen-record 1) with receipts, a recheck and a famsig stand-in
CR="$T/corpus/raw/records" CA="$T/corpus/abstraction/records"; mkdir -p "$CR/raw" "$CR/receipts" "$CA/abstraction" "$CA/receipts"
rec() { # rec <set dir> <kind> <id> <family> <example>: one verified corpus record whose body is the example's Omegese
  local C="$1" f="$1/$2/$3.rec" e="$5" r
  r="$(printf 'receipt for %s\n' "$3" | sha256sum | cut -d' ' -f1)"; printf 'receipt for %s\n' "$3" > "$C/receipts/$r"
  { echo "gen-record 1"; echo "id $3"; echo "kind $2"; echo "split corpus"; echo "family $4"
    grep -E '^(entry|params|ret)( |$)' "$EXD/$e/requirement"; echo "desc Test record GENTEST-KIND-$2 from the exp445 example $e."
    echo "receipt $r"; echo "validity verified"; echo body; cat "$EXD/$e/program.omegese"; } > "$f"
}
FS_TOK() { printf 'gs1:%s' "$1" | sha256sum | cut -c1-16 | sed 's/^/gs1-/'; }
rec "$CR" raw raw-power_mod gs1-1111111111111111 power_mod
rec "$CR" raw raw-hash_step gs1-2222222222222222 hash_step
rec "$CA" abstraction abs-sum_squares3 "$(FS_TOK 'loop(arith)')" sum_squares3
rec "$CA" abstraction abs-count_above gs1-3333333333333333 count_above
(cd "$CR" && sha256sum raw/raw-power_mod.rec raw/raw-hash_step.rec) > "$CR/MANIFEST"; sha "$CR/MANIFEST" > "$CR/SET.sha256"
(cd "$CA" && sha256sum abstraction/abs-sum_squares3.rec abstraction/abs-count_above.rec) > "$CA/MANIFEST"; sha "$CA/MANIFEST" > "$CA/SET.sha256"
PJT="$HERE/testdata/protocol.json"
[ "$(sha "$PJT")" = a28142baeeb3d7218ddd3baea6bcaf2ae48de5e6eb1975a38845c008737ee0b8 ] &&
  ok "testdata/protocol.json is Agent A's frozen protocol.json (gen/protocol 7eb8534c, sha256 a28142ba...)" || bad "testdata/protocol.json sha256"
cat > "$T/recheck" <<'EOF'
#!/bin/sh
# test stand-in for Agent D's recheck: OK <id> for every record whose receipt file exists
[ "$1" = --receipts ] || exit 2; rd="$2"; shift 2
for f in "$@"; do id="$(sed -n 's/^id //p' "$f")"; r="$(sed -n 's/^receipt //p' "$f")"
  if [ -f "$rd/$r" ]; then echo "OK $id"; else echo "REFUSED $id receipt"; exit 1; fi; done
EOF
cat > "$T/famsig" <<'EOF'
#!/bin/sh
# test stand-in for famsig token (FAMSIG_V1 section 2 token rule, without canonicalisation)
[ "$1" = token ] || exit 2; printf 'gs1:%s' "$2" | sha256sum | cut -c1-16 | sed 's/^/gs1-/'
EOF
chmod +x "$T/recheck" "$T/famsig"; sha "$T/recheck" > "$T/recheck.sha256"
{ echo "gen-meta 1"; i=0; for e in "${EXS[@]}"; do i=$((i + 1)); printf 'devgen%02d dev 1 0 1 0 0 0 loop(arith)\n' "$i"; done; } > "$T/devmeta"
# answers: D right on every task, C right on devgen01 only, B right on devgen01 only, A right on devgen02
# only (A - B = 0); C's devgen06 compiles but computes the wrong value (checker OK, functional REJECT);
# B's devgen02 is refused, then repaired (secondary set b-1-final); A's devgen03 gets one HTTP 500, then the answer
A="$T/ans"; mkdir -p "$A/A" "$A/B" "$A/C" "$A/D" "$A/any"
fence() { printf '```\n%s\n```\n' "$(cat "$1")"; }
wrong() { printf '```\n%s\n```\n' "$(sed -n '1p' "$EXD/$1/program.omegese")
ret 0
end"; }
for e in "${EXS[@]}"; do
  fence "$EXD/$e/program.omegese" > "$A/D/$e"
  printf 'I cannot do this.\n' > "$A/any/$e"
done
fence "$EXD/clamp_sub/program.omegese" > "$A/C/clamp_sub"; fence "$EXD/clamp_sub/program.omegese" > "$A/B/clamp_sub"
fence "$EXD/count_digits/program.omegese" > "$A/A/count_digits"
printf '```\nfn rotl8 x uchar k uchar to uchar\nret x\nend\n```\n' > "$A/C/rotl8"
printf '```\nfn count_digits n uint to\n```\n' > "$A/B/count_digits"; fence "$EXD/count_digits/program.omegese" > "$A/B/count_digits.repair"
{ printf '@@500ONCE\n'; fence "$EXD/dot3/program.omegese"; } > "$A/A/dot3"
printf '{"object":"list","data":[{"id":"qwen3-30b-a3b-instruct-2507","object":"model"}]}\n' > "$A/models.json"
printf '{"p_repeat": 0.5, "seed_salt": true}\n' > "$A/sampling.json"
startfake() { : > "$1"; "$FAKE" "$1" "$2" & FPIDS="$FPIDS $!"; for _ in $(seq 1 50); do [ -s "$1" ] && break; sleep 0.1; done; printf -v "$3" "http://127.0.0.1:%s/v1" "$(cat "$1")"; }
startfake "$T/port" "$A" URL
# test EXP445 PARAMS: config A pointed at fakegen, stand-in server and weights (the model id stays the pin)
mkdir -p "$T/models/m"; printf 'w1\n' > "$T/models/m/a.bin"; (cd "$T/models" && sha256sum m/a.bin) > "$T/wl.sha256"
sed -e "s|^config\.A\.base_url=.*|config.A.base_url=$URL|" -e 's|^config\.A\.server=.*|config.A.server=fakegen test server|' \
    -e "s|^models_root=.*|models_root=$T/models|" -e "s|^config\.A\.weights=.*|config.A.weights=$T/wl.sha256|" "$ROOT/tools/exp445/PARAMS" > "$T/E445"
grep -q "^config.A.base_url=$URL\$" "$T/E445" || { echo "test EXP445 PARAMS not pointed at fakegen" >&2; exit 2; }
# test GEN PARAMS
gp() { # gp <out file> [sed expressions...]
  local o="$1"; shift
  sed -e "s|^exp445_params=.*|exp445_params=$T/E445|" -e "s|^protocol=.*|protocol=-|" -e "s|^examples_dir=.*|examples_dir=$EXD|" \
      -e "s|^provider=.*|provider=$HERE/stub_genret.sh|" -e "s|^corpus_manifest_raw=.*|corpus_manifest_raw=$CR/MANIFEST|" \
      -e "s|^corpus_manifest_raw_sha256=.*|corpus_manifest_raw_sha256=$(cat "$CR/SET.sha256")|" -e "s|^recheck=.*|recheck=$T/recheck|" -e "s|^recheck_sha256_file=.*|recheck_sha256_file=$T/recheck.sha256|" -e "s|^recheck_source_sha256=.*|recheck_source_sha256=$(printf %064d 0)|" \
      -e "s|^corpus_manifest_abstraction=.*|corpus_manifest_abstraction=$CA/MANIFEST|" -e "s|^corpus_manifest_abstraction_sha256=.*|corpus_manifest_abstraction_sha256=$(cat "$CA/SET.sha256")|" \
      -e "s|^protocol_json=.*|protocol_json=$PJT|" -e "s|^provider_contract_sha256=.*|provider_contract_sha256=$(printf '%064d' 0)|" \
      -e "s|^famsig=.*|famsig=$T/famsig|" -e "s|^gencheck=.*|gencheck=$T/gencheck|" -e "s|^dev_meta=.*|dev_meta=$T/devmeta|" -e "s|^dev_tasks=.*|dev_tasks=$T/src|" \
      -e 's/^greedy_gate_tasks=.*/greedy_gate_tasks=3/' -e 's/^timeout_s=.*/timeout_s=5/' -e 's/^revision=.*/revision=gen-v1-test/' "$@" "$HERE/PARAMS" > "$o"
}
gp "$T/P"
! grep -v "^#" "$T/P" | grep -q PLACEHOLDER && ok "test PARAMS: every placeholder replaced by a test path" || bad "test PARAMS still holds placeholders"
# gencheck stand-in (Agent A's gencheck is on gen/protocol, not merged): run-ready / open pass unless told to fail,
# and open checks that line 3 of OPEN names this protocol.json; run-open = run.sh with a seal-open receipt for the
# protocol.json that GEN_PARAMS names (5 lines, GEN_PROTOCOL_V1 section 8)
cat > "$T/gencheck" <<EOF2
#!/usr/bin/env bash
# test stand-in for gencheck run-ready <protocol.json> <root> | open <protocol.json> <root> <OPEN>
case "\$1" in
  run-ready) [ ! -e "$T/gc.fail.run-ready" ] || { echo "not run-ready: record_sets.raw_manifest_sha256 is null"; exit 1; }; echo "ok run-ready" ;;
  open) [ ! -e "$T/gc.fail.open" ] || { echo "OPEN: tasks_sha256 differs from protocol.json"; exit 1; }
        [ "\$(sed -n 3p "\$4")" = "protocol_sha256 \$(sha256sum < "\$2" | cut -c1-64)" ] || { echo "OPEN: protocol_sha256 differs"; exit 1; }; echo "ok open" ;;
  *) echo "usage" >&2; exit 2 ;;
esac
EOF2
cat > "$T/run-open" <<EOF2
#!/usr/bin/env bash
# run.sh --open <a seal-open receipt for GEN_PARAMS's protocol.json> ...
p="\$(sed -n 's/^protocol_json=//p' "\${GEN_PARAMS:-$HERE/PARAMS}")"
if [ -f "\$p" ]; then o="$T/\$(basename "\$p").OPEN"
  printf 'gen-open 1\nomega_commit %040d\nprotocol_sha256 %s\ntasks_sha256 %064d\nheldout_pub_sha256 %064d\n' 0 "\$(sha256sum < "\$p" | cut -c1-64)" 0 0 > "\$o"
  exec "$HERE/run.sh" --open "\$o" "\$@"
fi
exec "$HERE/run.sh" "\$@"
EOF2
chmod +x "$T/gencheck" "$T/run-open"; RUN="$T/run-open"
cat > "$T/red-open" <<EOF2
#!/usr/bin/env bash
# reduce.sh --open <the run's own OPEN copy> --gencheck <the stand-in> ...
r=""; a=("\$@"); for i in "\${!a[@]}"; do [ "\${a[\$i]}" != --run ] || r="\${a[\$((i + 1))]}"; done
exec "$RED" --open "\$r/OPEN" --gencheck "$T/gencheck" "\$@"
EOF2
chmod +x "$T/red-open"; REDX="$RED"; RED="$T/red-open"
export GEN_PARAMS="$T/P"
cw() { EXP445_PARAMS="$T/E445" "$ROOT/tools/exp445/check_weights.sh" A "$1" > /dev/null 2>&1; }
greq() { cat "$A/requests.log" 2> /dev/null | wc -l; }

# ---- 2. driver: the full run --------------------------------------------------------------------------------
R="$T/run1"; cw "$R" || { echo "check_weights failed" >&2; exit 2; }
if "$RUN" --tasks "$T/pub" --out "$R" > "$T/run1.out" 2> "$T/run1.err"; then ok "run.sh: full run (4 conditions x 8 tasks x 3 attempts) exit 0"
else bad "run.sh full run failed: $(tail -n 3 "$T/run1.err")"; fi
lg() { awk -F'\t' -v s="$2" -v t="$3" -v c="$4" '$1 == s && $2 == t { print $c }' "$1/log.tsv"; }
mfk() { sed -n "s/^$3=//p" "$1/manifests/$(awk -F'\t' -v c="$2" -v t="$4" '$1 == c && $2 == t { print $3 }' "$1/manifests.tsv")"; }
[ "$(tail -n 1 "$R/gate/greedy/verdict.txt")" = "GREEDY PASS" ] && [ "$(tail -n 1 "$R/gate/g0/verdict.txt")" = "G0 PASS" ] && grep -qx 'pass3=AVAILABLE' "$R/gate/g0/verdict.txt" &&
  [ "$(($(wc -l < "$R/log.tsv") - 1))" = 96 ] && [ "$(ls "$R/cands" | wc -l)" = 24 ] &&
  ok "greedy gate PASS, G0 PASS, 96 run rows, 24 candidate sets (<c>-<attempt> and <c>-<attempt>-final)" || bad "full run record"
[ "$(mfk "$R" B prefix_sha256 devgen01)" = "$(mfk "$R" C prefix_sha256 devgen05)" ] && [ "$(mfk "$R" B prefix_sha256 devgen01)" = "$(mfk "$R" D prefix_sha256 devgen08)" ] &&
  [ "$(mfk "$R" A prefix_sha256 devgen01)" != "$(mfk "$R" B prefix_sha256 devgen01)" ] && [ "$(mfk "$R" A suffix_bytes devgen01)" = 0 ] && [ "$(mfk "$R" B suffix_bytes devgen01)" = 0 ] &&
  [ "$(mfk "$R" C retrieved devgen01 | cut -d: -f2 | head -c 4)" = raw- ] && [[ "$(mfk "$R" D retrieved devgen01)" == *abs-sum_squares3*abs-count_above* ]] && [ "$(mfk "$R" C suffix_bytes devgen01)" -gt 0 ] &&
  ok "manifests: B C D share one prefix, A has its own; A and B carry no suffix; C retrieves raw records, D abstraction records" || bad "manifest prefix / suffix"
sysok=1
for s in a-1 b-1 c-1 d-1; do for t in devgen01 devgen07; do
  c="$(tr a-z A-Z <<< "${s%-1}")"; q="$R/gen/$s/$t/r1.req.json"
  sys="$(jq -j '.messages[0].content' "$q"; printf x)"; sys="${sys%x}"; pre="$(cat "$R/blobs/$(mfk "$R" "$c" prefix_sha256 "$t")")"
  [ "${sys:0:${#pre}}" = "$pre" ] && [ "$(printf '%s' "$sys" | sha256sum | cut -d' ' -f1)" = "$(mfk "$R" "$c" system_sha256 "$t")" ] || sysok=0
  case "$c" in C) grep -q GENTEST-KIND-raw "$q" && ! grep -q GENTEST-KIND-abstraction "$q" || sysok=0 ;; D) grep -q GENTEST-KIND-abstraction "$q" && ! grep -q GENTEST-KIND-raw "$q" || sysok=0 ;;
    *) ! grep -q GENTEST-KIND "$q" || sysok=0 ;; esac
done; done
[ "$sysok" = 1 ] && ok "requests: system = shared prefix, then the retrieved suffix (sha256 as the manifest); retrieved records only in C (raw) and D (abstraction)" || bad "request system messages"
[ "$(lg "$R" a-1 devgen03 21)" = 2 ] && [ "$(grep -c 'http=500' "$R/gen/a-1/devgen03/r1.transport")" = 1 ] && [ "$(lg "$R" a-1 devgen03 10)" = OK ] && [ ! -e "$R/gen/a-1/devgen03/r2.req.json" ] &&
  ok "transport retry: one HTTP 500 then the answer, 2 tries in one round (no new sample, no repair)" || bad "transport retry record"
[ "$(lg "$R" b-2 devgen03 15)" = 478003 ] && [ "$(lg "$R" b-1 devgen03 15)" = 477003 ] && [ "$(lg "$R" b-1 devgen03 16)" = 0 ] && [ "$(lg "$R" b-2 devgen03 16)" = 0.7 ] &&
  [ "$(lg "$R" b-2 devgen03 23)" = sampled ] && [ "$(lg "$R" b-1 devgen03 23)" = greedy ] &&
  ok "seeds 476000 + 1000 x attempt + task index; attempt 1 greedy (temperature 0), attempts 2-3 sampled at 0.7" || bad "seed / sampling rule in run rows"
[ "$(lg "$R" b-1 devgen02 11)" = OK ] && [ ! -e "$R/gen/b-1/devgen02/r1.candidate.oscir" ] && [ "$(lg "$R" b-1 devgen02 27)" = "$(sha "$R/gen/b-1/devgen02/r1.content")" ] && cmp -s "$R/cands/b-1/devgen02/candidate.oscir" "$R/gen/b-1/devgen02/r1.content" && [ -f "$R/cands/b-1-final/devgen02/candidate.oscir" ] &&
  ok "repair round: a round-1 reply with no program is itself the b-1 candidate (graded, fails); the repaired one goes to b-1-final" || bad "repair round sets"
n0="$(greq)"; s0="$(sha "$R/SHA256SUMS")"
"$RUN" --tasks "$T/pub" --out "$R" > /dev/null 2> "$T/replay.err" && [ "$(greq)" = "$n0" ] && [ "$(sha "$R/SHA256SUMS")" = "$s0" ] &&
  ok "replay: rerun on the finished run sends 0 requests and leaves SHA256SUMS identical" || bad "replay idempotence ($(greq) vs $n0 requests): $(tail -n 2 "$T/replay.err")"
# hidden-leak canary: never in any request, prompt, retrieval query, manifest or log
leak() { grep -rlF "$CANARY" "$@" 2> /dev/null | head -n 1; }
[ -z "$(leak "$R" "$A/requests.log" "$T/run1.err" "$T/run1.out" "$T/replay.err" "$T/pub")" ] && [ -n "$(leak "$T/src")" ] &&
  ok "hidden canary: absent from the run dir (requests, manifests, blobs, retrieval queries), the server log and the driver logs; present in the hidden files" || bad "hidden canary leaked: $(leak "$R" "$A/requests.log" "$T/run1.err")"

# ---- 3. harness + reducer ------------------------------------------------------------------------------------
hrun() { # hrun <cands dir> -> prints the harness run dir
  ls "$T/evid/runs" 2> /dev/null | LC_ALL=C sort > "$T/hb.lst"
  "$OLA" experiment "$T/pack/tasks.tar" "$T/pack/TASKS.sha256" "$1" > "$T/h.out" 2> "$T/h.err" || return 1
  ls "$T/evid/runs" | LC_ALL=C sort | comm -13 "$T/hb.lst" - | sed "s|^|$T/evid/runs/|"
}
H1="$(hrun "$R/cands")" && [ -f "$H1/score.txt" ] && ok "harness experiment on the run's 24 sets: $(tail -n 2 "$H1/score.txt" | tr '\n' ' ')" || bad "harness experiment failed: $(tail -n 2 "$T/h.err")"
{ echo "model=qwen3-30b-a3b-instruct-2507"; echo "mask=off"; echo "tasks_public_sha256=$(sed -n 's/^tasks_public_sha256=//p' "$R/meta.txt")"
  echo "prefix_sha256.B=$(sed -n 's/^prefix_sha256.B=//p' "$R/meta.txt")"; echo "tasks_corpus_sha256=$(sed -n 's/^corpus_sha256=//p' "$H1/meta.txt")"; } > "$T/pins"
gk() { sed -n "s/^$2=//p" "$1/aggregate.txt"; }
"$RED" --run "$R" --harness "$H1" --pins "$T/pins" --out "$T/red1" > "$T/red1.log" 2>&1
if [ "$(gk "$T/red1" verdict)" = PASS ] && [ "$(gk "$T/red1" test_mode)" = yes ]; then ok "reducer: verdict PASS (D 8/8 vs A 2/8, B and C 1/8; exact one-sided McNemar 1/128), marked test mode"
else bad "reducer verdict $(gk "$T/red1" verdict): $(grep '^reason' "$T/red1/aggregate.txt" | head -n 3 | tr '\n' ' ')"; fi
[ "$(gk "$T/red1" arm.D.success_a1)" = 8/8 ] && [ "$(gk "$T/red1" arm.C.success_a1)" = 1/8 ] && [ "$(gk "$T/red1" arm.C.checker_ok_a1)" = 2/8 ] &&
  [ "$(gk "$T/red1" arm.B.success_a1_final)" = 2/8 ] && [ "$(gk "$T/red1" pass3)" = AVAILABLE ] && [ "$(gk "$T/red1" arm.D.pass3)" = 8/8 ] &&
  [ "$(gk "$T/red1" pair.D-C.mcnemar_p_one_sided)" = 0.007812 ] && [ "$(gk "$T/red1" paired.devgen06)" = 0001 ] && [ "$(gk "$T/red1" sanity.a_minus_b)" = 1 ] && [ "$(gk "$T/red1" arm.A.success_a1)" = 2/8 ] &&
  [ "$(gk "$T/red1" arm.D.wilson95_lo)" = "$("$GS" wilson 8 8 | cut -d' ' -f2)" ] && [ "$(grep "^fail\.C\." "$T/red1/aggregate.txt" | cut -d= -f2 | paste -sd+ | bc)" = 7 ] &&
  [ "$(gk "$T/red1" check.C.OK)" = 2 ] &&
  ok "receipt: success per arm, checker acceptance apart from functional success (C 2/8 vs 1/8), repair secondary, pass@3, paired bits, failure and check tables" ||
  { bad "receipt values"; grep -E '^(arm\.C|fail\.C|check\.C|pair\.D-C\.mc|paired\.devgen06)' "$T/red1/aggregate.txt" | head -n 20; }
(cd "$T/red1" && sha256sum -c --quiet receipt.sha256) && grep -qx "aggregate_sha256=$(sha "$T/red1/aggregate.txt")" "$T/red1/receipt.txt" &&
  [ "$(head -n "$(wc -l < "$T/red1/aggregate.txt")" "$T/red1/receipt.txt")" = "$(cat "$T/red1/aggregate.txt")" ] &&
  ok "receipt.txt = aggregate.txt + time / input lines + aggregate_sha256; receipt.sha256 verifies" || bad "receipt layout"
H2="$(hrun "$R/cands")" && "$RED" --run "$R" --harness "$H2" --pins "$T/pins" --out "$T/red2" > /dev/null 2>&1 && [ "$H1" != "$H2" ] &&
  cmp -s "$T/red1/aggregate.txt" "$T/red2/aggregate.txt" && ok "identical rerun (new harness run, new reduce): byte-identical aggregate.txt" || bad "rerun aggregate differs"

# ---- 4. negatives -------------------------------------------------------------------------------------------
jq '.generation.attempts = 1' "$PJT" > "$T/pj1.json"
gp "$T/P1" -e "s|^protocol_json=.*|protocol_json=$T/pj1.json|" -e 's/^attempts=.*/attempts=1/'
sampling() { printf '%s\n' "$1" > "$A/sampling.json"; }
nb=0
# broken seeding: the server ignores the seed (G0 FAIL), forces greedy (G0 FAIL); pass@3 UNAVAILABLE
for mode in '"ignore_seed": true' '"force_greedy": true'; do
  sampling "{\"p_repeat\": 0.5, \"seed_salt\": true, $mode}"; o="$T/brk$((++nb))"; cw "$o"
  "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$o.err"; e=$?
  [ "$e" = 0 ] && [ "$(tail -n 1 "$o/gate/g0/verdict.txt")" = "G0 FAIL" ] && grep -qx 'pass3=UNAVAILABLE (attempts 2..3 not run)' "$o/gate/g0/verdict.txt" &&
    [ "$(($(wc -l < "$o/log.tsv") - 1))" = 32 ] && [ ! -e "$o/gen/a-2" ] && ok "broken seeding ($mode): G0 FAIL, attempts 2-3 not run, pass@3 UNAVAILABLE" || bad "broken seeding ($mode): exit $e $(tail -n 1 "$o.err")"
done
sampling '{"p_repeat": 0.5, "seed_salt": true}'
HB="$(hrun "$T/brk1/cands")" && "$RED" --run "$T/brk1" --harness "$HB" --out "$T/redb" > /dev/null 2>&1 &&
  [ "$(gk "$T/redb" pass3)" = UNAVAILABLE ] && [ "$(gk "$T/redb" gate.g0)" = FAIL ] && [ "$(gk "$T/redb" arm.D.pass3)" = UNAVAILABLE ] && [ "$(gk "$T/redb" verdict)" = PASS ] &&
  ok "reducer on the broken-seeding run: gate.g0=FAIL, pass3=UNAVAILABLE for every arm, primary unaffected" || bad "reducer on broken seeding: $(gk "$T/redb" verdict) $(gk "$T/redb" reason.1)"
# non-deterministic greedy decoding: greedy gate FAIL, no primary attempt
sampling '{"p_repeat": 0.5, "seed_salt": true, "nondet_greedy": true}'; o="$T/ndg"; cw "$o"
"$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$o.err"; e=$?
[ "$e" = 1 ] && [ "$(tail -n 1 "$o/gate/greedy/verdict.txt")" = "GREEDY FAIL" ] && [ ! -e "$o/gen" ] && [ -f "$o/SHA256SUMS" ] &&
  ok "non-deterministic greedy replies: greedy gate FAIL, exit 1, no generation sent" || bad "greedy gate negative: exit $e"
sampling '{"p_repeat": 0.5, "seed_salt": true}'
# interruption and resume: killed after round 1 of b-1 devgen02 (before its repair) and after c-1 devgen03's row
export GEN_PARAMS="$T/P1"
o="$T/full1"; cw "$o"; n0="$(greq)"; "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2>&1; nfull=$(($(greq) - n0))
o="$T/kill"; cw "$o"; n0="$(greq)"
GEN_TEST_KILL=after-r1:b-1:devgen02 "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2>&1; e1=$?
GEN_TEST_KILL=after-log:c-1:devgen03 "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2>&1; e2=$?
"$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$T/kill.err"; e3=$?
[ "$e1" = 137 ] && [ "$e2" = 137 ] && [ "$e3" = 0 ] && [ "$(($(greq) - n0))" = "$nfull" ] && [ "$(($(wc -l < "$o/log.tsv") - 1))" = 32 ] &&
  [ "$(cut -f1,2 "$o/log.tsv" | sort | uniq -d | wc -l)" = 0 ] && [ "$(cut -f1-8,10-25,27- "$o/log.tsv" | grep -v '^set' | sort)" = "$(cut -f1-8,10-25,27- "$T/full1/log.tsv" | grep -v '^set' | sort)" ] &&
  ok "interrupted twice (killed mid-generation and after a row), resumed: same $nfull requests as an uninterrupted run, no duplicate row, same rows" ||
  bad "interruption / resume: exits $e1 $e2 $e3, requests $(($(greq) - n0)) vs $nfull: $(tail -n 1 "$T/kill.err")"
# a request timeout is a transport failure of the round (tries recorded), not a new sample; on attempt 1 it makes
# the run INVALID and the driver stops sending (protocol, GEN-1 review)
printf '@@SLEEP 7\n' > "$A/B/tri_kind"
o="$T/tmo"; cw "$o"; nq0="$(grep -cxF -- "$(cat "$R/gen/b-1/devgen08/r1.req.json")" "$A/requests.log")"; "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$o.err"; e=$?
[ "$e" != 0 ] && [ "$(lg "$o" b-1 devgen08 10)" = "REQUEST FAILED" ] && [ "$(lg "$o" b-1 devgen08 21)" = 3 ] && [ ! -e "$o/cands/b-1/devgen08" ] &&
  [ -z "$(lg "$o" c-1 devgen08 1)" ] && grep -q 'run INVALID, no further requests' "$o.err" && (cd "$o" && sha256sum -c --quiet SHA256SUMS) &&
  ok "attempt-1 timeout: 3 identical tries in one round, REQUEST FAILED, no candidate, run INVALID, nothing sent after it" ||
  bad "timeout record: exit $e, $(lg "$o" b-1 devgen08 10) tries $(lg "$o" b-1 devgen08 21): $(tail -n 1 "$o.err")"
q="$(cat "$o/gen/b-1/devgen08/r1.req.json")"; [ "$(grep -cxF -- "$q" "$A/requests.log")" = "$((nq0 + 3))" ] && [ "$(grep -c . "$o/gen/b-1/devgen08/r1.transport")" = 3 ] && ok "  (the 3 tries sent the one recorded request)" || bad "timeout tries"
rm -f "$A/B/tri_kind"
"$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$o.err2"; e=$?
[ "$e" != 0 ] && grep -q 'run INVALID, no further requests' "$o.err2" && [ -z "$(lg "$o" c-1 devgen08 1)" ] && ok "  resume of that run: still refused, nothing sent" || bad "resume after an attempt-1 transport failure: exit $e"
# an attempt-1 reply that does not report mode=greedy: run INVALID, stops
{ printf '@@SAMPLEDMODE\n'; cat "$A/D/sum_range"; } > "$A/A/sum_range"
o="$T/nogr"; cw "$o"; "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$o.err"; e=$?
[ "$e" != 0 ] && [ "$(lg "$o" a-1 devgen07 23)" = sampled ] && [ -z "$(lg "$o" b-1 devgen07 1)" ] && grep -q 'run INVALID, no further requests' "$o.err" &&
  ok "attempt-1 reply with mode=sampled: run INVALID, nothing sent after it" || bad "non-greedy attempt-1 reply: exit $e mode $(lg "$o" a-1 devgen07 23)"
rm -f "$A/A/sum_range"
# refusals
cp -R "$T/pub" "$T/pubh"; mkdir -p "$T/pubh/tasks/cases/devgen01/hidden"; cp "$T/src/tasks/cases/devgen01/hidden/expected" "$T/pubh/tasks/cases/devgen01/hidden/"
refuse "a task dir holding hidden data" "no hidden data, ever" "$RUN" --tasks "$T/pubh" --out "$T/o-hidden"
gp "$T/Pm" -e 's/^mask=.*/mask=on/'; refuse "mask on" "mask must be off" env GEN_PARAMS="$T/Pm" "$RUN" --tasks "$T/pub" --out "$T/o-mask"
gp "$T/Pk" -e 's/^k=.*/k=4/'; cw "$T/o-k"; refuse "k differs from protocol.json" "protocol.json .retrieval.k is '3', the driver has '4'" env GEN_PARAMS="$T/Pk" "$RUN" --tasks "$T/pub" --out "$T/o-k"
gp "$T/Ps" -e 's/^seed_base=.*/seed_base=477000/'; cw "$T/o-s"; refuse "seed_base differs from protocol.json" "protocol.json .generation.seed_base" env GEN_PARAMS="$T/Ps" "$RUN" --tasks "$T/pub" --out "$T/o-s"
refuse "a real run on the draft PARAMS" "is a draft" env GEN_TEST=0 "$RUN" --tasks "$T/pub" --out "$T/o-real"
sed -e 's/^revision=.*/revision=gen-v1/' "$HERE/PARAMS" > "$T/Preal"
refuse "a real run with PLACEHOLDER values (GEN_PARAMS ignored outside test mode)" "is a draft" env GEN_TEST=0 GEN_PARAMS="$T/Preal" "$RUN" --tasks "$T/pub" --out "$T/o-real2"
refuse "no weight check in the run dir" "weights.check + server.json missing" "$RUN" --tasks "$T/pub" --out "$T/o-nowck"
cp -R "$T/pub" "$T/pubx"; mv "$T/pubx/tasks/cases/devgen08" "$T/pubx/tasks/cases/xtask08"; cw "$T/o-x"
refuse "cross-family control on a non-dev task" "DEV tasks only" "$RUN" --tasks "$T/pubx" --out "$T/o-x" --control cross-family
# resume refused when the run's identity changes
cp -R "$T/pub" "$T/pubp"; printf 'One more sentence.\n' >> "$T/pubp/tasks/cases/devgen01/prompt"
refuse "resume with a changed prompt" "resume refused" "$RUN" --tasks "$T/pubp" --out "$T/full1"
gp "$T/P1t" -e "s|^protocol_json=.*|protocol_json=$T/pj1.json|" -e 's/^attempts=.*/attempts=1/' -e 's/^timeout_s=.*/timeout_s=6/'
refuse "resume with changed params" "resume refused" env GEN_PARAMS="$T/P1t" "$RUN" --tasks "$T/pub" --out "$T/full1"
sed 's|^config\.A\.server=.*|config.A.server=fakegen test server 2|' "$T/E445" > "$T/E445b"
gp "$T/P1e" -e "s|^protocol_json=.*|protocol_json=$T/pj1.json|" -e 's/^attempts=.*/attempts=1/' -e "s|^exp445_params=.*|exp445_params=$T/E445b|"
refuse "resume with a changed model pin (server)" "resume refused" env GEN_PARAMS="$T/P1e" "$RUN" --tasks "$T/pub" --out "$T/full1"
cp "$CA/abstraction/abs-count_above.rec" "$T/rec.bak"; sed -i 's/^desc Test record/desc Changed test record/' "$CA/abstraction/abs-count_above.rec"
(cd "$CA" && sha256sum abstraction/abs-sum_squares3.rec abstraction/abs-count_above.rec) > "$CA/MANIFEST"; sha "$CA/MANIFEST" > "$CA/SET.sha256"
gp "$T/P1c" -e "s|^protocol_json=.*|protocol_json=$T/pj1.json|" -e 's/^attempts=.*/attempts=1/'
refuse "resume with a changed abstraction record set (manifest)" "resume refused" env GEN_PARAMS="$T/P1c" "$RUN" --tasks "$T/pub" --out "$T/full1"
refuse "a record set whose MANIFEST is not the pinned one" "not the pinned record set" "$RUN" --tasks "$T/pub" --out "$T/full1"
cp "$T/rec.bak" "$CA/abstraction/abs-count_above.rec"
(cd "$CA" && sha256sum abstraction/abs-sum_squares3.rec abstraction/abs-count_above.rec) > "$CA/MANIFEST"; sha "$CA/MANIFEST" > "$CA/SET.sha256"
# validator pins (genret-cli-v3): an unverified validator source (genret_anyrecheck) or a different one fails the
# run closed before any generation; a recheck that is not its recheck.sha256 is refused
for v in unverified "$(printf %063d 0)1"; do o="$T/o-rcs$((++nb))"; cw "$o"
  refuse "provider receipt recheck_source_sha256=${v:0:12}...: run fails closed" "recheck_source $v vs pinned" env GEN_STUB_RECHECK_SOURCE="$v" "$RUN" --tasks "$T/pub" --out "$o"
  [ ! -e "$o/log.tsv" ] && [ ! -d "$o/gen" ] && ok "  (no log rows, no generation dir)" || bad "recheck_source refusal left rows or generations in $o"
done
cp "$T/recheck.sha256" "$T/recheck.sha256.bak"; printf '%064d\n' 0 > "$T/recheck.sha256"; cw "$T/o-rch"
refuse "recheck binary is not its recheck.sha256" "is not the sha256 of" "$RUN" --tasks "$T/pub" --out "$T/o-rch"
cp "$T/recheck.sha256.bak" "$T/recheck.sha256"
# a receipt from another provider (the Cortex adapter marks provider=cortex-interop): fails closed, no rows
for pv in cortex-interop other; do o="$T/o-pv-$pv"; cw "$o"
  refuse "provider receipt provider=$pv: run fails closed" "says provider=$pv" env GEN_STUB_PROVIDER="$pv" "$RUN" --tasks "$T/pub" --out "$o"
  [ ! -e "$o/log.tsv" ] && [ ! -d "$o/gen" ] && ok "  (no log rows, no generation dir)" || bad "provider=$pv refusal left rows in $o"
done
o="$T/o-pv-both"; cw "$o"
refuse "Cortex adapter receipt (provider=cortex-interop, recheck_source_sha256=unverified): run fails closed" "provider=cortex-interop" env GEN_STUB_PROVIDER=cortex-interop GEN_STUB_RECHECK_SOURCE=unverified "$RUN" --tasks "$T/pub" --out "$o"
o="$T/o-ct-cortex"; cw "$o"
refuse "receipt contract=cortex-interop-v1 (not genret-cli-v3): run fails closed" "contract=cortex-interop-v1, not genret-cli-v3" env GEN_STUB_CONTRACT=cortex-interop-v1 "$RUN" --tasks "$T/pub" --out "$o"
[ ! -e "$o/log.tsv" ] && [ ! -d "$o/gen" ] && ok "  (no log rows, no generation dir)" || bad "contract refusal left rows in $o"
o="$T/o-pv-genret"; cw "$o"; GEN_STUB_PROVIDER=genret GEN_PARAMS="$T/P1" "$RUN" --tasks "$T/pub" --out "$o" > /dev/null 2> "$o.err" && ok "provider=genret receipt line: accepted (control)" || bad "provider=genret refused: $(tail -n 1 "$o.err")"
# gencheck run-ready / open and the seal-open receipt (GEN_PROTOCOL_V1 sections 8, 14): refused before any request
cw "$T/o-gc"; refuse "no --open receipt" "--open <OPEN> is required" "$HERE/run.sh" --tasks "$T/pub" --out "$T/o-gc"
touch "$T/gc.fail.run-ready"; refuse "gencheck run-ready fails" "gencheck run-ready refused" "$RUN" --tasks "$T/pub" --out "$T/o-gc"; rm -f "$T/gc.fail.run-ready"
touch "$T/gc.fail.open"; refuse "gencheck open fails" "gencheck open refused" "$RUN" --tasks "$T/pub" --out "$T/o-gc"; rm -f "$T/gc.fail.open"
sed '3s/.*/protocol_sha256 0000000000000000000000000000000000000000000000000000000000000000/' "$T/pj1.json.OPEN" > "$T/OPEN.bad"
refuse "OPEN for another protocol.json" "is not a seal-open receipt for this protocol.json" "$HERE/run.sh" --open "$T/OPEN.bad" --tasks "$T/pub" --out "$T/o-gc"
[ ! -e "$T/o-gc/meta.txt" ] && ok "  (no run record written by those refusals)" || bad "gencheck refusals wrote a run record"
# dirty tree: a private clone of the current sources, committed there, then one file changed
G="$T/clone"; git clone -q --shared "$ROOT" "$G" 2> /dev/null && rm -rf "$G/tools/gen" && cp -R "$ROOT/tools/gen" "$G/tools/" && cp "$ROOT/mk/gen-runner.mk" "$G/mk/" &&
  git -C "$G" add -A tools/gen mk/gen-runner.mk && git -C "$G" -c user.name=test -c user.email=test@invalid commit -qm test --allow-empty
refuse "clean clone reaches the weight check (control)" "weights.check + server.json missing" env GEN_TEST_CLEAN_TREE=1 "$G/tools/gen/runner/run.sh" --tasks "$T/pub" --out "$T/o-clean"
printf '\n' >> "$G/tools/gen/runner/FORMATS.md"
refuse "dirty source tree" "dirty source tree" env GEN_TEST_CLEAN_TREE=1 "$G/tools/gen/runner/run.sh" --tasks "$T/pub" --out "$T/o-dirty"
# cross-family control: each dev task's own family token excluded from retrieval
o="$T/xf"; cw "$o"
if "$RUN" --tasks "$T/pub" --out "$o" --control cross-family > /dev/null 2> "$o.err"; then
  tok="$(FS_TOK 'loop(arith)')"
  [ "$(mfk "$o" D exclude_family devgen03)" = "$tok" ] && grep -qx "control=cross-family" "$o/meta.txt" && [[ "$(mfk "$o" D retrieved devgen03)" != *abs-sum_squares3* ]] &&
    grep -qP "^abs-sum_squares3\t.*\tfamily_excluded:$tok\$" "$o/retrieval/d/devgen03/out/excluded.tsv" && grep -qx -- "--exclude-family" "$o/retrieval/d/devgen03/argv" &&
    ok "cross-family control: --exclude-family <the dev task's famsig token>; the record of that family is excluded" || bad "cross-family control record"
else bad "cross-family control run failed: $(tail -n 1 "$o.err")"; fi
# hidden-leak probe, positive control: a public prompt that carries the canary IS caught
cp -R "$T/pub" "$T/publ"; printf '%s\n' "$CANARY" >> "$T/publ/tasks/cases/devgen05/prompt"; o="$T/leak"; cw "$o"
"$RUN" --tasks "$T/publ" --out "$o" > /dev/null 2>&1
[ -n "$(leak "$o")" ] && [ -n "$(leak "$A/requests.log")" ] && ok "  (positive control: a canary put in a prompt is found in the run dir and the server log)" || bad "canary positive control not caught"
unset GEN_PARAMS; export GEN_PARAMS="$T/P"

# ---- 5. reducer mutants ----------------------------------------------------------------------------------------
rehash() { (cd "$1" && find . -type f ! -path ./SHA256SUMS ! -path ./.lock ! -path './gate/g0.tmp/*' -printf '%P\n' | LC_ALL=C sort | tr '\n' '\0' | xargs -0 sha256sum) > "$1/SHA256SUMS"; }
mut() { # mut <label> <expected reason fragment> <run dir> <harness dir> [pins]: the reducer must say INVALID for that reason
  local l="$1" m="$2" o="$T/mut$((++NM))"
  "$RED" --run "$3" --harness "$4" ${5:+--pins "$5"} --out "$o" > /dev/null 2>&1
  if [ "$(gk "$o" verdict)" = INVALID ] && grep -q "^reason\.[0-9]*=.*$m" "$o/aggregate.txt"; then ok "mutant detected, INVALID: $l"
  else bad "mutant not detected: $l ($(gk "$o" verdict): $(gk "$o" reason.1))"; fi
}
NM=0
cpr() { rm -rf "$T/mr" "$T/mh"; cp -R "$R" "$T/mr"; mkdir -p "$T/mhe/runs"; rm -rf "$T/mhe/runs/h"; cp -R "$H1" "$T/mhe/runs/h"; chmod -R u+w "$T/mhe/runs/h"
  rm -rf "$T/mhe/receipts"; cp -R "$(dirname "$(dirname "$H1")")/receipts" "$T/mhe/receipts"; chmod -R u+w "$T/mhe/receipts"; MH="$T/mhe/runs/h"; }
cpr; "$RED" --run "$T/mr" --harness "$MH" --pins "$T/pins" --out "$T/mut0" > /dev/null 2>&1
[ "$(gk "$T/mut0" verdict)" = PASS ] && cmp -s "$T/mut0/aggregate.txt" "$T/red1/aggregate.txt" && ok "  (control: unmodified copies reduce to the same PASS aggregate)" || bad "mutant control copy"
cpr; b="$(mfk "$T/mr" B prefix_sha256 devgen01)"; sed -i '1s/^/X/' "$T/mr/blobs/$b"; rehash "$T/mr"
mut "prompt source (the shared prefix blob edited, sums rehashed)" "blob $b missing or altered" "$T/mr" "$MH"
cpr; q="$T/mr/gen/d-1/devgen04/r1.req.json"; jq -c '.seed = 1' "$q" > "$q.t" && mv "$q.t" "$q"; rehash "$T/mr"
mut "seed (a round-1 request's seed changed, sums rehashed)" "request d-1 devgen04 round 1 is not the request" "$T/mr" "$MH"
cpr; sed -i 's/^mask=off$/mask=on/' "$T/mr/meta.txt"; rehash "$T/mr"
mut "mask state (meta mask=on, sums rehashed)" "run mask is 'on'" "$T/mr" "$MH"
cpr; sed 's/^model=.*/model=qwen3-8b/' "$T/pins" > "$T/pins.m"
mut "model pin (pins name another model)" "pin model" "$T/mr" "$MH" "$T/pins.m"
cpr; sed -i 's/^model=.*/model=qwen3-8b/' "$T/mr/meta.txt"; rehash "$T/mr"
mut "model pin (meta model edited, sums rehashed: requests no longer match)" "is not the request its manifest" "$T/mr" "$MH"
cpr; sed 's/^tasks_corpus_sha256=.*/tasks_corpus_sha256='"$(printf '%064d' 1)"'/' "$T/pins" > "$T/pins.s"
mut "split (the harness scored another task corpus than the pinned one)" "pin tasks_corpus_sha256" "$T/mr" "$MH" "$T/pins.s"
cpr; sed 's/^tasks_public_sha256=.*/tasks_public_sha256='"$(printf '%064d' 2)"'/' "$T/pins" > "$T/pins.p"
mut "split (the run's public tasks are not the pinned split)" "pin tasks_public_sha256" "$T/mr" "$MH" "$T/pins.p"
cpr; v="$MH/sets/a-1/verdicts.txt"; sed -i '1s/ REJECT / ACCEPT /' "$v"; sed -i '1s/ REJECT / ACCEPT /' "$MH/sets/a-1/replay/verdicts.txt"
mut "verdict (a REJECT turned ACCEPT in both passes)" "set a-1 task devgen01: receipt" "$T/mr" "$MH"
cpr; rm -rf "$T/mr/gen/c-1/devgen05/done"; rehash "$T/mr"
mut "an unfinished generation (no done marker)" "has no done marker" "$T/mr" "$MH"
cpr; sed -i 's/^GREEDY PASS$/GREEDY PASS/; s/identical=yes mode_r1=greedy/identical=yes mode_r1=greedy/' "$T/mr/gate/greedy/verdict.txt"; jq -c '.choices[0].message.content += "x"' "$T/mr/gate/greedy/devgen02/r2.resp.json" > "$T/x.json" && mv "$T/x.json" "$T/mr/gate/greedy/devgen02/r2.resp.json"; rehash "$T/mr"
mut "greedy gate record (a reply changed, verdict kept PASS)" "greedy gate verdict PASS but task devgen02" "$T/mr" "$MH"
cpr; echo extra > "$T/mr/blobs/extra"
mut "an unlisted file in the run dir" "run SHA256SUMS" "$T/mr" "$MH"

cpr; awk -F'\t' 'BEGIN { OFS = "\t" } $1 == "a-1" && $2 == "devgen01" { $10 = "REQUEST FAILED" } { print }' "$T/mr/log.tsv" > "$T/x.tsv" && mv "$T/x.tsv" "$T/mr/log.tsv"; rehash "$T/mr"
mut "an attempt-1 transport failure (no_candidate by transport)" "no reply (transport)" "$T/mr" "$MH"
# refusals (GEN_PROTOCOL_V1 section 6): no receipt, no verdict at all
rref() { # rref <label> <expected message fragment> <reducer args...>
  local l="$1" m="$2" o="$T/rref$((++NM))"; shift 2
  "$@" --out "$o" > /dev/null 2> "$o.err"; local e=$?
  if [ "$e" = 2 ] && [ ! -e "$o" ] && grep -qF -- "$m" "$o.err"; then ok "reducer refuses, no receipt: $l"
  else bad "reducer did not refuse: $l (exit $e: $(tail -n 1 "$o.err"))"; fi
}
cpr; sed -i '2d' "$MH/sets/d-1/verdicts.txt" "$MH/sets/d-1/replay/verdicts.txt"
rref "a MISSING cell (one d-1 verdict line removed)" "MISSING outcome" "$RED" --run "$T/mr" --harness "$MH" --pins "$T/pins"
cpr; sed -i '1p' "$MH/sets/b-1/verdicts.txt" "$MH/sets/b-1/replay/verdicts.txt"
rref "a duplicated cell (a b-1 task graded twice)" "grades a task more than once" "$RED" --run "$T/mr" --harness "$MH" --pins "$T/pins"
cpr; sed -n '2p' "$T/mr/log.tsv" >> "$T/mr/log.tsv"; rehash "$T/mr"
rref "a duplicated cell (two log rows for one generation)" "two rows for one generation" "$RED" --run "$T/mr" --harness "$MH" --pins "$T/pins"
cpr; sed '5s/.*/heldout_pub_sha256 '"$(printf %064d 9)"'/' "$T/mr/OPEN" > "$T/OPEN.other"
rref "an OPEN that is not the run's own" "is not $T/OPEN.other" "$REDX" --run "$T/mr" --harness "$MH" --open "$T/OPEN.other" --gencheck "$T/gencheck"
cpr; touch "$T/gc.fail.run-ready"
rref "gencheck run-ready fails" "gencheck run-ready refused" "$RED" --run "$T/mr" --harness "$MH"; rm -f "$T/gc.fail.run-ready"
cpr; touch "$T/gc.fail.open"
rref "gencheck open fails" "gencheck open refused" "$RED" --run "$T/mr" --harness "$MH"; rm -f "$T/gc.fail.open"
cpr; jq '.splits.heldout.count = 59' "$T/mr/protocol.json" > "$T/x.json" && mv "$T/x.json" "$T/mr/protocol.json"; rehash "$T/mr"
rref "protocol.json heldout.count is not 60" "splits.heldout.count is '59'" "$RED" --run "$T/mr" --harness "$MH"
[ "$(grep -c '^no_candidate_a1\.[ABCD]=0$' "$T/red1/aggregate.txt")" = 4 ] && grep -qx 'pass3.label=pass@3 (1 greedy + 2 sampled)' "$T/red1/aggregate.txt" &&
  grep -qx "outcomes_sha256=$(sha "$T/red1/outcomes.tsv")" "$T/red1/receipt.txt" && [ "$(tail -n +2 "$T/red1/outcomes.tsv" | wc -l)" = 8 ] &&
  ! grep -q 'no_candidate\|MISSING' "$T/red1/outcomes.tsv" && [ "$(gk "$T/red1" arm.C.tasks_selected_below_k)" != "" ] && [ -n "$(gk "$T/red1" arm.D.selected_mean)" ] &&
  ok "receipt: no_candidate_a1 = 0 per condition, pass@3 label, outcomes.tsv (8 tasks, one class per cell, hashed), selected per C and D" || bad "receipt outcome / selected lines"

# ---- 6. length-matched control (dev only, secondary) --------------------------------------------------------------
o="$T/lm"; cw "$o"
if GEN_PARAMS="$T/P1" "$RUN" --tasks "$T/pub" --out "$o" --control length-matched > /dev/null 2> "$o.err"; then
  okm=1
  for t in devgen01 devgen04; do
    [ "$(mfk "$o" C mode "$t")" = length-matched ] || [ "$(mfk "$o" D suffix_bytes "$t")" = 0 ] || okm=0
    [ "$(mfk "$o" C target_bytes "$t")" = "$(mfk "$o" D suffix_bytes "$t")" ] || okm=0
  done
  [ "$okm" = 1 ] && grep -qx control=length-matched "$o/meta.txt" && ok "length-matched control: C targets D's realised suffix bytes per task" || bad "length-matched control manifests"
  HL="$(hrun "$o/cands")" && "$RED" --run "$o" --harness "$HL" --out "$T/redlm" > /dev/null 2> "$T/redlm.err"
  m="$(gk "$T/redlm" lenmatch.matched)"; st="$(gk "$T/redlm" lenmatch.status)"
  want=UNAVAILABLE; [ -n "$m" ] && [ $((2 * ${m%/*})) -ge "${m#*/}" ] && want=AVAILABLE
  [ "$(grep -c '^lenmatch\.task\.' "$T/redlm/aggregate.txt")" = 8 ] && [ "$st" = "$want" ] && grep -q '^lenmatch.caveat=' "$T/redlm/aggregate.txt" &&
    { [ "$st" = UNAVAILABLE ] || [ -n "$(gk "$T/redlm" lenmatch.D.success_a1_matched)" ]; } && ! grep -q '^reason.*lenmatch' "$T/redlm/aggregate.txt" &&
    ok "length-matched ablation: 8 per-task lines, matched $m, status $st (genstat lenset), caveat, never a validity reason" ||
    bad "length-matched ablation lines: $(grep '^lenmatch\|^verdict' "$T/redlm/aggregate.txt" 2> /dev/null | head -n 4 | tr '\n' ' ') $(tail -n 1 "$T/redlm.err")"
else bad "length-matched control run failed: $(tail -n 1 "$o.err")"; fi

echo "test_gen_runner: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
