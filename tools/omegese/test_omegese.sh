#!/usr/bin/env bash
# tools/omegese/test_omegese.sh <omegese> <omega_emit> <checker2> -- Omegese checks (docs/one_layer/OMEGESE.md).
#  1. every public dev program (tests/one_layer/omegese/dev) -> omegese emit -> omega_emit emit -> checker2
#     ACCEPT;
#  1b. coverage programs and omegese next prefixes (including prefixes that must be refused);
#  2. refusals: each bad program is refused by omegese with its expected reason, and a control (the
#     same program with the defect fixed) is accepted;
#  3. loop guard negative control: the dev04 op list with the hidden loop counter increment removed
#     still passes omega_emit, and checker2 must REJECT it as C2-LOOP-UNBOUNDED;
#  4. tokenizer gate (only when OMEGESE_TOKCOUNT and OMEGESE_TOKENIZER are set): every fixed word of
#     the language, with its leading space, is exactly one token.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ $# -eq 3 ] || { echo "usage: test_omegese.sh <omegese> <omega_emit> <checker2>" >&2; exit 2; }
G="$1" OME="$2" C2="$3"
T="$(mktemp -d "${TMPDIR:-/tmp}/omegese-test.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# 1. dev programs
n=0
for f in "$ROOT"/tests/one_layer/omegese/dev/dev*.omegese; do
  id="$(basename "$f" .omegese)"
  if ! "$G" emit "$f" "$T/$id.json" > "$T/$id.out"; then bad "$id omegese: $(cat "$T/$id.out")"; continue; fi
  if ! "$OME" emit "$T/$id.json" "$T/$id.oscir" > "$T/$id.out"; then bad "$id omega_emit: $(cat "$T/$id.out")"; continue; fi
  v="$("$C2" check "$T/$id.oscir" 2>&1 | head -1 || true)"
  case "$v" in checker2=ACCEPT*) n=$((n + 1)) ;; *) bad "$id checker2: $v" ;; esac
done
[ "$n" -eq 10 ] || bad "dev programs accepted $n/10"
echo "dev: $n/10 omegese -> omega_emit -> checker2 ACCEPT"

# 1a. nested values: tests/one_layer/omegese/nested (dev tasks rewritten with nested values and
#     brackets) and tests/one_layer/omegese/trial2 (the model's own answers from phase-2 trial 2,
#     verbatim, that v0 refused for nesting or brackets): omegese -> omega_emit -> checker2 ACCEPT.
#     trial2/dev07 has a real extra value and must stay refused, at its line, for that reason.
accept() { # <file> <id>: omegese -> omega_emit -> checker2 ACCEPT
  if ! "$G" emit "$1" "$T/$2.json" > "$T/$2.out"; then bad "$2 omegese: $(cat "$T/$2.out")"; return 1; fi
  if ! "$OME" emit "$T/$2.json" "$T/$2.oscir" > "$T/$2.out"; then bad "$2 omega_emit: $(cat "$T/$2.out")"; return 1; fi
  local v; v="$("$C2" check "$T/$2.oscir" 2>&1 | head -1 || true)"
  case "$v" in checker2=ACCEPT*) return 0 ;; *) bad "$2 checker2: $v"; return 1 ;; esac
}
nn=0 nt=0
for f in "$ROOT"/tests/one_layer/omegese/nested/*.omegese; do accept "$f" "nested-$(basename "$f" .omegese)" && nn=$((nn + 1)); done
for f in "$ROOT"/tests/one_layer/omegese/trial2/*.omegese; do
  id="$(basename "$f" .omegese)"
  if [ "$id" = dev07 ]; then
    out="$("$G" emit "$f" "$T/x.json" || true)"
    [ "$out" = "REFUSED omegese: line 3: le has 3 values after it; it takes 2" ] || bad "trial2 dev07 (extra value) must be refused: got '$out'"
  else accept "$f" "trial2-$id" && nt=$((nt + 1)); fi
done
[ "$nn" -eq 6 ] && [ "$nt" -eq 5 ] || bad "nested $nn/6, trial2 $nt/5"
echo "nested: $nn/6 rewritten dev programs and $nt/5 trial-2 model answers ACCEPT; trial-2 dev07 refused for its extra value"

# 1b. coverage programs (break, nested loops, else-if, a for with a name as the count, arrays and
#     calls across functions): omegese -> omega_emit -> checker2 ACCEPT
cov=0
awk -v d="$T" '/^== / { k++; f = d "/c" k ".omegese"; next } { print > f }' <<'EOF'
== break and nested loops
fn f n uint to uint
s = uint 0
for i uint n
for j uint 8
if gt j i
break
end
s = add s j
end
end
ret s
end
== else-if chain
fn sign a int to int
if lt a 0
ret -1
else
if gt a 0
ret 1
end
end
ret 0
end
== arrays and calls
fn sum4 v view uint 4 to uint
s = uint 0
for i uint 4
x = get v i
s = add s x
end
ret s
end
fn fill e edit uint 4 to void
for i uint 4
put e i i
end
end
fn f to uint
a = new uint 4 0
m = edit a
fill m
s = sum4 a
free a
ret s
end
== IR type names as written in the task signature
fn clamp_add a u8 b u8 to u8
s = add a b
if lt s a
ret u8 255
end
ret s
end
== calls without values, a bool function as a condition
fn seven to uint
ret 7
end
fn odd a uint to bool
b = and a 1
ret ne b 0
end
fn f a uint to uint
x = seven
if odd a
ret x
end
ret a
end
== nested values in every place a value goes: call args, get index, put index and value, for count, ret, conversions, brackets
fn sq a uint to uint
ret mul a a
end
fn f v view uint 4 n uint to uint
s = uint 0
for i uint sub n 1
s = add s sq get v rem i 4
end
w = new uint 4 0
put w rem add s 2 4 mul s 2
for j uint (4)
s = add s get w j
end
free w
ret uint ulong add (s) 0
end
== a number first takes the type of the other value; shifts; neg and not nested; a call with no values nested
fn seven to int
ret 7
end
fn g a int b ulong to bool
x = sub 100 neg a
y = shl 1 and b 7
z = add seven x
if and not eq z 0 gt y 3
ret true
end
ret ne (rem 10 z) 0
end
== a while condition with nested values; trap ends a path; borrows as call arguments
fn total v view uint 4 to uint
ret add add get v 0 get v 1 add get v 2 get v 3
end
fn h a uint to uint
while lt mul a a 1000 bound 64
a = add a 1
end
if gt a 100
trap 1
else
if eq a 7
trap 2
end
end
b = new uint 4 a
c = total view b
free b
ret c
end
EOF
for f in "$T"/c*.omegese; do
  if "$G" emit "$f" "$T/c.json" > "$T/c.out" && "$OME" emit "$T/c.json" "$T/c.oscir" > "$T/c.out"; then
    v="$("$C2" check "$T/c.oscir" 2>&1 | head -1 || true)"
    case "$v" in checker2=ACCEPT*) cov=$((cov + 1)) ;; *) bad "coverage $(basename "$f") checker2: $v" ;; esac
  else bad "coverage $(basename "$f"): $(cat "$T/c.out")"; fi
done
echo "coverage: $cov/$(ls "$T"/c*.omegese | wc -l) accepted"

# 1c. omegese next: the allowed words after a prefix. A value slot lists "names:" (names of a type
#     that fits), "number:" (when a bare number may stand there, and its type) and "words:" (true
#     false, ops, types, own/edit/view, functions that can give a fitting value, and "(").
nx() { printf '%b' "$1" > "$T/p"; "$G" next "$T/p" || true; }
nxk=0
nxeq() { # <prefix> <expected output>
  nxk=$((nxk + 1))
  local got; got="$(nx "$1")"
  [ "$got" = "$(printf '%b' "$2")" ] || bad "next after '$1': got '$got'"
}
nxcase() { # <prefix> <glob the output must match> <what>
  nxk=$((nxk + 1))
  local got; got="$(nx "$1")"
  # shellcheck disable=SC2254
  case "$got" in $2) ;; *) bad "next $3: got '$got'" ;; esac
}
nxeq 'fn f a uint to uint\nret a\n' 'words: end'
nxeq 'fn f a uint b ulong to uint\nx = add a ' 'names: a\nnumber: uint\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxcase 'fn f a uint to uint\nfor i uint 4\n' 'words: if while for ret put free trap break end*' 'in a loop'
nxcase 'fn f a uint to uint\nret a\nret ' 'REFUSED omegese: *' 'after a dead line: not refused'
nxcase 'x = add ' 'REFUSED omegese: *' 'before fn: not refused'
nxeq 'fn odd a uint to bool\nret true\nend\nfn f a uint fl view bool 4 to uint\nif ' 'names:\nwords: true false and or xor eq ne lt le gt ge not get bool odd ('
# nesting: a stack of open operand slots, each with the type it needs
nxeq 'fn f r uint q ulong c bool to uint\nx = add mul r ' 'names: r\nnumber: uint\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f r uint q ulong c bool to uint\nx = add mul r r ' 'names: r\nnumber: uint\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f r uint q ulong c bool to uint\nret sub ' 'names: r\nnumber: any (it takes the type of the other value)\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f p short to short\nif lt p short ' 'names: p\nnumber: short\nwords: add sub mul div rem and or xor shl shr neg not uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64 ('
# the type is still open: every whole-number name (bool and arrays excluded for add); after a bare
# number, no second bare number, and only names the number fits
nxeq 'fn f r uint q ulong c bool to uint\nx = add ' 'names: r q\nnumber: any (it takes the type of the other value)\nwords: add sub mul div rem and or xor shl shr neg not uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64 ('
nxeq 'fn f r uint q ulong c bool to uint\nx = add 1 ' 'names: r q\nwords: add sub mul div rem and or xor shl shr neg not uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64 ('
nxeq 'fn f a uchar b uint to uint\nx = add 300 ' 'names: b\nwords: add sub mul div rem and or xor shl shr neg not ushort uint ulong short int long u16 u32 u64 i16 i32 i64 ('
# brackets, the end of a value, arrays, trap
nxeq 'fn f r uint q ulong c bool to uint\nx = (add r r ' 'words: )'
nxeq 'fn f r uint x uint to uint\nwhile le add mul r r 4 r ' 'words: bound (newline)'
nxeq 'fn f r uint x uint to uint\nret add r (r) ' 'words: (newline)'
nxeq 'fn f v view uint 4 e edit uint 4 to uint\nput ' 'names: e'
nxeq 'fn g v view uint 4 to uint\nret 1\nend\nfn f a own uint 4 to uint\nx = g ' 'names: a\nwords: own edit view ('
nxeq 'fn f p short c bool to short\nif c\ntrap 1\n' 'words: else end'
nxcase 'fn f r uint to uint\nx = add r r r ' 'REFUSED omegese: line 2: add has 3 values after it; it takes 2' 'after an extra value: not refused'
nxcase 'fn f r uint to uint\nx = add 1 2 ' 'REFUSED omegese: *two numbers*' 'after two numbers: not refused'
nxcase 'fn f r uint to uint\nx = add r ) ' 'REFUSED omegese: *no matching*' 'after a stray ): not refused'
# never offer a closing word that emit would refuse
nxeq 'fn f a uint to uint\nb = add a 1\n' 'words: if while for ret put free trap\nassign: a b\nnew-name: any name not in use'
nxcase 'fn f a uint to uint\nb = add a 1\nend' 'REFUSED omegese: *without ret*' 'end before ret: not refused'
nxcase 'fn f a uint to uint\nelse' 'REFUSED omegese: line 2: else without an open if' 'stray else: not refused'
nxcase 'fn f a uint to uint\nbreak' 'REFUSED omegese: line 2: break outside a loop' 'stray break: not refused'
echo "next: $nxk prefixes give the expected words"

# 2. refusals: "== <expected reason substring>", the bad program, "-- control", the fixed program
r=0
awk -v d="$T" '
  /^== / { k++; sub(/^== /, ""); print > (d "/r" k ".want"); f = d "/r" k ".bad"; next }
  /^-- control$/ { f = d "/r" k ".ok"; next }
  { print > f }' <<'EOF'
== needs two values of one type
fn f a uint b ulong to uint
x = add a b
ret a
end
-- control
fn f a uint b uint to uint
x = add a b
ret x
end
== is not a name in scope here
fn f a uint to uint
if gt a 1
y = add a 1
a = y
end
ret y
end
-- control
fn f a uint to uint
y = a
if gt a 1
y = add a 1
end
ret y
end
== can reach its end without ret
fn f a uint to uint
if gt a 1
ret a
end
end
-- control
fn f a uint to uint
if gt a 1
ret a
end
ret 0
end
== can never run
fn f a uint to uint
ret a
b = add a 1
end
-- control
fn f a uint to uint
b = add a 1
ret b
end
== does not fit uchar
fn f to uchar
x = uchar 300
ret x
end
-- control
fn f to uchar
x = uchar 255
ret x
end
== two numbers
fn f to uint
x = add 1 2
ret x
end
-- control
fn f to uint
x = uint 1
y = add x 2
ret y
end
== the type of this number is unknown
fn f to uint
x = 5
ret x
end
-- control
fn f to uint
x = uint 5
ret x
end
== a condition must be a bool
fn f a uint to uint
if a
ret 1
end
ret 0
end
-- control
fn f a uint to uint
if ne a 0
ret 1
end
ret 0
end
== must be 1..1000000
fn f a uint to uint
while ne a 0 bound 2000000
a = shr a 1
end
ret a
end
-- control
fn f a uint to uint
while ne a 0 bound 64
a = shr a 1
end
ret a
end
== else without an open if
fn f a uint to uint
else
ret a
end
-- control
fn f a uint to uint
ret a
end
== is the loop counter
fn f to uint
s = uint 0
for i uint 4
i = add i 1
end
ret s
end
-- control
fn f to uint
s = uint 0
for i uint 4
s = add s i
end
ret s
end
== is not part of Omegese
fn f a uint b uint to uint
x = add a, b
ret x
end
-- control
fn f a uint b uint to uint
x = add a b
ret x
end
== put needs an own or edit array
fn f to uint
a = new uint 4 0
v = view a
put v 0 1
free a
ret 0
end
-- control
fn f to uint
a = new uint 4 0
put a 0 1
x = get a 0
free a
ret x
end
== nor an op, a type or a function
fn f a uint to uint
x = g a
ret x
end
-- control
fn g a uint to uint
ret a
end
fn f a uint to uint
x = g a
ret x
end
== neg needs a signed number
fn f a uint to uint
ret neg a
end
-- control
fn f a int to int
ret neg a
end
== g takes uint there
fn g a uint to uint
ret a
end
fn f b ulong to uint
x = g b
ret x
end
-- control
fn g a uint to uint
ret a
end
fn f b ulong to uint
c = uint b
x = g c
ret x
end
== cannot call itself
fn f a uint to uint
x = f a
ret x
end
-- control
fn f a uint to uint
ret a
end
== is named twice
fn f a uint a ulong to ulong
ret a
end
-- control
fn f a uint b ulong to ulong
ret b
end
== is written but never read
fn f a uint to uint
x = add a 1
rte x
ret x
end
-- control
fn f a uint to uint
x = add a 1
ret x
end
== is already a name here
fn f to uint
i = uint 0
for i uint 10
end
ret i
end
-- control
fn f to uint
i = uint 0
for k uint 10
i = add i k
end
ret i
end
== is a word of the language, not a name
fn f bound uint to uint
ret bound
end
-- control
fn f n uint to uint
ret n
end
== ( ... ) must hold exactly one value; 'b' is extra
fn f a uint b uint to uint
x = add (a b) a
ret x
end
-- control
fn f a uint b uint to uint
x = add (a) b
ret x
end
== ')' has no matching '('
fn f a uint b uint to uint
x = add a b)
ret x
end
-- control
fn f a uint b uint to uint
x = (add a b)
ret x
end
== '(' has no matching ')'
fn f a uint b uint to uint
x = (add a b
ret x
end
-- control
fn f a uint b uint to uint
x = (add a (b))
ret x
end
== ( ) holds no value
fn f a uint b uint to uint
x = add () a
ret x
end
-- control
fn f a uint b uint to uint
x = add (b) a
ret x
end
== line 2: add has 3 values after it; it takes 2
fn f a uint b uint to uint
x = add a b a
ret x
end
-- control
fn f a uint b uint to uint
x = add add a b a
ret x
end
== line 2: sub has 1 value after it; it takes 2
fn f a uint b uint to uint
x = sub mul a b
ret x
end
-- control
fn f a uint b uint to uint
x = sub mul a b a
ret x
end
== line 2: le has 3 values after it; it takes 2
fn f a uint b uint to uint
while le add a 1 b a bound 8
a = add a 1
end
ret a
end
-- control
fn f a uint b uint to uint
while le add a 1 b bound 8
a = add a 1
end
ret a
end
== line 2: extra words at the end of the line: 'b'
fn f a uint b uint to uint
ret a b
end
-- control
fn f a uint b uint to uint
ret a
end
== mul 2 3: two numbers
fn f a uint to uint
x = add a mul 2 3
ret x
end
-- control
fn f a uint to uint
x = add a mul a 3
ret x
end
== add needs two values of one type: 'a' is uint, '(ulong b)' is ulong
fn f a uint b uint to uint
x = add a (ulong b)
ret x
end
-- control
fn f a uint b uint to uint
x = add a (uint b)
ret x
end
== g has 1 value after it; it takes 2
fn g a uint b uint to uint
ret add a b
end
fn f a uint to uint
ret g a
end
-- control
fn g a uint b uint to uint
ret add a b
end
fn f a uint to uint
ret g a mul a 2
end
== the path already ended (ret, trap or break)
fn f a uint to uint
trap 1
x = add a 1
ret x
end
-- control
fn f a uint to uint
if gt a 9
trap 1
end
x = add a 1
ret x
end
== new makes an array for a name
fn total v view uint 4 to uint
ret get v 0
end
fn f a uint to uint
x = total new uint 4 a
ret x
end
-- control
fn total v view uint 4 to uint
ret get v 0
end
fn f a uint to uint
w = new uint 4 a
x = total w
free w
ret x
end
EOF
for want in "$T"/r*.want; do
  k="${want%.want}"
  w="$(cat "$want")"
  out="$("$G" emit "$k.bad" "$T/x.json" || true)"
  case "$out" in "REFUSED omegese: "*"$w"*) r=$((r + 1)) ;; *) bad "refusal '$w': got '$out'" ;; esac
  if ! "$G" emit "$k.ok" "$T/x.json" > "$T/x.out" || ! "$OME" emit "$T/x.json" "$T/x.oscir" > "$T/x.out"; then
    bad "control for '$w': $(cat "$T/x.out")"
  else
    v="$("$C2" check "$T/x.oscir" 2>&1 | head -1 || true)"
    case "$v" in checker2=ACCEPT*) ;; *) bad "control for '$w' checker2: $v" ;; esac
  fi
done
echo "refusals: $r/$(ls "$T"/r*.want | wc -l) refused with the expected reason, every control accepted"

# 3. loop guard negative control
ctr="$(grep -o '"op": "ge", "dst": [0-9]*, "a": [0-9]*' "$T/dev04.json" | head -1 | sed 's/.*"a": //')"
sed -E "s/\{\"op\": \"add\", \"dst\": $ctr, \"a\": $ctr, \"b\": [0-9]+\}, //" "$T/dev04.json" > "$T/m.json"
if cmp -s "$T/dev04.json" "$T/m.json"; then bad "loop mutant: the counter increment was not found"; fi
if "$OME" emit "$T/m.json" "$T/m.oscir" > "$T/m.out"; then
  v="$("$C2" check "$T/m.oscir" 2>&1 | head -1 || true)"
  case "$v" in *"rule=C2-LOOP-UNBOUNDED"*) echo "loop guard: mutant without the counter increment REJECTED (C2-LOOP-UNBOUNDED)" ;;
    *) bad "loop mutant not rejected: $v" ;; esac
else bad "loop mutant refused earlier than checker2: $(cat "$T/m.out")"; fi

# 4. tokenizer gate
if [ -n "${OMEGESE_TOKCOUNT:-}" ] && [ -n "${OMEGESE_TOKENIZER:-}" ]; then
  multi="$("$OMEGESE_TOKCOUNT" "$OMEGESE_TOKENIZER" words $("$G" words) bound | awk '$1 != 1 {print $3}')"
  if [ -n "$multi" ]; then bad "words that are not one token: $multi"; else echo "tokenizer gate: every fixed word is one token"; fi
else
  echo "tokenizer gate: SKIPPED (set OMEGESE_TOKCOUNT and OMEGESE_TOKENIZER)"
fi


# 5. next agrees with emit. Oracle: at every word boundary of every accepted program (dev, nested,
#    trial-2, coverage, refusal controls), the word that actually follows is among the words next
#    allows, so a mask built from next never blocks a valid program. Truncation fuzz: every byte
#    prefix of the nested, trial-2 and coverage programs goes through emit and next and ends with
#    exit 0 or 1 (no crash; run the suite on an ASan/UBSan build to check memory).
allowed() { # <next output> <word>: does the output allow the word
  local out="$1" w="$2" line x
  case "$out" in "REFUSED"*) return 1 ;; *"new-name:"*|*"(finish the fn line)"*) return 0 ;; esac
  if [ "$w" = NL ]; then case "$out" in *"(newline)"*) return 0 ;; *) return 1 ;; esac; fi
  if [[ "$w" =~ ^-?[0-9]+$ ]]; then case "$out" in *"number:"*) return 0 ;; esac; fi
  while IFS= read -r line; do
    for x in ${line#*:}; do [ "$x" = "$w" ] && return 0; done
  done <<< "$out"
  return 1
}
progs=("$ROOT"/tests/one_layer/omegese/dev/*.omegese "$ROOT"/tests/one_layer/omegese/nested/*.omegese "$T"/c*.omegese "$T"/r*.ok)
for f in "$ROOT"/tests/one_layer/omegese/trial2/*.omegese; do [ "$(basename "$f")" = dev07.omegese ] || progs+=("$f"); done
nb=0
for f in "${progs[@]}"; do
  s="$(cat "$f")"$'\n'
  for ((i = 0; i < ${#s}; i++)); do
    c="${s:i:1}" p="${s:i-1:1}"
    [ "$c" = " " ] && continue
    if [ "$i" -gt 0 ] && [ "$p" != " " ] && [ "$p" != $'\n' ] && [ "$p" != "(" ] && [ "$c" != ")" ] && [ "$c" != $'\n' ]; then continue; fi
    if [ "$c" = $'\n' ]; then w=NL
    elif [ "$c" = "(" ] || [ "$c" = ")" ]; then w="$c"
    else w="${s:i}"; w="${w%%[ ()$'\n']*}"; fi
    printf '%s' "${s:0:i}" > "$T/p"
    out="$("$G" next "$T/p" 2>&1 || true)"
    nb=$((nb + 1))
    allowed "$out" "$w" || { bad "next blocks '$w' after byte $i of $(basename "$f"): $(printf '%s' "$out" | head -3 | tr '\n' '|')"; break; }
  done
done
echo "next oracle: the following word allowed at all $nb word boundaries of ${#progs[@]} accepted programs"
nf=0
for f in "$ROOT"/tests/one_layer/omegese/nested/*.omegese "$ROOT"/tests/one_layer/omegese/trial2/*.omegese "$T"/c*.omegese; do
  s="$(cat "$f")"$'\n'
  for ((i = 0; i <= ${#s}; i++)); do
    printf '%s' "${s:0:i}" > "$T/p"
    rc=0; "$G" emit "$T/p" "$T/p.json" > /dev/null 2>&1 || rc=$?
    [ "$rc" -le 1 ] || { bad "emit exit $rc on byte prefix $i of $(basename "$f")"; break; }
    rc=0; "$G" next "$T/p" > /dev/null 2>&1 || rc=$?
    [ "$rc" -le 1 ] || { bad "next exit $rc on byte prefix $i of $(basename "$f")"; break; }
    nf=$((nf + 1))
  done
done
echo "truncation fuzz: $nf byte prefixes through emit and next, exit 0 or 1"
[ "$fails" -eq 0 ] || { echo "test-omegese: $fails failure(s)"; exit 1; }
echo "test-omegese: PASS"
