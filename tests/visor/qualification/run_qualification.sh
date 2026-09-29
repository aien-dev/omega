#!/usr/bin/env bash
# Omega Visor V1 -- lane 8 qualification runner (user-perspective / adversarial).
#
# usage: tests/visor/qualification/run_qualification.sh [OMEGA_BINARY]
#   OMEGA_BINARY  default: build/q8/omega (build it with
#                 make OUT_DIR=build/q8 PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 build/q8/omega)
#   env QUAL_WORK  scratch dir (default build/q8/qual-work; wiped at start)
#
# Every assertion of the console campaign lives here or in the *.omega-session /
# *.expected files next to this script. Output lines:
#   QUAL <section> <case> PASS|FAIL [detail]
#   QUAL_SECTION <section> run=<n> failed=<n>
#   QUAL_HOSTILE run=<n> failed_closed=<n> crashed=<n> noop=<n>
#   QUAL_DETERMINISM runs=<n> identical=<yes|no>
# Exit: 0 when every case passed, 1 otherwise.
set -u
LC_ALL=C
export LC_ALL

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
cd "$ROOT" || exit 2
Q=tests/visor/qualification
OMEGA=${1:-${OMEGA:-build/q8/omega}}
WORK=${QUAL_WORK:-build/q8/qual-work}
EVROOT=tests/visor/fixtures/evidence
rm -rf "$WORK"
mkdir -p "$WORK"

if [ ! -x "$OMEGA" ]; then
    echo "QUAL setup binary FAIL not executable: $OMEGA"
    exit 1
fi

declare -A RUN FAILED
SECTIONS="semantic determinism realize hostile authority scriptability usability"
for s in $SECTIONS; do RUN[$s]=0; FAILED[$s]=0; done
H_RUN=0; H_CLOSED=0; H_CRASH=0; H_NOOP=0

pass() { RUN[$1]=$(( ${RUN[$1]} + 1 )); echo "QUAL $1 $2 PASS${3:+ $3}"; }
fail() { RUN[$1]=$(( ${RUN[$1]} + 1 )); FAILED[$1]=$(( ${FAILED[$1]} + 1 )); echo "QUAL $1 $2 FAIL${3:+ $3}"; }
check() { # check <section> <case> <condition-exit-code> [detail]
    if [ "$3" -eq 0 ]; then pass "$1" "$2" "${4:-}"; else fail "$1" "$2" "${4:-}"; fi
}

# om <outfile> <args...> : run omega with stdin closed, capture stdout+stderr, echo rc
om() {
    local out=$1; shift
    "$OMEGA" --evidence-root "$EVROOT" "$@" </dev/null >"$out" 2>&1
    echo $?
}

# jid <cmds...> : fresh process, --json, print the "id" of the LAST output line
jid() {
    local args=() c
    for c in "$@"; do args+=(--command "$c"); done
    "$OMEGA" --evidence-root "$EVROOT" --json "${args[@]}" </dev/null 2>/dev/null | tail -n 1 |
        sed -n 's/.*"id":"\(sha256:[0-9a-f]\{64\}\)".*/\1/p'
}
# jfield <field> <cmds...> : string field of the LAST output line
jfield() {
    local f=$1; shift
    local args=() c
    for c in "$@"; do args+=(--command "$c"); done
    "$OMEGA" --evidence-root "$EVROOT" --json "${args[@]}" </dev/null 2>/dev/null | tail -n 1 |
        sed -n "s/.*\"$f\":\"\\([^\"]*\\)\".*/\\1/p"
}

# mask host-dependent content of a text transcript (machine block, machine name/id, estimates)
mask() { # mask <in> <out>
    local name mid
    name=$("$OMEGA" --json --command machine </dev/null 2>/dev/null | sed -n 's/.*"result":{"name":"\([^"]*\)".*/\1/p')
    mid=$("$OMEGA" --json --command machine </dev/null 2>/dev/null | sed -n 's/.*"result":{"name":"[^"]*","id":"sha256:\([0-9a-f]*\)".*/\1/p')
    awk -v name="$name" -v mid="$mid" '
        /^\xce\xa9> / { inmach = ($0 == "\xce\xa9> machine"); print; if (inmach) print "<machine block masked>"; next }
        inmach { next }
        {
            if (name != "") gsub(name, "<HOST_MACHINE>")
            if (mid != "") gsub(mid, "<HOST_MACHINE_ID>")
            gsub(/estimated cycles=[0-9]+/, "estimated cycles=<N>")
            gsub(/estimated A=[0-9]+ B=[0-9]+ \((equal|A lower|B lower|differ)[^)]*\)/, "estimated A=<N> B=<N> (<cmp>)")
            print
        }' "$1" >"$2"
}

# ---------------------------------------------------------------- 1. semantic
S=semantic
v7=$(jid '7: u64')
for f in '07: u64' '0x07: u64' '0b111: u64' '(7: u64)' '0x7: u64' '/* c */ 7: u64 // tail'; do
    got=$(jid "$f")
    [ -n "$v7" ] && [ "$got" = "$v7" ]; check $S "literal-spelling[$f]" $? "id=$got"
done
u32=$(jid '7: u32')
[ -n "$u32" ] && [ "$u32" != "$v7" ]; check $S "width-is-identity[u32!=u64]" $?
u8=$(jid '7: u8'); u16=$(jid '7: u16')
[ -n "$u8" ] && [ -n "$u16" ] && [ "$u8" != "$u16" ] && [ "$u8" != "$u32" ] && [ "$u16" != "$v7" ]; check $S "width-is-identity[u8,u16,u32,u64 distinct]" $?
P1='let x: u64 = 7'; P2='let y: u64 = 11'
a1=$(jid "$P1" "$P2" 'x + y')
for f in 'x+y' 'x + /* c */ y' 'x + y // trailing comment' '(x + y)' '  x   +   y  ' '(x) + (y)'; do
    got=$(jid "$P1" "$P2" "$f")
    [ -n "$a1" ] && [ "$got" = "$a1" ]; check $S "whitespace-comments[$f]" $? "id=$got"
done
b1=$(jid "$P1" "$P2" 'y + x')
[ -n "$b1" ] && [ "$b1" != "$a1" ]; check $S "operand-order-is-identity[y+x!=x+y]" $?
la=$(jid 'let a: u64 = 7' 'let b: u64 = 7' 'id a'); lb=$(jid 'let a: u64 = 7' 'let b: u64 = 7' 'id b')
[ -n "$la" ] && [ "$la" = "$lb" ] && [ "$la" = "$v7" ]; check $S "binding-name-not-in-id[let a/let b]" $?
# second let adds no object: graph size of the session is identical with and without it
ga=$("$OMEGA" --command 'let a: u64 = 7' --command 'verify a' </dev/null 2>&1 | sed -n 's/.*(\([0-9]*\) object(s)).*/\1/p' | head -n 1)
gb=$("$OMEGA" --command 'let a: u64 = 7' --command 'let b: u64 = 7' --command 'verify a' </dev/null 2>&1 | sed -n 's/.*(\([0-9]*\) object(s)).*/\1/p' | head -n 1)
[ -n "$ga" ] && [ "$ga" = "$gb" ]; check $S "dedupe[second let adds no object]" $? "objects=$ga/$gb"
for bl in true false; do
    t=$(jfield type "$bl" "type _")
    [ "$t" = "bool" ]; check $S "bool-literal-type[$bl]" $? "type=$t"
done
t=$(jfield type "$P1" "$P2" 'x + y' 'type _'); [ "$t" = "u64" ]; check $S "apply-type[x+y:u64]" $? "type=$t"
t=$(jfield type '(7: u32) + (1: u32)' 'type _'); [ "$t" = "u32" ]; check $S "apply-type[u32+u32:u32]" $? "type=$t"
# values the language reports (evaluation of canonical applies)
val() { jfield value "$@"; }
[ "$(val "$P1" "$P2" 'x + y')" = 18 ]; check $S "value[x+y=18]" $?
[ "$(val 'let m: u64 = 18446744073709551615' 'm + (1: u64)')" = 0 ]; check $S "value[u64 wrap max+1=0]" $?
[ "$(val '(0: u64) - (1: u64)')" = 18446744073709551615 ]; check $S "value[u64 wrap 0-1=max]" $?
[ "$(val '(255: u8) + (1: u8)')" = 0 ]; check $S "value[u8 wrap 255+1=0]" $?
[ "$(val "$P1" '(x + x) * x')" = 98 ]; check $S "value[(x+x)*x=98]" $?
[ "$(val "$P1" 'x & (3: u64)')" = 3 ]; check $S "value[7&3=3]" $?
[ "$(val "$P1" 'x | (8: u64)')" = 15 ]; check $S "value[7|8=15]" $?
[ "$(val "$P1" 'x / (2: u64)')" = 3 ]; check $S "value[7/2=3]" $?
p1=$(jfield program_id 'fn f(x: u64) -> u64 { x * 2 + 1 }' 'id f'); p2=$(jfield program_id 'fn f(n: u64) -> u64 { 1 + 2 * n }' 'id f')
[ -n "$p1" ] && [ "$p1" = "$p2" ]; check $S "program-id[param name + commutative side]" $?
# program identity v2 (spec/program-identity.md): the id binds the canonical body, so
# redefining a name with a different body gives a distinct id and rebinds the name.
p1=$(jfield program_id 'fn t(x: u64) -> u64 { x + 1 }' 'id t'); p2=$(jfield program_id 'fn t(x: u64) -> u64 { x + 1 }' 'clear' 'fn t(x: u64) -> u64 { x + 2 }' 'id t')
[ -n "$p1" ] && [ -n "$p2" ] && [ "$p1" != "$p2" ]; check $S "program-id[body-bound: x+1 != x+2 after clear]" $?
rc=$(om "$WORK/coll.out" --command 'fn f(x: u64) -> u64 { x * 2 + 1 }' --command 'fn f(x: u64) -> u64 { x * 3 + 1 }')
[ "$rc" = 0 ] && ! grep -q 'collision' "$WORK/coll.out"; check $S "program-id-redefine-distinct" $?
# golden transcript: exact text of a pure, host-independent semantic session
rc=$(om "$WORK/semantic.out" --script $Q/semantic.omega-session)
cmp -s "$WORK/semantic.out" $Q/semantic.expected; check $S "golden[semantic.omega-session]" $? "rc=$rc"
[ "$rc" = "$(cat $Q/semantic.exit)" ]; check $S "golden-exit[semantic.omega-session]" $? "rc=$rc"

# ---------------------------------------------------------------- 1b. realize / run / cost
S=realize
# realize -> run agrees with the language value (machine code executed natively)
runv() { jfield result "$@"; }
for e in 'x + y' 'x - y' 'x * y' 'x & y' 'x | y' 'y - x' '(x + x) * x'; do
    v=$(val "$P1" "$P2" "$e"); r=$(runv "$P1" "$P2" "$e" 'realize _' 'run _')
    [ -n "$v" ] && [ "$v" = "$r" ]; check $S "realize-run-agrees[$e]" $? "value=$v run=$r"
done
r=$(runv 'let m: u64 = 18446744073709551615' 'm + (1: u64)' 'realize _' 'run _'); [ "$r" = 0 ]; check $S "realize-run-wrap[max+1=0]" $? "run=$r"
FN='fn f(x: u64) -> u64 requires true ensures result >= 1 { x * 2 + 1 }'
r=$(runv "$FN" 'realize f' 'run _ 5'); [ "$r" = 11 ]; check $S "program-run[f(5)=11]" $? "run=$r"
r=$(runv "$FN" 'run f 0'); [ "$r" = 1 ]; check $S "program-run[f(0)=1]" $? "run=$r"
out=$("$OMEGA" --json --command "$P1" --command "$P2" --command 'x + y' --command 'realize _' </dev/null 2>&1 | tail -n 1)
echo "$out" | grep -q '"disasm":\["8b010000  add x0, x0, x1","d65f03c0  ret"\]'; check $S "realize-code[x+y = add;ret]" $?
echo "$out" | grep -q '"compatible":true,"runnable":true'; check $S "realize-compatible-runnable" $?
echo "$out" | grep -q '"measured":{"present":false' && echo "$out" | grep -q '"qualified":{"present":false'; check $S "cost-measured-qualified-absent" $?
echo "$out" | grep -q '"predicted":{"present":true' && echo "$out" | grep -q '"estimated":{"present":true'; check $S "cost-predicted-estimated-present" $?
out=$("$OMEGA" --json --command "$P1" --command "$P2" --command 'x + y' --command 'cost _' </dev/null 2>&1 | tail -n 1)
echo "$out" | grep -q '"status":"error"'; check $S "cost-before-realize-refused" $?
# arity: running with the wrong number of arguments must be refused, not silently defaulted or ignored
arity() { # arity <case> <cmds...> : the LAST command must be an error
    local name=$1; shift
    local args=() c last
    for c in "$@"; do args+=(--command "$c"); done
    last=$("$OMEGA" --json "${args[@]}" </dev/null 2>&1 | tail -n 1)
    echo "$last" | grep -q '"status":"error"'
    check $S "$name" $? "$(echo "$last" | cut -c1-120)"
}
arity "run-arity[unary fn, 0 args refused]" "$FN" 'run f'
arity "run-arity[unary fn, 2 args refused]" "$FN" 'run f 5 6'
arity "run-arity[binary apply realization, 1 arg refused]" "$P1" "$P2" 'x + y' 'realize _' 'run _ 1'
arity "run-arity[binary apply realization, 3 args refused]" "$P1" "$P2" 'x + y' 'realize _' 'run _ 1 2 3'
arity "realize-u8-refused" '(1: u8) + (2: u8)' 'realize _'
arity "realize-div-refused" "$P1" 'x / (2: u64)' 'realize _'
out=$("$OMEGA" --command "$FN" --command 'alternatives f' </dev/null 2>&1)
echo "$out" | grep -q '^direct@'; check $S "alternatives-lists-direct" $?

# ---------------------------------------------------------------- 2. determinism
S=determinism
DET_RUNS=3; DET_OK=yes
for i in 1 2 3; do
    om "$WORK/det-text-$i.out" --script $Q/usability.omega-session >/dev/null
    om "$WORK/det-json-$i.out" --json --script $Q/usability.omega-session >/dev/null
    om "$WORK/det-sem-$i.out" --json --script $Q/semantic.omega-session >/dev/null
done
for k in text json sem; do
    cmp -s "$WORK/det-$k-1.out" "$WORK/det-$k-2.out" && cmp -s "$WORK/det-$k-1.out" "$WORK/det-$k-3.out"
    r=$?; [ $r -eq 0 ] || DET_OK=no
    check $S "byte-identical-x3[$k]" $r
done
r1=$(jfield realization_id "$P1" "$P2" 'x + y' 'realize _'); r2=$(jfield realization_id "$P1" "$P2" 'x + y' 'realize _')
r3=$(jfield realization_id "$P1" "$P2" 'x + y' 'realize _')
[ -n "$r1" ] && [ "$r1" = "$r2" ] && [ "$r1" = "$r3" ]; r=$?; [ $r -eq 0 ] || DET_OK=no
check $S "realization-id-identical-x3-fresh-processes" $r "id=$r1"
i1=$(jid "$P1" "$P2" 'x * y'); i2=$(jid "$P1" "$P2" 'x * y')
[ -n "$i1" ] && [ "$i1" = "$i2" ]; r=$?; [ $r -eq 0 ] || DET_OK=no
check $S "apply-id-identical-fresh-processes" $r
# the ids the golden file pins must equal the ids fresh processes compute
grep -q "${v7#sha256:}" $Q/semantic.expected; check $S "golden-pins-fresh-id[7:u64]" $?

# ---------------------------------------------------------------- 3. hostile
S=hostile
HD="$WORK/hostile"; mkdir -p "$HD"
# hcase <name> <expect: E=error|N=no-op|A=any non-crash> <script-file>
# The script's LAST line is always `help quit` (needs no table space); the process must still answer it.
hcase() {
    local name=$1 exp=$2 f=$3 rc out
    out="$HD/$name.out"
    rc=$(om "$out" --script "$f")
    H_RUN=$((H_RUN + 1))
    local alive=1 crashed=0
    tail -n 1 "$out" | grep -q '^  quit  ' || alive=0
    case "$rc" in 0|1|2) ;; *) crashed=1 ;; esac
    if [ $crashed -eq 1 ]; then
        H_CRASH=$((H_CRASH + 1)); fail $S "$name" "CRASH rc=$rc"; return
    fi
    case "$exp" in
        E) if [ "$rc" = 1 ] && [ $alive -eq 1 ] && grep -q '^error: ' "$out"; then
               H_CLOSED=$((H_CLOSED + 1)); pass $S "$name" "rc=1 failed closed: $(grep -m1 '^error: ' "$out" | cut -c1-110)"
           else fail $S "$name" "rc=$rc alive=$alive expected error+rc1"; fi ;;
        N) if [ "$rc" = 0 ] && [ $alive -eq 1 ]; then H_NOOP=$((H_NOOP + 1)); pass $S "$name" "rc=0 no-op (spec: blank/comment line)"
           else fail $S "$name" "rc=$rc alive=$alive expected no-op"; fi ;;
        A) if [ $alive -eq 1 ]; then pass $S "$name" "rc=$rc alive"; else fail $S "$name" "rc=$rc not alive"; fi ;;
    esac
}
# mk <name> <printf-format> [args] : write a hostile script: prelude + hostile line + sentinel
PRE='let x: u64 = 7\nlet y: u64 = 11\nx + y\n'
mk() { local n=$1 fmt=$2; shift 2; printf "$PRE$fmt\\nhelp quit\\n" "$@" >"$HD/$n.s"; echo "$HD/$n.s"; }
rep() { local i out=""; for ((i = 0; i < $2; i++)); do out="$out$1"; done; printf '%s' "$out"; }
Z64=$(rep 0 64); H63=$(rep a 63); H65=$(rep b 65)
TY=037a444b0f120fd4bbabd0c16ae143599b231d2469827fccbf9bc79c57bab822
hcase empty-line          N "$(mk empty-line '')"
hcase only-spaces         N "$(mk only-spaces '      \t   ')"
hcase comment-only        N "$(mk comment-only '// just a comment')"
hcase block-comment-only  N "$(mk block-comment-only '/* only a block comment */')"
hcase token-300           E "$(mk token-300 'let %s: u64 = 1' "$(rep a 300)")"
hcase line-5000           E "$(mk line-5000 '%s' "$(rep z 5000)")"
hcase line-5000-valid-prefix E "$(mk line-5000-valid-prefix 'let v: u64 = 1 %s' "$(rep ' ' 4990)")"
hcase invalid-utf8-name   E "$(mk invalid-utf8-name 'let \377\376: u64 = 1')"
hcase invalid-utf8-cmd    E "$(mk invalid-utf8-cmd 'inspect \303\050')"
hcase invalid-utf8-bare   E "$(mk invalid-utf8-bare '\200\201\202')"
hcase nul-in-name         E "$(mk nul-in-name 'let a\000b: u64 = 1')"
hcase nul-after-valid     E "$(mk nul-after-valid 'let n: u64 = 5\000 junk that must not be ignored')"
hcase nul-only            A "$(mk nul-only '\000')"
hcase id-63-hex           E "$(mk id-63-hex 'id %s' "$H63")"
hcase id-65-hex           E "$(mk id-65-hex 'id %s' "$H65")"
hcase id-non-hex          E "$(mk id-non-hex 'id %sg' "$(rep 0 63)")"
hcase id-sha-junk         E "$(mk id-sha-junk 'inspect sha256:not-a-hash')"
hcase id-sha-empty        E "$(mk id-sha-empty 'inspect sha256:')"
hcase id-sha-64-plus-junk E "$(mk id-sha-64-plus-junk 'inspect sha256:%szz' "$Z64")"
hcase id-uppercase-prefix A "$(mk id-uppercase-prefix 'inspect SHA256:%s' "$Z64")"
hcase unknown-command     E "$(mk unknown-command 'frobnicate the graph')"
hcase unknown-command-2   E "$(mk unknown-command-2 'delete x')"
hcase unterminated-paren  E "$(mk unterminated-paren '(x + y')"
hcase unbalanced-close    E "$(mk unbalanced-close 'x + y)')"
hcase overflow-2p64       E "$(mk overflow-2p64 '18446744073709551616: u64')"
hcase overflow-2p65       E "$(mk overflow-2p65 '36893488147419103232: u64')"
hcase overflow-hex-65bit  E "$(mk overflow-hex-65bit '0x1ffffffffffffffff: u64')"
hcase overflow-u8-256     E "$(mk overflow-u8-256 '256: u8')"
hcase overflow-u16        E "$(mk overflow-u16 'let w: u16 = 65536')"
hcase overflow-u32        E "$(mk overflow-u32 '4294967296: u32')"
hcase nest-40             E "$(mk nest-40 '%s7: u64%s' "$(rep '(' 40)" "$(rep ')' 40)")"
hcase nest-100            E "$(mk nest-100 '%s7: u64%s' "$(rep '(' 100)" "$(rep ')' 100)")"
hcase nest-1000-open      E "$(mk nest-1000-open '%s' "$(rep '(' 1000)")"
hcase chain-200-adds      E "$(mk chain-200-adds 'x%s' "$(rep ' + x' 200)")"
hcase chain-2000-adds     E "$(mk chain-2000-adds 'x%s' "$(rep '+x' 2000)")"
hcase inspect-absent-id   E "$(mk inspect-absent-id 'inspect %s' "$Z64")"
hcase graph-absent-id     E "$(mk graph-absent-id 'graph sha256:%s' "$Z64")"
hcase verify-absent-id    E "$(mk verify-absent-id 'verify %s' "$Z64")"
hcase run-too-many-args   E "$(mk run-too-many-args 'realize _\nrun _ 1 2 3 4 5 6 7 8')"
hcase run-no-args         E "$(mk run-no-args 'run')"
hcase compare-one-arg     E "$(mk compare-one-arg 'compare x')"
hcase compare-three-args  E "$(mk compare-three-args 'compare x y x')"
hcase realize-bool        E "$(mk realize-bool 'let b: bool = true\nrealize b')"
hcase realize-no-arg      E "$(mk realize-no-arg 'realize')"
hcase realize-bad-machine E "$(mk realize-bad-machine 'realize _ for other.machine')"
hcase run-a-type          E "$(mk run-a-type 'run sha256:%s' "$TY")"
hcase run-a-value         E "$(mk run-a-value 'run x')"
hcase run-u8-apply        E "$(mk run-u8-apply '(1: u8) + (2: u8)\nrealize _')"
hcase mixed-width         E "$(mk mixed-width '(1: u32) + (1: u64)')"
hcase bool-arith          E "$(mk bool-arith 'true + false')"
hcase ambiguous-width     E "$(mk ambiguous-width '1 + 2')"
hcase div-const-zero      E "$(mk div-const-zero 'x / (0: u64)')"
hcase comparison-op       E "$(mk comparison-op 'x == y')"
hcase signed-type         E "$(mk signed-type 'let s: i64 = 1')"
hcase fn-body-not-chain   E "$(mk fn-body-not-chain 'fn g(x: u64) -> u64 { x * x }')"
hcase fn-recursion        E "$(mk fn-recursion 'fn g(x: u64) -> u64 { g + 1 }')"
hcase fn-wide-const       E "$(mk fn-wide-const 'fn g(x: u64) -> u64 { x + 4294967296 }')"
hcase name-64-chars       E "$(mk name-64-chars 'let %s: u64 = 1' "$(rep n 64)")"
hcase help-unknown        E "$(mk help-unknown 'help nosuchcommand')"
hcase crlf-line           A "$(mk crlf-line 'let c: u64 = 1\r')"
hcase tab-separated       A "$(mk tab-separated 'let\tt:\tu64\t=\t1')"
# capacity: 300 distinct lets (binding table is 64) -> must fail closed, not crash
{ for ((i = 0; i < 300; i++)); do echo "let v$i: u64 = $i"; done; echo 'help quit'; } >"$HD/binding-table-300.s"
hcase binding-table-300   E "$HD/binding-table-300.s"
# capacity: graph (256 objects) exhaustion
{ echo 'let x: u64 = 7'; for ((i = 1; i < 400; i++)); do echo "x + ($i: u64)"; done; echo 'help quit'; } >"$HD/graph-full-400.s"
hcase graph-full-400      E "$HD/graph-full-400.s"
# capacity: 20 distinct programs (table is 16)
{ for ((i = 1; i <= 20; i++)); do echo "fn p$i(x: u64) -> u64 { x + $i }"; done; echo 'help quit'; } >"$HD/program-table-20.s"
hcase program-table-20    E "$HD/program-table-20.s"
# capacity: 40 realizations
{ echo 'let x: u64 = 7'; for ((i = 1; i <= 40; i++)); do echo "fn r$i(x: u64) -> u64 { x * $i }"; echo "realize r$i"; done; echo 'help quit'; } >"$HD/realization-table-40.s"
hcase realization-table-40 A "$HD/realization-table-40.s"
# --command mode hostile arguments (argv cannot carry NUL)
rc=$(om "$HD/cmd-huge.out" --command "$(rep q 5000)" --command 'let alive: u64 = 42'); H_RUN=$((H_RUN + 1))
if [ "$rc" = 1 ] && tail -n 1 "$HD/cmd-huge.out" | grep -qx 42; then H_CLOSED=$((H_CLOSED + 1)); pass $S cmd-5000-byte-arg "rc=1"; else
    case "$rc" in 0|1|2) ;; *) H_CRASH=$((H_CRASH + 1)) ;; esac; fail $S cmd-5000-byte-arg "rc=$rc"; fi
rc=$(om "$HD/cmd-bad-flag.out" --no-such-flag); H_RUN=$((H_RUN + 1))
if [ "$rc" = 2 ]; then H_CLOSED=$((H_CLOSED + 1)); pass $S bad-flag "rc=2 usage"; else
    case "$rc" in 0|1|2) ;; *) H_CRASH=$((H_CRASH + 1)) ;; esac; fail $S bad-flag "rc=$rc"; fi
rc=$(om "$HD/cmd-missing-arg.out" --command); H_RUN=$((H_RUN + 1))
if [ "$rc" = 2 ]; then H_CLOSED=$((H_CLOSED + 1)); pass $S flag-missing-value "rc=2"; else
    case "$rc" in 0|1|2) ;; *) H_CRASH=$((H_CRASH + 1)) ;; esac; fail $S flag-missing-value "rc=$rc"; fi
rc=$(om "$HD/cmd-script-dir.out" --script /); H_RUN=$((H_RUN + 1))
if [ "$rc" = 1 ] || [ "$rc" = 2 ]; then H_CLOSED=$((H_CLOSED + 1)); pass $S script-is-directory "rc=$rc"; else
    case "$rc" in 0) ;; *) H_CRASH=$((H_CRASH + 1)) ;; esac; fail $S script-is-directory "rc=$rc (a directory given as a script must not succeed)"; fi
# golden: the full hostile transcript text (error wording is part of the contract)
rc=$(om "$WORK/hostile.out" --script $Q/hostile.omega-session)
cmp -s "$WORK/hostile.out" $Q/hostile.expected; check $S "golden[hostile.omega-session]" $? "rc=$rc"
[ "$rc" = "$(cat $Q/hostile.exit)" ]; check $S "golden-exit[hostile.omega-session]" $? "rc=$rc"

# ---------------------------------------------------------------- 4. authority (console side)
S=authority
for w in 'authorize x' 'authorize' 'submit' 'submit _' 'execute _' 'execute' 'mint cap' 'mint' 'grant x' 'revoke x' 'publish x' 'promote _' 'capability' 'effect x' 'syscall' 'admin'; do
    rc=$(om "$WORK/auth.out" --command 'let x: u64 = 7' --command "$w" --command 'let alive: u64 = 42')
    { [ "$rc" = 1 ] && grep -q '^error: ' "$WORK/auth.out" && tail -n 1 "$WORK/auth.out" | grep -qx 42; }
    check $S "escalation-word-refused[$w]" $? "$(grep -m1 '^error: ' "$WORK/auth.out" | cut -c1-100)"
done
out=$("$OMEGA" --json --command "$P1" --command "$P2" --command 'x + y' --command 'verify _' </dev/null 2>&1 | tail -n 1)
echo "$out" | grep -q '"name":"AUTHORITY","status":"PASS"[^}]*"detail":"NONE:' &&
    echo "$out" | grep -q '"authority":{"required":false,"effects":0,"capability_refs":0,"granted":false}'
check $S "verify-authority-NONE[pure apply]" $?
out=$("$OMEGA" --json --command 'fn f(x: u64) -> u64 { x * 2 + 1 }' --command 'verify f' </dev/null 2>&1 | tail -n 1)
echo "$out" | grep -q '"granted":false'; check $S "verify-authority-not-granted[program]" $?
out=$("$OMEGA" --command 'effects' </dev/null 2>&1)
[ "$out" = "$(printf '[effect-request]\nno effect objects in session')" ]; check $S "effects-empty-session" $?
rc=$(om "$WORK/eff.out" --command "$P1" --command 'effects x'); grep -q "is not an EFFECT object" "$WORK/eff.out" && [ "$rc" = 1 ]
check $S "effects-on-value-refused" $?
out=$("$OMEGA" --json --command "$P1" --command "$P2" --command 'x + y' --command 'realize _' --command 'run _' </dev/null 2>&1 | tail -n 1)
echo "$out" | grep -q '"class":"pure-execution"' && ! echo "$out" | grep -qi 'authorized":true'
check $S "run-pure-is-pure-execution-class" $?
out=$("$OMEGA" --command 'world' </dev/null 2>&1)
echo "$out" | grep -q 'unattached' && echo "$out" | grep -q 'holds no authority'; check $S "world-unattached-no-authority" $?
# no command anywhere in help is an authority/grant verb
"$OMEGA" --command help </dev/null 2>&1 | grep -Eiq '^  (authorize|grant|mint|submit|execute|promote|publish|revoke)\b'
[ $? -ne 0 ]; check $S "help-lists-no-grant-verb" $?

# ---------------------------------------------------------------- 5. scriptability
S=scriptability
rc=$(om "$WORK/s1.out" --command 'let a: u64 = 1'); [ "$rc" = 0 ] && [ "$(cat "$WORK/s1.out")" = 1 ]; check $S "single-command-rc0" $?
rc=$(om "$WORK/s2.out" --command 'let a: u64 = 2' --command 'let b: u64 = 3' --command 'a * b'); [ "$rc" = 0 ] && [ "$(tail -n 1 "$WORK/s2.out")" = 6 ]
check $S "repeated-command-share-session" $?
printf 'let a: u64 = 4\nlet b: u64 = 5\na + b\n' >"$WORK/s3.omega-session"
rc=$(om "$WORK/s3.out" --script "$WORK/s3.omega-session"); [ "$rc" = 0 ] && [ "$(tail -n 1 "$WORK/s3.out")" = 9 ]; check $S "script-file-rc0" $?
"$OMEGA" --evidence-root "$EVROOT" --script - <"$WORK/s3.omega-session" >"$WORK/s4.out" 2>&1; rc=$?
[ "$rc" = 0 ] && cmp -s "$WORK/s3.out" "$WORK/s4.out"; check $S "script-stdin-equals-file" $?
"$OMEGA" --evidence-root "$EVROOT" <"$WORK/s3.omega-session" >"$WORK/s5.out" 2>&1; rc=$?
[ "$rc" = 0 ] && ! grep -q $'\xce\xa9>' "$WORK/s5.out" && [ "$(tail -n 1 "$WORK/s5.out")" = 9 ]; check $S "no-prompt-when-stdin-not-tty" $?
rc=$(om "$WORK/s6.out" --command 'let a: u64 = 1' --command 'nosuch'); [ "$rc" = 1 ]; check $S "any-error-rc1" $? "rc=$rc"
rc=$(om "$WORK/s7.out" --script "$WORK/does-not-exist.omega-session"); [ "$rc" = 2 ]; check $S "missing-script-rc2" $? "rc=$rc"
rc=$(om "$WORK/s8.out" --bogus); [ "$rc" = 2 ] && grep -q '^usage: ' "$WORK/s8.out"; check $S "bad-flag-rc2-usage" $? "rc=$rc"
rc=$(om "$WORK/s9.out" --command 'let a: u64 = 1' --command quit --command 'let b: u64 = 2'); [ "$rc" = 0 ] && ! grep -qx 2 "$WORK/s9.out"
check $S "quit-stops-processing" $?
# --json: exactly one parseable object per non-blank command, with the fixed envelope
"$OMEGA" --evidence-root "$EVROOT" --json --script $Q/usability.omega-session </dev/null >"$WORK/j.out" 2>"$WORK/j.err"; rc=$?
ncmd=$(grep -cv -e '^[[:space:]]*$' $Q/usability.omega-session)   # comment lines answer too; blank lines do not
nl=$(wc -l <"$WORK/j.out")
[ "$nl" = "$ncmd" ]; check $S "json-one-line-per-command" $? "lines=$nl commands=$ncmd"
python3 -c '
import json, sys
ok = True
for n, line in enumerate(open(sys.argv[1], encoding="utf-8"), 1):
    try:
        o = json.loads(line)
    except Exception as e:
        print("line", n, "not JSON:", e); ok = False; continue
    if set(o) != {"command", "status", "class", "result", "error"}:
        print("line", n, "envelope keys", sorted(o)); ok = False
    if o["status"] not in ("ok", "error"):
        print("line", n, "status", o["status"]); ok = False
    if (o["status"] == "error") != (o["error"] is not None):
        print("line", n, "status/error mismatch"); ok = False
sys.exit(0 if ok else 1)' "$WORK/j.out"
check $S "json-every-line-parses-with-envelope" $?
"$OMEGA" --evidence-root "$EVROOT" --json --script $Q/hostile.omega-session </dev/null >"$WORK/jh.out" 2>/dev/null
python3 -c '
import json, sys
bad = 0
for line in open(sys.argv[1], "rb"):
    try: json.loads(line.decode("utf-8"))
    except Exception: bad += 1
sys.exit(1 if bad else 0)' "$WORK/jh.out"
check $S "json-hostile-lines-still-valid-json" $?
[ ! -s "$WORK/j.err" ]; check $S "json-mode-no-stderr-noise" $?
# stderr/stdout: errors in text mode go to stdout together with the transcript (documented behaviour check)
"$OMEGA" --command nosuch </dev/null >"$WORK/so.out" 2>"$WORK/se.out"
{ grep -q '^error:' "$WORK/so.out" || grep -q '^error:' "$WORK/se.out"; }; check $S "error-text-emitted" $?

# ---------------------------------------------------------------- 6. usability (help-only discovery trace)
S=usability
rc=$(om "$WORK/usability.raw" --script $Q/usability.omega-session)
mask "$WORK/usability.raw" "$WORK/usability.out"
cmp -s "$WORK/usability.out" $Q/usability.expected; r=$?
[ $r -ne 0 ] && diff $Q/usability.expected "$WORK/usability.out" | head -n 20
check $S "golden[usability.omega-session masked]" $r "rc=$rc"
[ "$rc" = "$(cat $Q/usability.exit)" ]; check $S "golden-exit[usability.omega-session]" $? "rc=$rc"
# discovery questions answered by the trace (structure, host-independent)
q() { grep -q -- "$2" "$WORK/usability.raw"; check $S "$1" $?; }
q "what-is-it[inspect shows kind]" '^kind: OPERATION'
q "why-valid[verify verdict]" '^VERDICT         PASS'
q "where-runs[machine targets]" '^  target aarch64-v8a '
q "where-runs[realize compatible]" '^  compatible   yes'
q "what-costs[four classes]" '^cost (four separate classes; never merged)'
q "what-costs[measured absent, not invented]" '^  measured  ABSENT'
q "evidence[scope reported]" '^  scope      HOST'
q "run-result[18]" '^18$'
# machine view structure via JSON (host-dependent values not asserted)
out=$("$OMEGA" --json --command machine </dev/null 2>&1)
for k in '"name":' '"provenance":"assumed' '"observed":false' '"pipeline":' '"registers":' '"caches":' '"targets":' '"physics_seal_is_placeholder":true'; do
    echo "$out" | grep -q -- "$k"; check $S "machine-json-has[$k]" $?
done

# ---------------------------------------------------------------- sanitizer reports (meaningful when OMEGA is an ASan/UBSan build)
SAN=$(grep -rl -e 'ERROR: AddressSanitizer' -e 'runtime error:' -e 'ERROR: LeakSanitizer' "$WORK" 2>/dev/null | wc -l)
[ "$SAN" = 0 ]; check hostile "no-sanitizer-reports-in-any-transcript" $? "files_with_reports=$SAN"

# ---------------------------------------------------------------- summary
TOTAL_FAIL=0
for s in $SECTIONS; do
    echo "QUAL_SECTION $s run=${RUN[$s]} failed=${FAILED[$s]}"
    TOTAL_FAIL=$((TOTAL_FAIL + ${FAILED[$s]}))
done
echo "QUAL_HOSTILE run=$H_RUN failed_closed=$H_CLOSED crashed=$H_CRASH noop=$H_NOOP"
echo "QUAL_DETERMINISM runs=$DET_RUNS identical=$DET_OK"
echo "QUAL_BINARY $(sha256sum "$OMEGA" | cut -d' ' -f1) $OMEGA"
if [ $TOTAL_FAIL -eq 0 ]; then echo "QUAL_RESULT PASS"; exit 0; fi
echo "QUAL_RESULT FAIL failed=$TOTAL_FAIL"
exit 1
