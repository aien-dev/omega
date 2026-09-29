#!/bin/bash
# qualify_visor.sh -- Omega Visor V1 qualification glue (lane 8).
#
#   tools/qualify_visor.sh [--skip-regression] [--no-asan] [out-dir]   (default out-dir: evidence/VISOR)
#
# Runs the make targets and tests/visor/qualification/run_qualification.sh, collects
# their counts and writes ONE new receipt <out-dir>/<sha256 of its own bytes>.json.
# Every assertion lives in the C tests, the .omega-session goldens and the runner; this
# script only runs, counts and records. It never rewrites a tracked file: nothing under
# evidence/ is modified, only the new content-addressed receipt is added. A plain-text
# gate summary goes to stdout.
#
# Steps (all make / test runs wrapped in `flock $BENCH_LOCK`):
#   0. the AIENOS capability library, rebuilt from `git archive` of the commit in
#      aienos.lock (AIENOS_REPO, default ~/workspace/aienos-repo), as
#      tools/effect_cap64_receipt.sh and CI do; its identity goes into the receipt
#   1. physics-free omega binary (build/q8r) + physics-free Visor suites and link checks
#   2. runtime-linked suites test-visor-world, test-visor-authority (pinned library)
#   3. CPU regression gates (build/q8reg); GPU/silicon targets are never run
#   4. the console campaign (runner) on the release binary, then on an ASan+UBSan build
#   5. five gates -> verdict OMEGA_VISOR_V1_PASS / OMEGA_VISOR_V1_FAIL
# Verdict PASS needs all five gates PASS, a tree that did not change during the run
# (HEAD and each dirty file's sha256), the AIENOS library built from aienos.lock and the
# physics checkout at physics.lock. The ASan pass is recorded but, as before, does not
# gate the verdict. Builds only into private OUT_DIRs; never touches build/ itself
# (except the pre-existing test-m5 QEMU path, see non_claims).
# Shell and coreutils only (git, sha256sum, grep, sed, awk, sort, tar, make, flock).
set -u
LC_ALL=C
export LC_ALL

HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 2

SKIP_REG=0
NO_ASAN=0
OUTDIR=evidence/VISOR
for a in "$@"; do
    case "$a" in
        --skip-regression) SKIP_REG=1 ;;
        --no-asan) NO_ASAN=1 ;;
        -h|--help) sed -n '2,4p' "$0"; exit 0 ;;
        -*) echo "usage: tools/qualify_visor.sh [--skip-regression] [--no-asan] [out-dir]" >&2; exit 2 ;;
        *) OUTDIR=$a ;;
    esac
done

AIENOS_REPO=${AIENOS_REPO:-$HOME/workspace/aienos-repo}
PHYSICS_DIR=${PHYSICS_DIR:-$HOME/workspace/physics-r13}
BENCH_LOCK=${BENCH_LOCK:-$HOME/workspace/.argus-bench.lock}
OUT=build/q8r
ASAN_OUT=build/q8asan
REG_OUT=build/q8reg
Q=build/q8q
LOGS=$Q/logs
REASONS=()

json_str() {   # JSON string body: escape \ and ", tabs; drop other control bytes; join lines with \n
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/\t/\\t/g' | tr -d '\000-\010\013-\037' |
        awk 'BEGIN{ORS=""} {if (NR>1) print "\\n"; print}'
}

tree_state() {   # -> "<dirty-file JSON array body>" ; one element per `git status --short` line
    local line st path sha out=""
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        st=$(printf '%s' "${line:0:2}" | tr -d ' ')
        path=${line:3}
        if [ -f "$path" ]; then sha="\"$(sha256sum "$path" | awk '{print $1}')\""; else sha=null; fi
        out+="${out:+, }{\"status\": \"$(json_str "$st")\", \"path\": \"$(json_str "$path")\", \"sha256\": $sha}"
    done < <(git status --short)
    printf '%s' "$out"
}

run_id=$(date -u +%Y%m%dT%H%M%SZ)
head0=$(git rev-parse HEAD)
tree0=$(git rev-parse 'HEAD^{tree}')
st0=$(git status --short)
files0=$(tree_state)

rm -rf "$Q" && mkdir -p "$LOGS" "$Q/aienos"

# ---- pins -------------------------------------------------------------------
aienos_pin=$(awk 'NF && $1 !~ /^#/ {print $1; exit}' aienos.lock)
physics_pin=$(awk 'NF && $1 !~ /^#/ {print $1; exit}' physics.lock)
physics_head=$(git -C "$PHYSICS_DIR" rev-parse HEAD 2>/dev/null || echo missing)
[ "$physics_head" = "$physics_pin" ] || REASONS+=("physics checkout $PHYSICS_DIR at $physics_head != physics.lock $physics_pin")

aienos_src_tree=$(git -C "$AIENOS_REPO" rev-parse "$aienos_pin:native/capability" 2>/dev/null || echo missing)
if git -C "$AIENOS_REPO" archive "$aienos_pin" native/capability | tar -x -C "$Q/aienos" &&
   flock "$BENCH_LOCK" make -C "$Q/aienos/native/capability" > "$LOGS/aienos-lib.log" 2>&1; then
    lib_ok=1
else
    lib_ok=0
    REASONS+=("AIENOS capability library did not build from aienos.lock $aienos_pin ($AIENOS_REPO)")
fi
LIB=$Q/aienos/native/capability/out/libaienos_capability.a
lib_sha=null; [ -f "$LIB" ] && lib_sha="\"$(sha256sum "$LIB" | awk '{print $1}')\""
hdr_sha=null; [ -f "$Q/aienos/native/capability/aienos_capability.h" ] &&
    hdr_sha="\"$(sha256sum "$Q/aienos/native/capability/aienos_capability.h" | awk '{print $1}')\""

# ---- suites -----------------------------------------------------------------
PF=(PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 AIENOS_R7_DIR="$HERE/$Q/aienos")
RT=(PHYSICS_DIR="$PHYSICS_DIR" AIENOS_R7_DIR="$HERE/$Q/aienos" VISOR_AUTH_CAP_LIB="$HERE/$LIB")
FREE_SUITES=(test-visor-semantic test-language test-visor-verify test-visor-evidence
             test-visor-machine test-visor-realization test-visor-console)
LINKED_SUITES=(test-visor-world test-visor-authority)
REGRESSION=(test test-m5 test-m6 test-m7 test-m8 test-m9 test-m10 test-m13 test-m14)
NEVER=("test-m12|GPU (living matvec)" "test-m15|accelerator/GPU" "test-m17|Blackwell GPU"
       "test-m18|GPU" "test-m19|GPU")

declare -A STATUS RUN FAILED
SUITES_JSON=""

counts() {   # "PASS n/m" / "FAIL n/m" lines + omegatool "TOTAL GATES: n | PASSED: n | FAILED: n" -> "run failed"
    awk '/^(PASS|FAIL)[ \t]+[0-9]+\/[0-9]+[ \t]*$/ { split($2, x, "/"); r += x[2]; f += x[2] - x[1] }
         { s = $0
           while (match(s, /TOTAL GATES:[ \t]*[0-9]+[ \t]*\|[ \t]*PASSED:[ \t]*[0-9]+[ \t]*\|[ \t]*FAILED:[ \t]*[0-9]+/)) {
               m = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
               split(m, p, "|"); t = p[1]; u = p[3]; gsub(/[^0-9]/, "", t); gsub(/[^0-9]/, "", u)
               r += t; f += u } }
         END { printf "%d %d\n", r, f }' "$1"
}

suite() {    # suite <target> <out-dir> <make args...>
    local name=$1 out=$2 log=$LOGS/$1.log rc n f status marker=""
    shift 2
    flock "$BENCH_LOCK" make OUT_DIR="$out" "$@" "$name" > "$log" 2>&1
    rc=$?
    case "$name" in
        visor-authority-check) marker=OMEGA_VISOR_AUTHORITY_ISOLATION_PASS ;;
        visor-physics-free-check) marker=OMEGA_VISOR_PHYSICS_FREE_BUILD_PASS ;;
    esac
    if [ -n "$marker" ]; then
        n=1; f=1; { grep -aq "$marker" "$log" && [ "$rc" -eq 0 ]; } && f=0
    else
        read -r n f < <(counts "$log")
    fi
    status=FAIL
    [ "$rc" -eq 0 ] && [ "$n" -gt 0 ] && [ "$f" -eq 0 ] && status=PASS
    STATUS[$name]=$status; RUN[$name]=$n; FAILED[$name]=$f
    SUITES_JSON+="${SUITES_JSON:+,}
    {\"target\": \"$name\", \"rc\": $rc, \"tests_run\": $n, \"tests_failed\": $f, \"status\": \"$status\", \"log_sha256\": \"$(sha256sum "$log" | awk '{print $1}')\"}"
    echo "suite $name: $status (rc=$rc run=$n failed=$f)" >&2
}

not_run() {  # not_run <target> <reason>
    STATUS[$1]=NOT_RUN
    SUITES_JSON+="${SUITES_JSON:+,}
    {\"target\": \"$1\", \"status\": \"NOT_RUN\", \"reason\": \"$(json_str "$2")\"}"
}

# 1. physics-free omega binary + physics-free suites (fresh private OUT_DIR)
rm -rf "$OUT"
flock "$BENCH_LOCK" make OUT_DIR="$OUT" "${PF[@]}" "$OUT/omega" > "$LOGS/omega-build.log" 2>&1
binary_rc=$?
for t in "${FREE_SUITES[@]}" visor-authority-check visor-physics-free-check; do suite "$t" "$OUT" "${PF[@]}"; done
# 2. runtime-linked suites (AIENOS capability library pinned by aienos.lock)
for t in "${LINKED_SUITES[@]}"; do suite "$t" "$OUT" "${RT[@]}"; done
# 3. regression (CPU gates only)
for t in "${REGRESSION[@]}"; do
    if [ "$SKIP_REG" -eq 1 ]; then not_run "$t" "--skip-regression"; else suite "$t" "$REG_OUT" "${RT[@]}"; fi
done
for e in "${NEVER[@]}"; do not_run "${e%%|*}" "GPU/silicon target excluded by lane-8 brief: ${e#*|}"; done
not_run test-m11 "not in the lane-8 regression list"

# 4. console campaign (runner) on the release binary, then on an ASan+UBSan build
failed_cases() {   # runner log -> "section<TAB>case<TAB>detail" for every QUAL ... FAIL line
    awk '/^QUAL / { sec = $2; rest = substr($0, length("QUAL " sec " ") + 1)
                    fi = index(rest, " FAIL"); pi = index(rest, " PASS")
                    if (fi > 1 && (pi == 0 || fi < pi)) {
                        d = substr(rest, fi + 5); sub(/^ /, "", d)
                        printf "%s\t%s\t%s\n", sec, substr(rest, 1, fi - 1), d } }' "$1"
}
RLOG=$LOGS/runner.log
flock "$BENCH_LOCK" env QUAL_WORK="$OUT/qual-work" bash tests/visor/qualification/run_qualification.sh "$OUT/omega" > "$RLOG" 2>&1
runner_rc=$?
declare -A SEC_RUN SEC_FAILED
SECTIONS_JSON=""
while read -r _ sec r f; do
    r=${r#run=}; f=${f#failed=}
    SEC_RUN[$sec]=$r; SEC_FAILED[$sec]=$f
    SECTIONS_JSON+="${SECTIONS_JSON:+, }\"$sec\": {\"run\": $r, \"failed\": $f}"
done < <(grep -aE '^QUAL_SECTION [^ ]+ run=[0-9]+ failed=[0-9]+$' "$RLOG")
hline=$(grep -aE '^QUAL_HOSTILE run=[0-9]+ failed_closed=[0-9]+ crashed=[0-9]+ noop=[0-9]+$' "$RLOG" | tail -n 1)
dline=$(grep -aE '^QUAL_DETERMINISM runs=[0-9]+ identical=[A-Za-z0-9_]+$' "$RLOG" | tail -n 1)
field() { printf '%s\n' "$1" | tr ' ' '\n' | awk -F= -v k="$2" '$1 == k {print $2; exit}'; }
if [ -n "$hline" ]; then
    hostile_json="{\"run\": $(field "$hline" run), \"failed_closed\": $(field "$hline" failed_closed), \"crashed\": $(field "$hline" crashed), \"benign_noop\": $(field "$hline" noop),
    \"note\": \"benign_noop = blank/whitespace/comment lines, a spec'd no-op (exit 0); the rest must exit 1 with an error line\"}"
else
    hostile_json=null
fi
if [ -n "$dline" ]; then
    det_ident=false; [ "$(field "$dline" identical)" = yes ] && det_ident=true
    det_json="{\"runs\": $(field "$dline" runs), \"identical\": $det_ident, \"scope\": \"same host, fresh processes; text, --json and realization ids\"}"
else
    det_json=null
fi
FAILED_CASES=$(failed_cases "$RLOG")
FAILED_JSON=""
while IFS=$'\t' read -r sec name detail; do
    [ -n "$sec" ] || continue
    FAILED_JSON+="${FAILED_JSON:+,}
    {\"section\": \"$(json_str "$sec")\", \"case\": \"$(json_str "$name")\", \"detail\": \"$(json_str "$detail")\"}"
done <<< "$FAILED_CASES"

asan_json='{"status": "NOT_RUN"}'
if [ "$NO_ASAN" -eq 0 ]; then
    rm -rf "$ASAN_OUT"
    flock "$BENCH_LOCK" make OUT_DIR="$ASAN_OUT" "${PF[@]}" \
        "CC=cc -fsanitize=address,undefined -fno-omit-frame-pointer -g" "$ASAN_OUT/omega" > "$LOGS/asan-build.log" 2>&1
    asan_brc=$?
    ALOG=$LOGS/runner-asan.log
    flock "$BENCH_LOCK" env QUAL_WORK="$ASAN_OUT/qual-work" bash tests/visor/qualification/run_qualification.sh "$ASAN_OUT/omega" > "$ALOG" 2>&1
    asan_st=RUN; [ "$asan_brc" -eq 0 ] || asan_st=BUILD_FAILED
    san=PRESENT_OR_UNKNOWN
    grep -aqE '^QUAL hostile no-sanitizer-reports-in-any-transcript PASS' "$ALOG" && san=none
    ahl=$(grep -aE '^QUAL_HOSTILE run=[0-9]+ failed_closed=[0-9]+ crashed=[0-9]+' "$ALOG" | tail -n 1)
    acr=null; [ -n "$ahl" ] && acr=$(field "$ahl" crashed)
    same=false
    [ "$(failed_cases "$ALOG" | cut -f2 | sort)" = "$(printf '%s' "$FAILED_CASES" | cut -f2 | sort)" ] && same=true
    asan_json="{\"status\": \"$asan_st\", \"sanitizer_reports\": \"$san\", \"crashed\": $acr, \"same_case_failures_as_release\": $same}"
fi

files1=$(tree_state)
head1=$(git rev-parse HEAD)
stable=true
{ [ "$head0" = "$head1" ] && [ "$files0" = "$files1" ]; } || stable=false

# 5. gates
GATE_NAMES=(OMEGA_VISOR_SEMANTIC_PASS OMEGA_VISOR_VERIFY_PASS OMEGA_VISOR_REALIZE_PASS
            OMEGA_VISOR_AUTHORITY_ISOLATION_PASS OMEGA_VISOR_REGRESSION_PASS)
declare -A G_SUITES G_SECTIONS
G_SUITES[OMEGA_VISOR_SEMANTIC_PASS]="test-visor-semantic test-language"
G_SECTIONS[OMEGA_VISOR_SEMANTIC_PASS]="semantic determinism"
G_SUITES[OMEGA_VISOR_VERIFY_PASS]="test-visor-verify test-visor-evidence test-visor-world"
G_SECTIONS[OMEGA_VISOR_VERIFY_PASS]="usability"
G_SUITES[OMEGA_VISOR_REALIZE_PASS]="test-visor-machine test-visor-realization test-visor-console"
G_SECTIONS[OMEGA_VISOR_REALIZE_PASS]="realize scriptability"
G_SUITES[OMEGA_VISOR_AUTHORITY_ISOLATION_PASS]="visor-authority-check test-visor-authority"
G_SECTIONS[OMEGA_VISOR_AUTHORITY_ISOLATION_PASS]="hostile authority"
G_SUITES[OMEGA_VISOR_REGRESSION_PASS]="${REGRESSION[*]} visor-physics-free-check"
G_SECTIONS[OMEGA_VISOR_REGRESSION_PASS]=""

GATES_JSON=""
SUMMARY=""
all_pass=1
for g in "${GATE_NAMES[@]}"; do
    run=0; failed=0; srcs=""; notrun=""
    for t in ${G_SUITES[$g]}; do
        s=${STATUS[$t]:-NOT_RUN}
        if [ "$s" = NOT_RUN ]; then notrun+="${notrun:+, }\"$t\""; continue; fi
        run=$((run + RUN[$t])); failed=$((failed + FAILED[$t]))
        [ "$s" = FAIL ] && [ "${FAILED[$t]}" -eq 0 ] && failed=$((failed + 1))
        srcs+="${srcs:+, }\"make $t\""
    done
    for sec in ${G_SECTIONS[$g]}; do
        if [ -n "${SEC_RUN[$sec]+x}" ]; then
            run=$((run + SEC_RUN[$sec])); failed=$((failed + SEC_FAILED[$sec]))
            srcs+="${srcs:+, }\"run_qualification.sh:$sec\""
        else
            notrun+="${notrun:+, }\"runner:$sec\""
        fi
    done
    if [ "$run" -eq 0 ]; then st=NOT_RUN
    elif [ "$failed" -eq 0 ] && [ -z "$notrun" ]; then st=PASS
    else st=FAIL; fi
    GATES_JSON+="${GATES_JSON:+,}
    \"$g\": {\"status\": \"$st\", \"tests_run\": $run, \"tests_failed\": $failed, \"sources\": [$srcs]${notrun:+, \"not_run\": [$notrun]}}"
    SUMMARY+="$g=$st ($run run, $failed failed)"$'\n'
    if [ "$st" != PASS ]; then
        all_pass=0
        REASONS+=("$g $st ($failed/$run failed${notrun:+; not run: $(printf '%s' "$notrun" | tr -d '"')})")
    fi
done
while IFS=$'\t' read -r sec name detail; do
    [ -n "$sec" ] && REASONS+=("runner $sec: $name FAIL ${detail:0:140}")
done <<< "$FAILED_CASES"
[ "$stable" = true ] || REASONS+=("tree changed during the run (HEAD or modified-file hashes differ): receipt INVALID")

verdict=OMEGA_VISOR_V1_PASS
{ [ "$all_pass" -eq 1 ] && [ "$stable" = true ] && [ "$lib_ok" -eq 1 ] &&
  [ "$physics_head" = "$physics_pin" ]; } || verdict=OMEGA_VISOR_V1_FAIL

tree_dirty=false; [ -z "$st0" ] || tree_dirty=true
dirty_detail="see dirty_files"
if [ -n "$st0" ] && ! printf '%s\n' "$st0" | cut -c4- |
       grep -qvE '^(tests/visor/qualification|tools/qualify_visor\.sh|evidence/VISOR)'; then
    dirty_detail="only lane-8 files (untracked, not yet committed)"
fi
bin_json="\"built\": false, \"sha256\": null"
[ "$binary_rc" -eq 0 ] && [ -f "$OUT/omega" ] &&
    bin_json="\"built\": true, \"sha256\": \"$(sha256sum "$OUT/omega" | awk '{print $1}')\""
scr_json=null
[ -n "${SEC_RUN[scriptability]+x}" ] &&
    scr_json="{\"run\": ${SEC_RUN[scriptability]}, \"failed\": ${SEC_FAILED[scriptability]}}"
reasons_json=""
for r in "${REASONS[@]}"; do reasons_json+="${reasons_json:+,
    }\"$(json_str "$r")\""; done
product=$(cat /sys/devices/virtual/dmi/id/product_name 2>/dev/null)

body=$(cat <<EOF
{
  "schema": "omega-visor-v1-qualification/1",
  "tool": "tools/qualify_visor.sh",
  "run_id": "$run_id",
  "candidate_commit": "$head0",
  "candidate_tree": "$tree0",
  "tree_dirty": $tree_dirty,
  "tree_dirty_detail": "$dirty_detail",
  "dirty_files": [$files0],
  "tree_stable_during_run": $stable,
  "physics_commit": "$physics_pin",
  "physics": {"lock": "$physics_pin", "checkout": "$(json_str "$PHYSICS_DIR")", "checkout_head": "$physics_head"},
  "aienos": {"lock": "$aienos_pin", "source": "git archive $aienos_pin native/capability (tree $aienos_src_tree)", "header_sha256": $hdr_sha, "lib_sha256": $lib_sha, "lib_built": $([ "$lib_ok" -eq 1 ] && echo true || echo false),
    "used_by": "test-visor-authority (VISOR_AUTH_CAP_LIB) and every make run (AIENOS_R7_DIR)"},
  "host": {"uname_m": "$(uname -m)", "uname_r": "$(uname -r)", "product_name": "$(json_str "$product")"},
  "hardware_scope": "host-only: no GPU/silicon claim; no QEMU claim by the Visor (the pre-existing test-m5 gate runs its own QEMU check)",
  "omega_binary": {"path": "$OUT/omega", $bin_json,
    "build": "make OUT_DIR=$OUT PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 $OUT/omega"},
  "gates": {$GATES_JSON
  },
  "suites": [$SUITES_JSON
  ],
  "runner_rc": $runner_rc,
  "runner_log_sha256": "$(sha256sum "$RLOG" | awk '{print $1}')",
  "runner_sections": {$SECTIONS_JSON},
  "runner_failed_cases": [$FAILED_JSON
  ],
  "hostile_cases": $hostile_json,
  "sanitizer_pass": $asan_json,
  "determinism": $det_json,
  "scriptability": {"section": $scr_json,
    "contract": "exit 0 = every line ok; 1 = at least one line errored; 2 = usage error or unopenable script; --json = one object per non-blank line with keys command/status/class/result/error; no prompt when stdin is not a tty; --script - reads stdin"},
EOF
)
body+=$'\n'
body+=$(cat <<'EOF'
  "ux_findings": [
    "`true`/`false` echo as 1/0 although `type _` says bool; a user reads 1 as an integer.",
    "`why <x>` is described as 'explain where x came from' but only explains realizations; on a value it says `use realize x first`, and `realize x` then refuses a value, a dead end.",
    "Errors raised before parsing, and unknown commands, are labelled `unknown` (`error: unknown: invalid UTF-8 at byte 4`, `error: unknown: unknown command or invalid source line: 'authorize'`).",
    "`id` prints the same hash three times (id, canonical_sha256) plus canonical_len; the relation is not explained.",
    "After `realize _`, `_` silently becomes the realization, so `inspect _`/`type _` now refer to a different object than one line earlier.",
    "Every u64 ADD has the same realization id (`realized` = the ADD operation, not the apply), so `x + y` and `x + 1` share one realization id; correct per spec but surprising in `bindings`/`compare`.",
    "`run _ a b` (exactly all operands) on a binary-apply realization runs the operation on the given numbers, not on the object's operands (7+11 -> `run _ 1 2` prints 3); allowed by design after the D2 fix, but the output does not say the object's own operands were replaced.",
    "Machine/realization output uses hex profile codes (`profile 0x01 vs machine 0x01`), `physics flag=set seal=builder-constant placeholder`, and C function names (`omega_machine_estimate_latency(...)`, `omega_exec_native_f3`) as explanations; not readable without the C source.",
    "`verify` row INVARIANTS reports 399 generic checks that are 'not object-specific'; a user may read the PASS as evidence about their object.",
    "`evidence <name>` for a session object always says `no evidence`: receipts are repository files, not linked to session objects; 'what evidence supports this object' cannot be answered in V1.",
    "Blank lines produce no JSON object while comment-only lines produce one (`kind: none`); a script driver counting lines must know this.",
    "Commands after `quit` in --command/--script mode are silently dropped (exit 0).",
    "`--script /dev/null` (a character device, not a regular file) is accepted as an empty script (exit 0), although the round-2 note says non-regular files exit 2; harmless, but the claim and behaviour differ."
  ],
  "defects": [
    {
      "id": "D1",
      "severity": "high; FIXED in ec2ec0b (found at 4fab549)",
      "what": "The ./omega shipped at commit 4fab549 (sha256 543d874d...d4d3) crashed on every realize/run path (exit 139 SIGSEGV in om_realization_show; 'stack smashing detected', exit 134). Cause: omega_main.d missing from VISOR_DEPS, so tools/omega.c was not rebuilt when src/visor/visor.h changed. ec2ec0b adds it; a clean-checkout build of 4fab549 was not tested."
    },
    {
      "id": "D2",
      "severity": "medium; FIXED in d7e8a4c (open in receipt #1)",
      "what": "`run` did not check arity: `run f` -> 1, `run f 5 6` -> 11, `run _ 1` -> 12, `run _ 1 2 3` -> 3, all exit 0. At d7e8a4c: 'program f takes 1 input; give it on the command line', 'program f takes 1 input; got 2', '_ takes 2 operands; got 1 (give none, or all 2)', '... got 3 ...'; exit 1."
    },
    {
      "id": "D3",
      "severity": "low; FIXED in d7e8a4c",
      "what": "`./omega --script /` exited 0; at d7e8a4c it prints 'error: --script: / is a directory' and exits 2."
    },
    {
      "id": "D4",
      "severity": "cosmetic; FIXED in d7e8a4c",
      "what": "Doubled 'alternatives:' prefix, `effects x` error without 'error:', help `graph [x]` and `let x = <expr>`, parser jargon for `authorize x`/`execute _`: all corrected (hostile.expected and usability.expected regenerated; the only other changed line is the `run <type-id>` message, now '... is not an APPLY; only pure binary u64 applies run in V1', still exit 1)."
    }
  ],
  "non_claims": [
    "Host-only qualification on one DGX Spark (aarch64); no GPU, no silicon, no QEMU claim by the Visor.",
    "No measured or qualified cost: the Visor fills only predicted (static) and estimated (assumed machine model) cost; measured/qualified are ABSENT by design.",
    "Machine identity is the assumed canonical profile chosen from DMI, not an observed descriptor; the physics seal is a builder placeholder.",
    "World is unattached in the omega binary: `world` observes nothing; the snapshot API is covered only by the runtime-linked test-visor-world.",
    "Language V0 scope only: explicit-width unsigned integers, bool, let, + - * / & |, fn chains (x op c)...; realize/run cover u64 binary ADD/SUB/MUL/AND/OR only.",
    "Effects: the language cannot build an EFFECT object, so `run` on an effect is not reachable from the console; effect-request refusal is covered only by the C hostile suite (test-visor-authority) and the link check.",
    "GPU targets test-m12, test-m15, test-m17, test-m18, test-m19 and all Blackwell paths were not run (lane-8 brief); test-m11 was not in the list.",
    "The pre-existing test-m5 gate launches QEMU and writes build/qemu_omega_runner.bin under the shared build/ directory (hard-coded path in omegatool).",
    "The e2e session golden (tests/visor/sessions/e2e.expected) is compared host-independently only after ec2ec0b; the lane-8 goldens mask the machine block, machine name/id and estimated cycles.",
    "Determinism is shown across fresh processes on this host only, not across hosts, compilers or ABIs (golden bytes are pinned to LP64/aarch64).",
    "Visor V1 is not claimed safe against a hostile local user with write access to the binary or the evidence directory."
  ]
EOF
)
body+=$(cat <<EOF
,
  "verdict": "$verdict",
  "verdict_reasons": [$reasons_json]
}
EOF
)
mkdir -p "$OUTDIR"
tmp=$(mktemp "$OUTDIR/.tmp-XXXXXX")
printf '%s\n' "$body" > "$tmp"
digest=$(sha256sum "$tmp" | awk '{print $1}')
final=$OUTDIR/$digest.json
if [ -e "$final" ]; then rm -f "$tmp"; else mv "$tmp" "$final"; fi
chmod 0644 "$final"
printf '%s' "$SUMMARY"
[ ${#REASONS[@]} -eq 0 ] || printf 'reason: %s\n' "${REASONS[@]}"
echo "RECEIPT $final"
echo "VERDICT $verdict"
[ "$verdict" = OMEGA_VISOR_V1_PASS ]
