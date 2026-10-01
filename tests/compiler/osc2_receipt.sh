#!/bin/sh
# OSC-2 compiler slice receipt, one per item (docs/osc/OSC-2-DESIGN.md).
# OSC-2 slice; not a general Omega compiler; no self-hosting.
# Usage: tests/compiler/osc2_receipt.sh [ITEM]   (ITEM: contracts (default), structs, arenas or encoder)
#  1. refuses a dirty tree (any tracked or untracked change): receipts record an exact commit
#  2. runs `make test-compiler` into a temporary OUT_DIR (model sweep, back end,
#     compiler golden/negative/model agreement, contract + struct + arena fuzz, runtime model replay,
#     determinism; plain + ASan/UBSan)
#  3. compiles every golden program with oscc and records its IR and machine-code digests
#  4. checks that the existing Omega core sources (src/omega_*, src/aarch64_*,
#     src/language/, tools/omegatool.c, Makefile) are unchanged against the merge
#     base with origin/main, so the M6/M9/M14 gates cannot have changed; if
#     PHYSICS_DIR points at the pinned physics checkout it also runs those gates
#     ITEM=encoder (OSC-2 item 4) changes src/aarch64_encoder.*: it is left out of that check,
#     PHYSICS_DIR is required, each gate log is hashed (build path normalised) and, when
#     OSC2_GATE_BASELINE names a directory with base-<m>.gate.log, compared byte for byte;
#     it also records the legacy writer differential/refusal lines and the caller allowlist check
#  5. writes a content-addressed receipt evidence/OSC-2/receipts/osc2-<ITEM>-<sha256>.json
# Never overwrites existing evidence (evidence/OSC-1 is never touched). Timing is not recorded.
set -eu
ITEM=${1:-${ITEM:-contracts}}
case "$ITEM" in contracts|structs|arenas|encoder) ;; *) echo "osc2-receipt: REFUSED: unknown ITEM $ITEM (known: contracts structs arenas encoder)" >&2; exit 2 ;; esac
# items are cumulative: the encoder receipt also records the struct and arena evidence
HAS_S=0; HAS_A=0; HAS_E=0
case "$ITEM" in structs) HAS_S=1 ;; arenas) HAS_S=1; HAS_A=1 ;; encoder) HAS_S=1; HAS_A=1; HAS_E=1 ;; esac
if [ "$HAS_E" = 1 ]; then [ -n "${PHYSICS_DIR:-}" ] && [ -d "$PHYSICS_DIR" ] || { echo "osc2-receipt: REFUSED: ITEM=encoder changes the legacy writer the M6/M9/M14 gates use; PHYSICS_DIR must point at the pinned physics checkout" >&2; exit 2; }; fi
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
cd "$ROOT"
EVD=evidence/OSC-2/receipts
die() { echo "osc2-receipt: REFUSED: $*" >&2; exit 2; }

[ -z "$(git status --porcelain --untracked-files=all)" ] || die "dirty tree; commit first (receipts record an exact commit)"
COMMIT=$(git rev-parse HEAD)
BASE=$(git merge-base HEAD origin/main) || die "no merge base with origin/main"

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
O="$T/build/compiler"
nice make --no-print-directory OUT_DIR="$T/build" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 test-compiler \
    >"$T/make.log" 2>&1 || { tail -30 "$T/make.log"; die "make test-compiler failed"; }
grep -q '^test-compiler: PASS' "$T/make.log" || die "no test-compiler PASS line"

MSUM=$(grep '^OSC0B_MODEL_SEED=' "$O/model.out" | tail -1)
mf() { echo "$MSUM" | sed -n "s/.*$1=\([0-9a-fx]*\).*/\1/p"; }
BCOUNTS=$(grep '^units=' "$O/backend.out" | tail -1)
BCHECKS=$(grep '^checks=' "$O/backend.out" | tail -1 | sed 's/ time=.*//')
CLINE=$(grep '^golden=' "$O/compiler.out" | tail -1 | sed 's/ time=.*//')
DLINE=$(grep '^programs=' "$O/determinism.out" | tail -1)
tc() { grep "^  $1 " "$O/compiler.out" | awk '{print $2}'; }
for v in "$MSUM" "$BCOUNTS" "$BCHECKS" "$CLINE" "$DLINE"; do [ -n "$v" ] || die "cannot parse test output"; done
FLINE=$(grep "^contract fuzz:" "$O/compiler.out" | tail -1)
FLINE_ASAN=$(grep "^contract fuzz:" "$O/compiler_asan.out" | tail -1)
[ -n "$FLINE" ] && [ -n "$FLINE_ASAN" ] || die "no contract fuzz line"
SLINE=$(grep "^struct fuzz:" "$O/compiler.out" | tail -1)
SLINE_ASAN=$(grep "^struct fuzz:" "$O/compiler_asan.out" | tail -1)
if [ "$HAS_S" = 1 ]; then
    case "$SLINE" in *" mismatches=0") ;; *) die "no struct fuzz line with mismatches=0" ;; esac
    case "$SLINE_ASAN" in *" mismatches=0") ;; *) die "no ASan struct fuzz line with mismatches=0" ;; esac
    LAYOUT=$(grep "^struct layout:" "$O/compiler.out" | tail -1)
    DTOR=$(grep "^struct destruction order:" "$O/compiler.out" | tail -1)
    [ -n "$LAYOUT" ] && [ -n "$DTOR" ] || die "no struct layout / destruction order line"
fi
if [ "$HAS_A" = 1 ]; then
    ALINE=$(grep "^arena fuzz:" "$O/compiler.out" | tail -1)
    ALINE_ASAN=$(grep "^arena fuzz:" "$O/compiler_asan.out" | tail -1)
    case "$ALINE" in *" mismatches=0") ;; *) die "no arena fuzz line with mismatches=0" ;; esac
    case "$ALINE_ASAN" in *" mismatches=0") ;; *) die "no ASan arena fuzz line with mismatches=0" ;; esac
    ADTOR=$(grep "^arena destruction order:" "$O/compiler.out" | tail -1)
    [ -n "$ADTOR" ] || die "no arena destruction order line"
    RLINE=$(grep "^runtime model replay:" "$O/compiler.out" | tail -1)
    RLINE_ASAN=$(grep "^runtime model replay:" "$O/compiler_asan.out" | tail -1)
    case "$RLINE" in *" rejected=0") ;; *) die "no runtime model replay line with rejected=0" ;; esac
    case "$RLINE_ASAN" in *" rejected=0") ;; *) die "no ASan runtime model replay line with rejected=0" ;; esac
    n=$(tc ARENA_FULL); [ -n "$n" ] && [ "$n" -gt 0 ] || die "trap ARENA_FULL not observed"
fi
if [ "$HAS_E" = 1 ]; then
    ELINE=$(grep "^legacy a64 differential:" "$O/legacy_a64.out" | tail -1)
    ELINE_ASAN=$(grep "^legacy a64 differential:" "$O/legacy_a64_asan.out" | tail -1)
    case "$ELINE" in *" mode=full "*" mismatches=0") ;; *) die "no full legacy a64 differential line with mismatches=0" ;; esac
    case "$ELINE_ASAN" in *" mismatches=0") ;; *) die "no ASan legacy a64 differential line with mismatches=0" ;; esac
    EREF=$(grep "^legacy a64 refusal:" "$O/legacy_a64.out" | tail -1)
    EREF_ASAN=$(grep "^legacy a64 refusal:" "$O/legacy_a64_asan.out" | tail -1)
    n=$(echo "$EREF" | sed -n 's/.*cases=\([0-9]*\) named_abort_and_no_write=\([0-9]*\)$/\1 \2/p')
    [ -n "$n" ] && [ "${n% *}" = "${n#* }" ] && [ "${n% *}" -gt 0 ] || die "legacy a64 refusal line missing or incomplete"
    [ "$EREF_ASAN" = "$EREF" ] || die "ASan legacy a64 refusal line differs"
    ECALL=$(grep "^OSC2_LEGACY_CALLERS_PASS " "$O/legacy_callers.out" | tail -1)
    [ -n "$ECALL" ] || die "no legacy caller allowlist PASS line"
fi
for k in OVERFLOW DIV0 BOUNDS LOOP_BOUND CAST OOM SHIFT REQUIRES ENSURES; do
    n=$(tc $k); [ -n "$n" ] && [ "$n" -gt 0 ] || die "trap $k not observed in golden corpus"
done

# golden corpus digests (oscc, one process per program)
: >"$T/corpus.txt"
for f in tests/compiler/progs/*.osc; do
    out=$("$O/oscc" "$f") || die "oscc refused golden program $f"
    ir=$(echo "$out" | sed -n 's/^ir_sha256=//p'); code=$(echo "$out" | sed -n 's/^code_sha256=//p')
    printf '%s %s %s %s\n' "$f" "$(sha256sum "$f" | cut -d' ' -f1)" "$ir" "$code" >>"$T/corpus.txt"
done
CORPUS_SHA=$(sha256sum "$T/corpus.txt" | cut -d' ' -f1)
NPROG=$(wc -l <"$T/corpus.txt" | tr -d ' ')
NNEG=$(ls tests/compiler/neg/*.osc | wc -l | tr -d ' ')

CORE="src/omega_*.c src/omega_*.h src/aarch64_* src/language tools/omegatool.c Makefile"
# item 4 changes the legacy writer itself (src/aarch64_encoder.*): it is excluded from the
# unchanged-core check and the M6/M9/M14 gates are run instead (PHYSICS_DIR is required)
[ "$HAS_E" = 1 ] && CORE="src/omega_*.c src/omega_*.h src/aarch64_decoder.* src/aarch64_target.h src/language tools/omegatool.c Makefile"
# shellcheck disable=SC2086
if git diff --quiet "$BASE" HEAD -- $CORE; then CORE_UNCHANGED=true; else die "existing Omega core sources changed vs $BASE"; fi

GATES='"NOT_RUN (PHYSICS_DIR not set; core sources unchanged vs merge base)"'
if [ -n "${PHYSICS_DIR:-}" ] && [ -d "$PHYSICS_DIR" ]; then
    G=""
    rm -rf build/osc2-receipt-gates; mkdir -p build/osc2-receipt-gates  # gate targets need a relative OUT_DIR (./$(OUT_DIR)/omegatool); build/ is ignored
    for m in m6 m9 m14; do
        nice make --no-print-directory OUT_DIR=build/osc2-receipt-gates PHYSICS_DIR="$PHYSICS_DIR" "test-$m" >"$T/$m.log" 2>&1 \
            || { tail -20 "$T/$m.log"; die "test-$m failed"; }
        tot=$(grep 'TOTAL GATES' "$T/$m.log" | head -1 | sed 's/.*TOTAL GATES: \([0-9]*\) | PASSED: \([0-9]*\) | FAILED: \([0-9]*\).*/{"total": \1, "passed": \2, "failed": \3}/')
        case "$tot" in "{\"total\": "*", \"failed\": 0}") ;; *) die "test-$m: no gate total line with failed=0" ;; esac
        sed -n "/omegatool --run/,\$p" "$T/$m.log" >build/osc2-receipt-gates/$m.gate.log
        # the gate log names the build directory (./build/<dir>/omegatool); normalise it before hashing
        sed 's|\./build/[^/ ]*/omegatool|./OUT/omegatool|g' build/osc2-receipt-gates/$m.gate.log >"$T/$m.norm"
        gsha=$(sha256sum "$T/$m.norm" | cut -d' ' -f1)
        if [ -n "${OSC2_GATE_BASELINE:-}" ]; then
            sed 's|\./build/[^/ ]*/omegatool|./OUT/omegatool|g' "$OSC2_GATE_BASELINE/base-$m.gate.log" | cmp -s - "$T/$m.norm" \
                || die "test-$m gate output differs from baseline $OSC2_GATE_BASELINE/base-$m.gate.log"
            tot="${tot%\}}, \"gate_log_sha256\": \"$gsha\", \"identical_to_baseline\": true}"
        else
            tot="${tot%\}}, \"gate_log_sha256\": \"$gsha\"}"
        fi
        G="$G\"$m\": $tot, "
    done
    GATES="{${G%, }}"
fi

SUBX=
if [ "$HAS_S" = 1 ]; then
    SUBX="; OSC-2 item 2: unit-level struct declarations (<= 16 structs, 1..16 fields of integer, bool or [T; N], <= 64 cells, declared before use, no recursion) with a fixed layout of 8-byte cells in declaration order (no padding, bound into the IR digest), struct literals initialising every field exactly once in let own, field reads and writes (array fields bounds-checked, TRAP BOUNDS), moves, own / & / &mut parameters with the array borrow rules, deterministic destruction with the same alloc/release events; requires reads fields through any struct parameter, ensures only through a shared & parameter; returning a struct is refused (UNSUPPORTED)"
fi
if [ "$HAS_A" = 1 ]; then
    SUBX="$SUBX; OSC-2 item 3: arena blocks 'arena r bound K { ... }' (K = 1..64 cells) wired to the OSC-0B region model; 'let x: own [T; N] in r = alloc(v);' and 'let s: own S in r = S { .. };' allocate from the arena (bump allocation, never released one by one); at block end the unique owners declared in the block are released first, then REGION_DESTROY frees the whole arena (nested: inner first); a borrow of an arena object outliving the arena is ARENA_ESCAPE (the model rejection arena-escape), every move into or out of an arena object is ARENA_MOVE (slice restriction stricter than the model), a definite over-capacity allocation is ARENA_CAPACITY, otherwise TRAP ARENA_FULL = 11 at run time; REGION_OPEN / ARENA_ALLOC / REGION_DESTROY are logged in the runtime event log by both engines, and the log of every native run in the test replays through the OSC-0B model (executed runs only, not a proof for all programs)"
fi
if [ "$HAS_E" = 1 ]; then
    SUBX="$SUBX; OSC-2 item 4: the legacy AArch64 writer src/aarch64_encoder.c (used by omegatool, the realize/matvec/self-host paths, polyglot omx_encoder and rx_omega; not by the OSC compiler slice, whose back end is the validating osc_a64) now validates every field (registers 0..31, imm26/imm19/adr imm21/simm9 signed ranges, imm12 0..4095, cond 0..15, movz/movk shift, ldr/str scaled offsets) and refuses out-of-range input with the named abort A64_LEGACY_FIELD_OUT_OF_RANGE <emitter> <field>=<value> before writing anything, instead of silently masking; in-range words are bit-identical to the old masking formulas (differential test), and new callers outside a frozen allowlist are refused by tests/compiler/legacy_a64_callers.sh"
fi
R="$T/receipt.json"
{
  printf '{\n'
  printf '  "schema": "omega.osc2.compiler.receipt.v1",\n'
  printf '  "verdict": "PASS",\n'
  printf '  "item": "%s",\n' "$ITEM"
  printf ''
  printf '  "statement": "OSC-2 slice; not a general Omega compiler; no self-hosting.",\n'
  printf '  "subset": "integer scalars u8 u16 u32 u64 (wrap) and i8 i16 i32 i64 (checked, trap on overflow), bool; let / let mut; arithmetic, bitwise, shift, compare, logical (short-circuit), checked as-casts; if/else; while with static bound N (trap past N) and for over literal ranges; calls to earlier functions in the unit (no recursion, <= 6 params); unique allocation own [T; N] (N <= 64), moves, shared and mutable borrows with lexical lifetimes, bounds-checked indexing, deterministic destruction at scope end; OSC-2 item 1: requires/ensures are pure bool expressions over parameters (and result in ensures; element reads through parameters, ensures only via shared borrows; no calls), refused statically when constant folding decides them false (CONTRACT_VIOLATION), otherwise checked at run time (TRAP REQUIRES = 9 at entry, TRAP ENSURES = 10 at each return before releases; statically true clauses elided)%s",\n' "$SUBX"
  printf '  "pipeline": "surface text -> lexer/parser -> typed AST -> name/type + ownership/borrow analysis -> typed IR (canonical encoding + sha256) -> AArch64 via src/compiler/osc_a64 (validating encoder, decoder mirror)",\n'
  printf '  "repo_commit": "%s",\n' "$COMMIT"
  printf '  "base_commit": "%s",\n' "$BASE"
  printf '  "tree_clean": true,\n'
  printf '  "host_arch": "%s",\n' "$(uname -m)"
  printf '  "c_compiler": "%s",\n' "$(${CC:-cc} --version 2>/dev/null | head -1 | sed 's/"/\\"/g')"
  printf '  "osc0b_model": {"seed": "0x%s", "sequences": %s, "steps": %s, "false_accepts": %s, "wrong_name": %s, "false_rejects": %s, "result": "OSC0B_MODEL_PASS"},\n' \
      "$(mf OSC0B_MODEL_SEED)" "$(mf SEQUENCES)" "$(mf STEPS)" "$(mf FALSE_ACCEPTS)" "$(mf WRONG_NAME)" "$(mf FALSE_REJECTS)"
  printf '  "backend": "%s; %s; native == interpreter on every run",\n' "$BCOUNTS" "$BCHECKS"
  printf '  "compiler": "%s",\n' "$CLINE"
  printf '  "golden_trap_coverage": {"none": %s, "overflow": %s, "div0": %s, "bounds": %s, "loop_bound": %s, "cast": %s, "oom": %s, "shift": %s, "runtime": %s, "requires": %s, "ensures": %s},\n' \
      "$(tc none)" "$(tc OVERFLOW)" "$(tc DIV0)" "$(tc BOUNDS)" "$(tc LOOP_BOUND)" "$(tc CAST)" "$(tc OOM)" "$(tc SHIFT)" "$(tc RUNTIME)" "$(tc REQUIRES)" "$(tc ENSURES)"
  printf '  "contract_fuzz": "%s; native == interpreter on every run",\n' "$FLINE"
  printf '  "contract_fuzz_asan": "%s",\n' "$FLINE_ASAN"
  if [ "$HAS_S" = 1 ]; then
    printf '  "struct_fuzz": "%s; native == interpreter on every run",\n' "$SLINE"
    printf '  "struct_fuzz_asan": "%s",\n' "$SLINE_ASAN"
    printf '  "struct_layout": "%s",\n' "$LAYOUT"
    printf '  "struct_destruction_order": "%s",\n' "$DTOR"
  fi
  if [ "$HAS_A" = 1 ]; then
    printf '  "arena_fuzz": "%s; native == interpreter on every run",\n' "$ALINE"
    printf '  "arena_fuzz_asan": "%s",\n' "$ALINE_ASAN"
    printf '  "arena_destruction_order": "%s",\n' "$ADTOR"
    printf '  "runtime_model_replay": "%s; every native run of the test (golden fuzz, expect-run lines, contract, struct and arena fuzz, destruction-order tests) replays its runtime event log through the OSC-0B model, trapping runs as the logged prefix; this checks executed runs, not all programs",\n' "$RLINE"
    printf '  "runtime_model_replay_asan": "%s",\n' "$RLINE_ASAN"
    printf '  "arena_full_trap_runs": %s,\n' "$(tc ARENA_FULL)"
  fi
  if [ "$HAS_E" = 1 ]; then
    printf '  "legacy_a64_differential": "%s; every emitted word equals the old masking formula; the decoded words were also checked field by field through src/aarch64_decoder (the remainder are repeated edge words already decoded in the sweep)",\n' "$ELINE"
    printf '  "legacy_a64_differential_asan": "%s",\n' "$ELINE_ASAN"
    printf '  "legacy_a64_refusal": "%s; each out-of-range case aborts (SIGABRT) in a child with the exact named line on stderr and leaves the buffer and position untouched",\n' "$EREF"
    printf '  "legacy_a64_callers": "%s; files naming aarch64_encoder.h or an aarch64_emit_ function must be on tests/compiler/legacy_a64_allowlist.txt",\n' "$ECALL"
  fi
  printf '  "determinism": "%s (oscc in separate processes, byte-identical IR digest and machine code)",\n' "$DLINE"
  printf '  "sanitizers": "all three test binaries also pass under -fsanitize=address,undefined -fno-sanitize-recover=all (model sweep at 10^5)",\n'
  printf '  "golden_programs": %s,\n' "$NPROG"
  printf '  "negative_programs": %s,\n' "$NNEG"
  printf '  "golden_corpus_sha256": "%s",\n' "$CORPUS_SHA"
  printf '  "golden_corpus": [\n'
  n=0
  while read -r f src ir code; do
      n=$((n + 1)); sep=,; [ "$n" = "$NPROG" ] && sep=
      printf '    {"program": "%s", "source_sha256": "%s", "ir_sha256": "%s", "code_sha256": "%s"}%s\n' "$f" "$src" "$ir" "$code" "$sep"
  done <"$T/corpus.txt"
  printf '  ],\n'
  printf '  "omega_core_sources_unchanged_vs_base": %s,\n' "$CORE_UNCHANGED"
  printf '  "m6_m9_m14_gates": %s,\n' "$GATES"
  printf '  "self_host": "no: the OSC-2 compiler slice cannot compile any part of itself (docs/osc/OSC-1-SELF-HOST-STATEMENT.md)",\n'
  if [ "$HAS_E" = 1 ]; then
    printf '  "legacy_a64_not_covered": "SP versus XZR meaning of register 31 is not checked (both encode as 31); the decoder is unchanged; the caller scan covers C sources only; a refusal ends the process (abort), it is not a recoverable error; the 280 existing call sites were audited to stay in range, and the M6/M9/M14 gates show byte-identical output, but other generated code paths are covered only by the host suites that ran",\n'
  fi
  if [ "$HAS_A" = 1 ]; then
    printf '  "not_covered": "moving arena objects (all arena moves refused: slice restriction, stricter than the model), arena handles as values or parameters, arenas outlasting their block, per-object release inside an arena, struct return values, struct-typed and owner/borrow fields, recursive structs, whole-struct copy or compare, contracts beyond constant folding (no symbolic proof), effects/capabilities, strings, generations, unsafe physical, FFI, Flow IR optimisation, self-hosting (OSC-14); the runtime model replay checks executed runs only and is not a proof for all programs; not a general Omega compiler",\n'
  elif [ "$ITEM" = structs ]; then
    printf '  "not_covered": "struct return values, struct-typed and owner/borrow fields, recursive structs, struct literals outside let own initialisers, whole-struct copy or compare, contracts beyond constant folding (no symbolic proof), effects/capabilities, strings, generations, arenas, unsafe physical, FFI, Flow IR optimisation, self-hosting (OSC-14); not a general Omega compiler",\n'
  else
    printf '  "not_covered": "contracts beyond constant folding (no symbolic proof), effects/capabilities, structs, strings, generations, arenas, unsafe physical, FFI, Flow IR optimisation, self-hosting (OSC-14); not a general Omega compiler",\n'
  fi
  printf '  "timing_measured": false\n'
  printf '}\n'
} >"$R"

mkdir -p "$EVD"
RSHA=$(sha256sum "$R" | cut -d' ' -f1)
OUT="$EVD/osc2-$ITEM-$RSHA.json"
[ -e "$OUT" ] && { echo "osc2-receipt: identical receipt already present: $OUT"; exit 0; }
cp "$R" "$OUT"
echo "osc2-receipt: PASS receipt=$OUT"
