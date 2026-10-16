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
export GEN_TEST=1 EXP445_TEST_BACKOFF_S=0 OLA_EVIDENCE_ROOT="$T/evid"
# refuse <label> <expected stderr fragment> <command...>: the command must exit non-zero with that message
refuse() { local l="$1" m="$2"; shift 2; if "$@" > "$T/ref.out" 2> "$T/ref.err"; then bad "$l: accepted"; elif grep -qF -- "$m" "$T/ref.err"; then ok "$l: refused"; else bad "$l: refused for another reason: $(head -c 300 "$T/ref.err")"; fi; }

# ---- 1. statistics ---------------------------------------------------------------------------------------
SV="$ROOT/tools/gen/reducer/testdata/stats_vectors.tsv"
[ "$(sha "$SV")" = 523f90438c5c8a30277f684041515535c5f73cb5661c5b6e48f8ca3179dad430 ] &&
  ok "Agent A's stats_vectors.tsv is the frozen file (gen/protocol c3c4fc9e, sha256 523f9043...)" || bad "stats_vectors.tsv sha256"
o="$("$GS" vectors "$SV" 2>&1)"; [[ "$o" == "vectors PASS: 90 rows "*", 0 failures" ]] && ok "genstat: $o" || bad "genstat vectors on Agent A's file: $o"
nown=0 fown=0
while IFS=$'\t' read -r cmd args want src; do
  case "$cmd" in ""|"#"*) continue ;; esac
  nown=$((nown + 1)); got="$("$GS" "$cmd" $args 2>&1)"; [ "$got" = "$want" ] || { fown=$((fown + 1)); echo "  own vector $cmd $args: got '$got', want '$want'"; }
done < "$ROOT/tools/gen/reducer/stats_vectors_own.tsv"
[ "$fown" = 0 ] && [ "$nown" -ge 10 ] && ok "genstat own vectors (exact McNemar fractions, x445 Wilson): $nown rows, 0 failures" || bad "own vectors: $fown of $nown failed"
sed "0,/0.0601718521/s//0.0611718521/" "$SV" > "$T/sv.mut"
if cmp -s "$SV" "$T/sv.mut"; then bad "vector mutant was not made"
elif "$GS" vectors "$T/sv.mut" > /dev/null 2>&1; then bad "genstat accepted a changed vector"; else ok "  (negative control: one changed Wilson vector value fails)"; fi
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
C="$T/corpus/records"; mkdir -p "$C/raw" "$C/abstraction" "$C/receipts"
rec() { # rec <kind> <id> <family> <example>: one verified corpus record whose body is the example's Omegese
  local f="$C/$1/$2.rec" e="$4" r
  r="$(printf 'receipt for %s\n' "$2" | sha256sum | cut -d' ' -f1)"; printf 'receipt for %s\n' "$2" > "$C/receipts/$r"
  { echo "gen-record 1"; echo "id $2"; echo "kind $1"; echo "split corpus"; echo "family $3"
    grep -E '^(entry|params|ret)( |$)' "$EXD/$e/requirement"; echo "desc Test record from the exp445 example $e."
    echo "receipt $r"; echo "validity verified"; echo body; cat "$EXD/$e/program.omegese"; } > "$f"
}
FS_TOK() { printf 'gs1:%s' "$1" | sha256sum | cut -c1-16 | sed 's/^/gs1-/'; }
rec raw raw-power_mod gs1-1111111111111111 power_mod
rec raw raw-hash_step gs1-2222222222222222 hash_step
rec abstraction abs-sum_squares3 "$(FS_TOK 'loop(arith)')" sum_squares3
rec abstraction abs-count_above gs1-3333333333333333 count_above
(cd "$C" && sha256sum raw/raw-power_mod.rec raw/raw-hash_step.rec abstraction/abs-sum_squares3.rec abstraction/abs-count_above.rec) > "$C/MANIFEST"
sha "$C/MANIFEST" > "$C/SET.sha256"
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
chmod +x "$T/recheck" "$T/famsig"
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
      -e "s|^provider=.*|provider=$HERE/stub_genret.sh|" -e "s|^corpus_manifest=.*|corpus_manifest=$C/MANIFEST|" \
      -e "s|^corpus_manifest_sha256=.*|corpus_manifest_sha256=$(cat "$C/SET.sha256")|" -e "s|^recheck=.*|recheck=$T/recheck|" \
      -e "s|^famsig=.*|famsig=$T/famsig|" -e "s|^dev_meta=.*|dev_meta=$T/devmeta|" -e "s|^dev_tasks=.*|dev_tasks=$T/src|" \
      -e 's/^greedy_gate_tasks=.*/greedy_gate_tasks=3/' -e 's/^timeout_s=.*/timeout_s=5/' -e 's/^revision=.*/revision=gen-v1-test/' "$@" "$HERE/PARAMS" > "$o"
}
gp "$T/P"
! grep -v "^#" "$T/P" | grep -q PLACEHOLDER && ok "test PARAMS: every placeholder replaced by a test path" || bad "test PARAMS still holds placeholders"
export GEN_PARAMS="$T/P"
cw() { EXP445_PARAMS="$T/E445" "$ROOT/tools/exp445/check_weights.sh" A "$1" > /dev/null 2>&1; }
greq() { cat "$A/requests.log" 2> /dev/null | wc -l; }

# ---- 2. driver: the full run --------------------------------------------------------------------------------
R="$T/run1"; cw "$R" || { echo "check_weights failed" >&2; exit 2; }
if "$RUN" --tasks "$T/pub" --out "$R" > "$T/run1.out" 2> "$T/run1.err"; then ok "run.sh: full run (4 conditions x 8 tasks x 3 attempts) exit 0"
else bad "run.sh full run failed: $(tail -n 3 "$T/run1.err")"; fi
