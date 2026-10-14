#!/usr/bin/env bash
# tools/omega_emit/test_omega_emit.sh <build/omega_emit dir> <oscc> -- tests of omega_emit (omega#445).
# Every check has a counterexample: each refusal is asserted with its stage and reason, and mutants of
# omega_emit (one defence removed at a time) must make a check fail.
set -euo pipefail
BIN="${1:?usage: test_omega_emit.sh <build/omega_emit dir> <oscc>}"
OSCC="${2:?oscc}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
cd "$ROOT"
E="$BIN/omega_emit"
T="$(mktemp -d "${TMPDIR:-/tmp}/omega-emit-test.XXXXXXXX")"
trap 'rm -rf "$T"' EXIT
fails=0
ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# fn wrapper: one function "f" (u32,u32)->u32 with the given locals and blocks
unit() { printf '{"functions": [{"name": "f", "params": ["u32", "u32"], "ret": "u32", "locals": %s, "blocks": %s}]}' "$1" "$2"; }

# accepts <emit binary> <name> <json>: emit must write bytes, and lift -> emit must give the same bytes
accepts() {
  local e="$1" n="$2"; printf '%s' "$3" > "$T/$n.json"
  "$e" emit "$T/$n.json" "$T/$n.oscir" > "$T/$n.out" || return 1
  "$E" lift "$T/$n.oscir" > "$T/$n.lift.json" || return 1
  "$E" emit "$T/$n.lift.json" "$T/$n.re" > /dev/null && cmp -s "$T/$n.oscir" "$T/$n.re"
}
# refuses <emit binary> <name> <expected substring> <json>: exit 1, one REFUSED line containing it, no output file
refuses() {
  local e="$1" n="$2"; printf '%s' "$4" > "$T/$n.json"; rm -f "$T/$n.oscir"
  local rc=0; "$e" emit "$T/$n.json" "$T/$n.oscir" > "$T/$n.out" 2>&1 || rc=$?
  [ "$rc" = 1 ] && [ ! -e "$T/$n.oscir" ] && [ "$(wc -l < "$T/$n.out")" = 1 ] && grep -qF -- "$3" "$T/$n.out"
}

# ---- 1. positive: the schema example, and one unit that uses every op of the schema ---------------
awk '/^```$/ { n++; next } n == 3 { print }' "$HERE/SCHEMA.md" > "$T/example.json"
if [ -s "$T/example.json" ] && accepts "$E" example "$(cat "$T/example.json")"; then ok "SCHEMA.md example emits; lift -> emit byte-identical"
else bad "SCHEMA.md example"; cat "$T/example.out" 2>/dev/null; fi
ALLOPS='{"functions": [
 {"name": "h", "params": ["shared:u8:4", "i64"], "ret": "i64", "locals": ["i64", "u8"],
  "blocks": [[{"op": "const", "dst": 2, "value": 0}, {"op": "load", "dst": 3, "arr": 0, "index": 2},
              {"op": "cast", "dst": 2, "a": 3}, {"op": "add", "dst": 2, "a": 2, "b": 1}, {"op": "ret", "a": 2}]]},
 {"name": "v", "params": ["mut:u8:4"], "ret": "void", "locals": ["u32", "u8"],
  "blocks": [[{"op": "const", "dst": 1, "value": 3}, {"op": "const", "dst": 2, "value": 255},
              {"op": "store", "arr": 0, "index": 1, "value": 2}, {"op": "ret"}]]},
 {"name": "g", "params": ["i64", "u32", "bool"], "ret": "i64",
  "locals": ["i64", "i64", "u32", "u32", "bool", "bool", "own:u8:4", "u8", "i64", "u32", "i16"],
  "blocks": [
   [{"op": "const", "dst": 3, "value": -9223372036854775808}, {"op": "const", "dst": 5, "value": 4294967295},
    {"op": "mov", "dst": 4, "a": 0}, {"op": "sub", "dst": 4, "a": 4, "b": 0}, {"op": "mul", "dst": 4, "a": 4, "b": 0},
    {"op": "const", "dst": 3, "value": 7}, {"op": "div", "dst": 4, "a": 4, "b": 3}, {"op": "rem", "dst": 4, "a": 4, "b": 3},
    {"op": "and", "dst": 6, "a": 1, "b": 5}, {"op": "or", "dst": 6, "a": 6, "b": 5}, {"op": "xor", "dst": 6, "a": 6, "b": 1},
    {"op": "shl", "dst": 6, "a": 6, "b": 1}, {"op": "shr", "dst": 6, "a": 6, "b": 1}, {"op": "neg", "dst": 4, "a": 4},
    {"op": "bnot", "dst": 6, "a": 6}, {"op": "const", "dst": 8, "value": true}, {"op": "not", "dst": 8, "a": 2},
    {"op": "and", "dst": 8, "a": 8, "b": 2},
    {"op": "eq", "dst": 7, "a": 0, "b": 4}, {"op": "ne", "dst": 7, "a": 0, "b": 4}, {"op": "lt", "dst": 7, "a": 0, "b": 4},
    {"op": "le", "dst": 7, "a": 0, "b": 4}, {"op": "gt", "dst": 7, "a": 0, "b": 4}, {"op": "ge", "dst": 7, "a": 0, "b": 4},
    {"op": "const", "dst": 10, "value": 1}, {"op": "alloc", "dst": 9, "init": 10}, {"op": "call", "func": "v", "args": [9]},
    {"op": "call", "dst": 11, "func": "h", "args": [9, 0]}, {"op": "release", "arr": 9},
    {"op": "const", "dst": 13, "value": -32768}, {"op": "cast", "dst": 12, "a": 13},
    {"op": "cbr", "cond": 7, "then": 1, "else": 2}],
   [{"op": "trap", "code": 2}, {"op": "br", "to": 2}],
   [{"op": "ret", "a": 11}]
  ]}
]}'
if accepts "$E" allops "$ALLOPS"; then
  used="$(grep -o '"op": "[a-z]*"' "$T/allops.lift.json" | sort -u | wc -l)"
  [ "$used" = 31 ] && ok "a unit using all 31 ops of the schema emits; lift -> emit byte-identical" || bad "all-ops unit covers $used ops, not 31"
else bad "all-ops unit refused: $(cat "$T/allops.out")"; fi

# ---- 2. lift -> emit is byte-identical over the compiler corpus (units the schema covers) -----------
# corpus_rt <omega_emit>: lift with it, emit with the real one; every covered unit must come back byte-identical
corpus_rt() {
  n_ok=0; n_ref=0; n_bad=0
  for f in tests/compiler/progs/*.osc tests/compiler/slices/*.osc src/osh/*.osc tests/physics0/osc/*.osc; do
    [ -f "$f" ] || continue
    b="c_$(echo "$f" | tr / _)"
    [ -f "$T/$b.ir" ] || "$OSCC" "$f" "$T/$b" > /dev/null 2>&1 || continue
    if "$1" lift "$T/$b.ir" > "$T/$b.json" 2> /dev/null; then
      if "$E" emit "$T/$b.json" "$T/$b.re" > /dev/null && cmp -s "$T/$b.ir" "$T/$b.re"; then n_ok=$((n_ok + 1)); else n_bad=$((n_bad + 1)); fi
    else
      grep -q "^REFUSED lift: " <("$1" lift "$T/$b.ir" 2>&1) && n_ref=$((n_ref + 1)) || n_bad=$((n_bad + 1))
    fi
  done
  [ "$n_ok" -ge 15 ] && [ "$n_bad" = 0 ]
}
corpus_rt "$E" && ok "compiler corpus: $n_ok units lift -> emit byte-identical; $n_ref outside the schema refused by lift with a reason" ||
  bad "corpus round trip ok=$n_ok bad=$n_bad"


# ---- 2b. decoder vs omega_emit, op by op and type by type (typediff.c) ----------------------------
# typediff writes one unit per op x operand-type combination (17 op shapes covering every type rule of
# the schema: the arithmetic, logical and shift groups, comparisons, unary, mov, cast, alloc, release,
# load, store, cbr, ret, const, call arguments and results) with the decoder's verdict. Required: every
# unit the decoder accepts, omega_emit emits byte-identically, and lift -> emit of its bytes is
# byte-identical; every unit the decoder refuses, omega_emit refuses at its own step or validator
# stage (its op-by-op check, the one that gives a model a reason), never only at the whole-unit validator or the final encoding check.
typediff_chk() { # typediff_chk <omega_emit>: prints one line per disagreement
  split -n l/8 -d "$T/td.list" "$T/td.part."
  for p in "$T"/td.part.*; do
    ( while read -r id v; do
        f="$T/td/$id"
        if out="$("$1" emit "$f.json" "$f.$$.re" 2>&1)"; then r=ACCEPT; else r=REFUSE; fi
        if [ "$v" = ACCEPT ]; then
          if [ "$r" = REFUSE ]; then echo "MISS $id: $out"
          elif ! cmp -s "$f.oscir" "$f.$$.re"; then echo "DIFF $id"
          elif ! { "$E" lift "$f.oscir" > "$f.$$.lj" && "$1" emit "$f.$$.lj" "$f.$$.lr" > /dev/null && cmp -s "$f.oscir" "$f.$$.lr"; }; then echo "LIFT $id"; fi
        else
          case "$r/$out" in ACCEPT/*) echo "EXTRA $id" ;; "REFUSE/REFUSED step: "*) ;; *) echo "LATE $id: $out" ;; esac
        fi
        rm -f "$f.$$.re" "$f.$$.lj" "$f.$$.lr"
      done < "$p" ) &
  done
  wait; rm -f "$T"/td.part.*
}
mkdir -p "$T/td"; "$BIN/typediff" "$T/td" > "$T/td.list" 2> /dev/null
n_acc="$(grep -c ' ACCEPT$' "$T/td.list")"; n_ref="$(grep -c ' REFUSE$' "$T/td.list")"
typediff_chk "$E" > "$T/td.res"
if [ ! -s "$T/td.res" ] && [ "$n_acc" -ge 100 ]; then ok "decoder vs omega_emit over $((n_acc + n_ref)) op x type units: all $n_acc decoder-accepted emit byte-identically (and lift -> emit), all $n_ref decoder-refused refused at a step"
else bad "decoder vs omega_emit: $(wc -l < "$T/td.res") disagreements"; head -5 "$T/td.res"; fi
for k in mov_4_w5 mov_4_w6 mov_6_w5 mov_4_w4 call_4_p5 call_6_p6; do grep -qx "$k ACCEPT" "$T/td.list" || bad "typediff: $k should be a decoder-accepted case"; done
for k in mov_5_w4 mov_5_w6 mov_7_w4 mov_8_w5 call_5_p6 call_5_p4; do grep -qx "$k REFUSE" "$T/td.list" || bad "typediff: $k should be a decoder-refused case"; done

# ---- 2c. lift -> emit over the public references and controls --------------------------------------
# the 10 exp445 dev references, the sample task references, the stage-1 PUBLIC sample controls
# (s01..s04; the sealed stage-1 controls are not readable by tool builders) and a borrow fixture whose
# IR moves arrays into own:, mut: and shared: registers (the case that omega_emit once refused).
cat > "$T/borrow.osc" <<'OSC'
fn g(v: u32) -> u32 {
    let a: own [u32; 1] = alloc(v);
    let m: &mut [u32; 1] = &mut a;
    m[0] = v + 1;
    return m[0];
}
fn f(v: u32) -> u32 {
    let a: own [u32; 1] = alloc(v);
    let s: &[u32; 1] = &a;
    return s[0];
}
OSC
refs_rt() { # refs_rt <omega_emit>
  n_rr=0
  for f in tests/one_layer/exp445/dev/tasks/cases/*/hidden/reference.osc tests/one_layer/acceptance/sample/tasks/cases/*/hidden/reference.osc \
           tests/one_layer/acceptance/sample/cases/s0[1-4]/candidate.osc "$T/borrow.osc"; do
    b="r_$(echo "$f" | tr / _)"
    "$OSCC" "$f" "$T/$b" > /dev/null || { echo "oscc refused $f"; return 1; }
    "$E" lift "$T/$b.ir" > "$T/$b.json" || { echo "lift refused $f"; return 1; }
    "$1" emit "$T/$b.json" "$T/$b.re" > /dev/null && cmp -s "$T/$b.ir" "$T/$b.re" || { echo "emit differs $f"; return 1; }
    n_rr=$((n_rr + 1))
  done
}
if refs_rt "$E" > "$T/rr.out"; then ok "public references and controls: $n_rr units lift -> emit byte-identical (incl. the borrow fixture)"; else bad "references round trip: $(cat "$T/rr.out")"; fi
grep -q '"op": "mov", "dst": 2, "a": 1' "$T/r_$(echo "$T/borrow.osc" | tr / _).json" && ok "  the borrow fixture's IR does hold array movs" || bad "borrow fixture has no array mov"

# ---- 3. refusals: stage and reason ---------------------------------------------------------------
R() { # R <name> <expected> <json>
  if refuses "$E" "$1" "$2" "$3"; then ok "refused $1: $(cut -c1-110 "$T/$1.out")"; else bad "$1: expected '$2', got: $(cat "$T/$1.out")"; fi
}
GOOD_BLOCKS='[[{"op": "add", "dst": 2, "a": 0, "b": 1}, {"op": "ret", "a": 2}]]'
R json_trailing_comma 'json: expected a key string' "$(unit '["u32"]' "$GOOD_BLOCKS" | sed 's/}]}$/,}]}/')"
R json_duplicate_key 'json: duplicate key "op"' "$(unit '["u32"]' '[[{"op": "add", "op": "sub", "dst": 2, "a": 0, "b": 1}, {"op": "ret", "a": 2}]]')"
R json_trailing_data 'json: trailing data' "$(unit '["u32"]' "$GOOD_BLOCKS") x"
R json_non_ascii 'json: control or non-ASCII' "$(unit '["u32"]' "$GOOD_BLOCKS" | sed 's/"f"/"f\xc3\xa9"/')"
R schema_top_key 'schema: top level: unknown key "units"' '{"functions": [], "units": 1}'
R schema_no_functions 'schema: "functions" must be an array of 1..32' '{"functions": []}'
R schema_missing_locals 'schema: function f: missing key "locals"' '{"functions": [{"name": "f", "params": [], "ret": "u32", "blocks": []}]}'
R schema_bad_type 'schema: function f: register r2: type must be' "$(unit '["u31"]' "$GOOD_BLOCKS")"
R schema_bad_name 'schema: functions[0]: "name" must match' '{"functions": [{"name": "9f", "params": [], "ret": "u32", "locals": [], "blocks": [[{"op": "ret"}]]}]}'
R schema_dup_func 'schema: function f: a function with this name already exists' "{\"functions\": [$(unit '["u32"]' "$GOOD_BLOCKS" | sed 's/^{"functions": \[//; s/\]}$//'), $(unit '["u32"]' "$GOOD_BLOCKS" | sed 's/^{"functions": \[//; s/\]}$//')]}"
R schema_empty_block 'schema: function f: block 0 must be a non-empty array' "$(unit '[]' '[[]]')"
R step_unknown_op 'step: function f block 0 op 0: unknown op "addi"' "$(unit '["u32"]' '[[{"op": "addi", "dst": 2, "a": 0, "b": 1}, {"op": "ret", "a": 2}]]')"
R step_missing_field 'step: function f block 0 op 0 (add): missing field "b"' "$(unit '["u32"]' '[[{"op": "add", "dst": 2, "a": 0}, {"op": "ret", "a": 2}]]')"
R step_unknown_field 'step: function f block 0 op 0 (add): unknown field "c"' "$(unit '["u32"]' '[[{"op": "add", "dst": 2, "a": 0, "b": 1, "c": 1}, {"op": "ret", "a": 2}]]')"
R step_reg_range 'step: function f block 0 op 0 (add): "b" is register 3 but this function has registers 0..2' "$(unit '["u32"]' '[[{"op": "add", "dst": 2, "a": 0, "b": 3}, {"op": "ret", "a": 2}]]')"
R step_type_mismatch 'step: function f block 0 op 1 (add): arithmetic needs a and b of the same integer type' "$(unit '["i32", "u32"]' '[[{"op": "const", "dst": 2, "value": 1}, {"op": "add", "dst": 3, "a": 0, "b": 2}, {"op": "ret", "a": 3}]]')"
R step_const_range 'step: function f block 0 op 0 (const): value 256 does not fit u8' "$(unit '["u8"]' '[[{"op": "const", "dst": 2, "value": 256}, {"op": "ret", "a": 0}]]')"
R step_const_negative 'step: function f block 0 op 0 (const): value -1 is negative' "$(unit '["u32"]' '[[{"op": "const", "dst": 2, "value": -1}, {"op": "ret", "a": 2}]]')"
R step_const_fraction 'step: function f block 0 op 0 (const): "value" must be a whole decimal number' "$(unit '["u32"]' '[[{"op": "const", "dst": 2, "value": 1.5}, {"op": "ret", "a": 2}]]')"
R step_const_i8_low 'value -129 does not fit i8' "$(unit '["i8"]' '[[{"op": "const", "dst": 2, "value": -129}, {"op": "ret", "a": 0}]]')"
R step_term_mid 'step: function f block 0 op 0 (ret): br, cbr and ret end a block' "$(unit '[]' '[[{"op": "ret", "a": 0}, {"op": "ret", "a": 1}]]')"
R step_no_term 'step: function f block 0 op 0 (add): the last op of a block must be br, cbr or ret' "$(unit '["u32"]' '[[{"op": "add", "dst": 2, "a": 0, "b": 1}]]')"
R step_blk_range 'step: function f block 0 op 0 (br): "to" is block 1 but this function has blocks 0..0' "$(unit '[]' '[[{"op": "br", "to": 1}]]')"
R step_cond_bool 'step: function f block 0 op 0 (cbr): cond r0 is u32, not bool' "$(unit '[]' '[[{"op": "cbr", "cond": 0, "then": 0, "else": 0}]]')"
R step_ret_type 'step: function f block 0 op 0 (ret): r2 is u8; this function returns u32' "$(unit '["u8"]' '[[{"op": "const", "dst": 2, "value": 1}, {"op": "ret", "a": 2}]]' | sed 's/{"op": "const", "dst": 2, "value": 1}, //')"
R step_call_later 'step: function f block 0 op 0 (call): no function named "g" before this one' "$(unit '["u32"]' '[[{"op": "call", "dst": 2, "func": "g", "args": [0]}, {"op": "ret", "a": 2}]]')"
R step_call_arity 'takes 2 arguments, got 1' "{\"functions\": [$(unit '["u32"]' "$GOOD_BLOCKS" | sed 's/^{"functions": \[//; s/\]}$//'), {\"name\": \"g\", \"params\": [\"u32\"], \"ret\": \"u32\", \"locals\": [\"u32\"], \"blocks\": [[{\"op\": \"call\", \"dst\": 1, \"func\": \"f\", \"args\": [0]}, {\"op\": \"ret\", \"a\": 1}]]}]}"
R step_trap_code 'step: function f block 0 op 0 (trap): trap code must be 1..14' "$(unit '[]' '[[{"op": "trap", "code": 0}, {"op": "ret", "a": 0}]]')"
R step_store_shared 'store needs an own: or mut: array' '{"functions": [{"name": "s", "params": ["shared:u8:4"], "ret": "void", "locals": ["u32", "u8"], "blocks": [[{"op": "const", "dst": 1, "value": 0}, {"op": "const", "dst": 2, "value": 0}, {"op": "store", "arr": 0, "index": 1, "value": 2}, {"op": "ret"}]]}]}'
R validator_use_before_def 'validator: function f block 0 op 0 (add): vreg 3 may be used before it is defined' "$(unit '["u32", "u32"]' '[[{"op": "add", "dst": 2, "a": 0, "b": 3}, {"op": "ret", "a": 2}]]')"
R validator_def_one_path 'validator: function f block 3 op 0 (ret): vreg 2 may be used before it is defined' "$(unit '["u32", "bool"]' '[[{"op": "lt", "dst": 3, "a": 0, "b": 1}, {"op": "cbr", "cond": 3, "then": 1, "else": 2}], [{"op": "mov", "dst": 2, "a": 0}, {"op": "br", "to": 3}], [{"op": "br", "to": 3}], [{"op": "ret", "a": 2}]]')"
rm -f "$T/input_missing.json"; if { "$E" emit "$T/input_missing.json" "$T/x.oscir" || true; } | grep -q '^REFUSED input: cannot read'; then ok "missing input file refused"; else bad "missing input"; fi
# usage
if "$E" frob > /dev/null 2>&1; then bad "unknown command accepted"; else ok "unknown command refused (usage, exit 2)"; fi

# ---- 4. mutants: one defence removed at a time; a check above must catch it ------------------------
echo "mutants (each must be caught):"
SRCS="$(ls src/compiler/*.c | grep -v oscc_main) src/sha256.c"
mut() { # mut <name> <sed>
  sed "$2" "$HERE/omega_emit.c" > "$T/m_$1.c"
  cmp -s "$HERE/omega_emit.c" "$T/m_$1.c" && { bad "mutant $1: sed changed nothing"; return 1; }
  ${CC:-cc} -std=gnu11 -w -D_GNU_SOURCE -O1 -Isrc -Isrc/compiler -o "$T/m_$1" "$T/m_$1.c" $SRCS || { bad "mutant $1 does not compile"; return 1; }
}
not() { ! "$@"; }
caught() { if "$@"; then ok "  mutant caught: $M"; else bad "  mutant survived: $M"; fi; }
M="no whole-unit validation"; mut novalidate 's/if (osc_ir_validate(&g_u, err, sizeof err)) {/if (0) {/' &&
  caught not refuses "$T/m_novalidate" mv1 'validator: function f block 0 op 0 (add): vreg 3 may be used' "$(unit '["u32", "u32"]' '[[{"op": "add", "dst": 2, "a": 0, "b": 3}, {"op": "ret", "a": 2}]]')"
M="no step type check for arithmetic"; mut nobintype 's/} else if (!is_int(A) || !teq(A, B))/} else if (0)/' &&
  caught not refuses "$T/m_nobintype" mv2 'step: function f block 0 op 1 (add): arithmetic needs' "$(unit '["i32", "u32"]' '[[{"op": "const", "dst": 2, "value": 1}, {"op": "add", "dst": 3, "a": 0, "b": 2}, {"op": "ret", "a": 3}]]')"
M="const range unchecked"; mut noconst 's/if (w < 64 \&\& (m >> w))/if (0)/' &&
  caught not refuses "$T/m_noconst" mv3 'step: function f block 0 op 0 (const): value 256 does not fit u8' "$(unit '["u8"]' '[[{"op": "const", "dst": 2, "value": 256}, {"op": "ret", "a": 0}]]')"
M="unknown fields ignored"; mut nofields 's/if (!ok) return STEP(AT "unknown field/if (0) return STEP(AT "unknown field/' &&
  caught not refuses "$T/m_nofields" mv4 'unknown field "c"' "$(unit '["u32"]' '[[{"op": "add", "dst": 2, "a": 0, "b": 1, "c": 1}, {"op": "ret", "a": 2}]]')"
lifts_value() { printf "%s" "$2" > "$T/lv.json"; "$1" emit "$T/lv.json" "$T/lv.oscir" > /dev/null && "$E" lift "$T/lv.oscir" | grep -qF "$3"; }
lifts_value "$E" "$(unit '["i32"]' '[[{"op": "const", "dst": 2, "value": -5}, {"op": "ret", "a": 0}]]')" '"value": -5' && ok "const -5 (i32) comes back from the bytes as -5" || bad "const -5 round trip"
M="signed constants not sign-extended"; mut nosext 's/in->imm = neg ? (uint64_t)0 - m : m;/in->imm = m;/' &&
  caught not lifts_value "$T/m_nosext" "$(unit '["i32"]' '[[{"op": "const", "dst": 2, "value": -5}, {"op": "ret", "a": 0}]]')" '"value": -5'
M="lift prints unsigned constants off by one"; mut liftoff '/EMIT:lift-const/s/x->imm);/x->imm ^ 1);/' && caught not corpus_rt "$T/m_liftoff"
M="duplicate JSON keys accepted"; mut nodup 's/if (!strcmp(j->key\[q\], k)) { fail/if (0) { fail/' &&
  caught not refuses "$T/m_nodup" mv6 'json: duplicate key' "$(unit '["u32"]' '[[{"op": "add", "op": "sub", "dst": 2, "a": 0, "b": 1}, {"op": "ret", "a": 2}]]')"
td_clean() { typediff_chk "$1" > "$T/td.m"; [ ! -s "$T/td.m" ]; }
M="array mov refused (the old scalar-only mov rule)"; mut movref '/EMIT:mov-ref/s/if (is_arr(&T\[in->dst\])) {/if (0) {/' && caught not td_clean "$T/m_movref"
M="shared array bound into mut: at a step"; mut refmut 's/(p->ref == OSC_REF_MUT \&\& a->ref != OSC_REF_SHARED)/(p->ref == OSC_REF_MUT)/' && caught not td_clean "$T/m_refmut"
refs_q() { refs_rt "$1" > /dev/null; }
M="borrow fixture: array mov refused"; caught not refs_q "$T/m_movref"
echo
if [ "$fails" -eq 0 ]; then echo "test-omega-emit: PASS"; else echo "test-omega-emit: FAIL ($fails)"; exit 1; fi
