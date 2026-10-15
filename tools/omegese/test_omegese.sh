#!/usr/bin/env bash
# tools/omegese/test_omegese.sh <omegese> <omega_emit> <checker2> -- Omegese checks (docs/one_layer/OMEGESE.md).
#  0. with OMEGESE_SANITIZED=1 (an ASan and UBSan build): canary negative controls, then every sanitizer
#     exit fails the run;
#  1. every public dev program (tests/one_layer/omegese/dev) -> omegese emit -> omega_emit emit -> checker2
#     ACCEPT; 1a nested and trial-2 programs; 1b coverage programs; 1c omegese next on fixed prefixes;
#  2. refusals: each bad program is refused by omegese with its expected reason, and a control (the
#     same program with the defect fixed) is accepted;
#  3. loop guard negative control: the dev04 op list with the hidden loop counter increment removed
#     still passes omega_emit, and checker2 must REJECT it as C2-LOOP-UNBOUNDED;
#  4. tokenizer gate (only when OMEGESE_TOKCOUNT and OMEGESE_TOKENIZER are set): every fixed word of
#     the language, with its leading space, is exactly one token;
#  5. next oracle: every word of every accepted program is legal where it is written;
#  6. completeness: single-word mutants (OMEGESE_MUTANTS per program): emit accepts iff every word legal;
#  7. soundness fuzz (OMEGESE_WALKS walks of OMEGESE_STEPS items) closed by omegese complete; then the
#     checker2 residual (OMEGESE_RESIDUAL_WALKS walks from each of two starts, closed by complete, through
#     omega_emit and checker2; at most OMEGESE_RESIDUAL_MAX, default 0, rejected there); then the
#     truncation fuzz: every byte prefix through emit, next and next --machine answers, never crashes,
#     allows the byte that follows, and is finished by complete.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ $# -eq 3 ] || { echo "usage: test_omegese.sh <omegese> <omega_emit> <checker2>" >&2; exit 2; }
G="$1" OME="$2" C2="$3"
T="$(mktemp -d "${TMPDIR:-/tmp}/omegese-test.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# 0. sanitized run (OMEGESE_SANITIZED=1: <omegese> is built with ASan and UBSan, which exit 86 on a
#    finding; omegese itself only exits 0, 1 or 2). <omegese> is wrapped so that every exit 86 is logged
#    with its report and fails the run at the end, wherever the test reads the exit code. Negative control
#    first: each binary in OMEGESE_CANARIES (built with -DOMEGESE_CANARY=1, a read after free, or =2, a
#    signed overflow) must be caught, so a sanitizer that is silently off cannot pass.
if [ "${OMEGESE_SANITIZED:-0}" = 1 ]; then
  export ASAN_OPTIONS="exitcode=86" UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1:exitcode=86"
  nc=0
  for c in ${OMEGESE_CANARIES:-}; do
    rc=0; "$c" emit "$ROOT/tests/one_layer/omegese/dev/dev01.omegese" "$T/canary.json" > /dev/null 2> "$T/canary.err" || rc=$?
    if [ "$rc" -eq 86 ] && grep -qE 'heap-use-after-free|signed integer overflow' "$T/canary.err"; then nc=$((nc + 1))
    else bad "sanitizer negative control $(basename "$c"): not caught (exit $rc)"; fi
  done
  [ "$nc" -eq 2 ] || bad "sanitizer negative control: $nc canaries caught, want 2 (set OMEGESE_CANARIES)"
  echo "sanitizers: $nc/2 canaries caught (read after free, signed overflow)"
  printf '%s\n' '#!/usr/bin/env bash' "e=\"\$(mktemp)\"; rc=0; '$G' \"\$@\" 2> \"\$e\" || rc=\$?" \
    "[ \$rc -ne 86 ] || { echo \"== \$*\"; cat \"\$e\"; } >> '$T/san.log'" 'cat "$e" >&2; rm -f "$e"; exit $rc' > "$T/gsan"
  chmod +x "$T/gsan"; G="$T/gsan"
fi

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

# 1a'. review fixes: a conversion of a typed number of another type must convert (not reuse the
#      number's register under the wrong type); brackets around a for count change nothing
printf 'fn f to long\nret long int 5\nend\n' > "$T/s1.omegese"
accept "$T/s1.omegese" stacked-ret || true
printf 'fn f x ulong to ulong\nret shl x uchar int 300\nend\n' > "$T/s2.omegese"
accept "$T/s2.omegese" stacked-shift || true
grep -q '"op": "cast"' "$T/stacked-shift.json" 2>/dev/null || bad "uchar int 300 must emit a cast (trap 5), not reuse the int const"
printf 'fn f to uint\ns = uint 0\nfor i ulong %s\ns = add s 1\nend\nret s\nend\n' 8 > "$T/f1.omegese"
printf 'fn f to uint\ns = uint 0\nfor i ulong %s\ns = add s 1\nend\nret s\nend\n' '( ( 8 ) )' > "$T/f2.omegese"
"$G" emit "$T/f1.omegese" "$T/f1.json" > /dev/null && "$G" emit "$T/f2.omegese" "$T/f2.json" > /dev/null \
  && cmp -s "$T/f1.json" "$T/f2.json" || bad "for i ulong ( ( 8 ) ) must lower exactly as for i ulong 8"
printf 'fn f to uint\ns = uint 0\nfor i ulong ( 2000000 )\ns = add s 1\nend\nret s\nend\n' > "$T/f3.omegese"
out="$("$G" emit "$T/f3.omegese" "$T/f3.json" || true)"
[ "$out" = "REFUSED omegese: line 3: count '2000000' must be 0..1000000" ] || bad "bracketed count over the limit: got '$out'"
echo "review fixes: stacked conversions convert; brackets around a for count change nothing"

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

# 1c. omegese next: what may come next after a prefix, exactly (docs/one_layer/OMEGESE.md section 6).
#     At a line start: "words:" (statement words and functions to call), "assign:" (names that may
#     be assigned), "new-name:"; elsewhere: "names:", "number: lo..hi" (the numbers that may stand
#     there), "new-name:" and "words:" (fixed words, functions, brackets, "(space)" inside a word,
#     "(newline)", "(end of input)"). Only what can still end in a program emit accepts is offered.
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
nxeq 'fn f a uint b ulong to uint\nx = add a ' 'names: a\nnumber: 0..4294967295\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f a uint to uint\nfor i uint 4\n' 'assign: a\nnew-name: any name not in use\nwords: if while for ret trap break end'
nxcase 'fn f a uint to uint\nret a\nret ' 'REFUSED omegese: *path already ended*' 'after a dead line: not refused'
nxcase 'x = add ' 'REFUSED omegese: *outside a function*' 'before fn: not refused'
nxeq 'fn odd a uint to bool\nret true\nend\nfn f a uint fl view bool 4 to uint\nif ' 'words: true false and or xor eq ne lt le gt ge not get bool odd ('
# nesting: a stack of open operand slots, each with the type it needs
nxeq 'fn f r uint q ulong c bool to uint\nx = add mul r ' 'names: r\nnumber: 0..4294967295\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f r uint q ulong c bool to uint\nx = add mul r r ' 'names: r\nnumber: 0..4294967295\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f r uint q ulong c bool to uint\nret sub ' 'names: r\nnumber: 0..4294967295\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f p short to short\nif lt p short ' 'names: p\nnumber: -32768..32767\nwords: add sub mul div rem and or xor shl shr neg not uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64 ('
# the type is still open: every whole-number name (bool and arrays excluded for add) and any number
# some whole-number type holds; after a bare number, no second bare number, and only names it fits
nxeq 'fn f r uint q ulong c bool to uint\nx = add ' 'names: r q\nnumber: -9223372036854775808..18446744073709551615\nwords: add sub mul div rem and or xor shl shr neg not uchar ushort uint ulong byte short int long u8 u16 u32 u64 i8 i16 i32 i64 ('
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
# never offer a word that leads to a dead end: no end before ret, no trap while b is unread, no put
# or free without an array
nxeq 'fn f a uint to uint\nb = add a 1\n' 'assign: a b\nnew-name: any name not in use\nwords: if while for ret'
nxcase 'fn f a uint to uint\nb = add a 1\nend\n' 'REFUSED omegese: *without ret*' 'end before ret: not refused'
nxcase 'fn f a uint to uint\nelse\n' 'REFUSED omegese: line 2: else without an open if' 'stray else: not refused'
nxcase 'fn f a uint to uint\nbreak\n' 'REFUSED omegese: line 2: break outside a loop' 'stray break: not refused'
nxcase 'fn f a uint to uint\nb = add a 1\ntrap 1\n' "REFUSED omegese: line 4: *'b' is written but never read*" 'a trap that leaves b unread: not refused'
# ret must read every name the ended path leaves unread: b only (no bare number, no a alone); a bool
# with no function to turn it into a number cannot be read by a number; inside an if, only the
# names of the if
nxeq 'fn f a uint to uint\nb = add a 1\nret ' 'names: b\nwords: add sub mul div rem and or xor shl shr not uint u32 ('
nxcase 'fn f a uint to uint\nb = gt a 1\nret ' "REFUSED omegese: *'b' is written but never read*" 'an unreadable bool before ret: not refused'
nxeq 'fn h c bool to ulong\nret ulong 0\nend\nfn f a uint to uint\nb = gt a 1\nret add a ' 'words: add sub mul div rem and or xor shl shr not uint u32 ('
nxeq 'fn f a uint v view bool 4 to bool\nx = view v\nret eq ' 'words: true false and or xor eq ne lt le gt ge not get bool ('
nxeq 'fn f a uint v view uint 4 to uint\nx = view v\nif gt a 2\ny = add a 1\nret ' 'names: y\nwords: add sub mul div rem and or xor shl shr not get uint u32 ('
# inside a word: the words that extend it, a name not in use, and what may end it
nxeq 'fn f a uint to uint\nb = add a 1\nen' 'new-name: any name not in use\nwords: ( (space)'
nxeq 'fn f a uint to uint\nb = add a 1\nre' 'new-name: any name not in use\nwords: ret ( (space)'
nxeq 'fn f a uint to uint\nx = new uint 4 0\nfree x\nx = new uint ' 'number: 4..4'
nxeq 'fn f a uint to uint\nx = new uint 4 0\nfree x\nx = new uint 4' 'number: 4..4\nwords: ( (space)'
nxcase 'fn f a uint to uint\nx = new uint 4 0\nfree x\nx = new uint 1' 'REFUSED omegese: line 4: *own uint 1 but the name holds own uint 4*' 'a length no number extends: not refused'
# ownership (checker2's C2-OWN rules, OMEGESE.md 6.4): no ret while an own array holds its array on
# every path, no second free; at a line start ret is left out until the array is freed; after a loop
# that frees and refills it the array still holds one on every path, so complete frees it
nxcase 'fn f a own uint 4 to uint\nret ' "REFUSED omegese: line 2: no text after this prefix*'a' still holds its array*" 'ret while an own array holds its array: not refused'
nxcase 'fn f a own uint 4 to uint\nfree a\nfree ' "REFUSED omegese: line 3: *freed or moved on every path*" 'a second free: not refused'
nxk=$((nxk + 1)); got="$(printf 'fn f a own uint 4 to uint\n' > "$T/p"; "$G" next --machine "$T/p")"
[ "$got" = "$(printf 'word if\nword while\nword for\nword put\nword free\nword trap\nword a\nnewname a f\nspace\nnewline')" ] || bad "next --machine with an own array that must be freed: got '$got'"
nxk=$((nxk + 1)); got="$(printf 'fn f a own uint 4 n uint to uint\nwhile gt n 0\nfree a\na = new uint 4 n\nn = sub n 1\nend\n' > "$T/p"; "$G" complete "$T/p")"
[ "$got" = "$(printf 'free a\nret 0\nend')" ] || bad "complete after a loop that refills an own array: got '$got'"
# a loop's state at its head is known at its end line: a use of an array freed before the loop is
# refused there unless the body gives it a new one, so end is left out and complete writes that line
nxeq 'fn f a own uint 4 n uint to uint\nfree a\nwhile gt get a 0 n\nn = add n 1\n' 'assign: a n\nnew-name: any name not in use\nwords: if while for put free'
nxcase 'fn f a own uint 4 n uint to uint\nfree a\nwhile gt get a 0 n\nn = add n 1\nend\n' "REFUSED omegese: line 5: 'a' holds no array at line 3 on every path, once this loop is closed*" 'end of a loop whose head uses a freed array: not refused'
nxk=$((nxk + 1)); got="$(printf 'fn f a own uint 4 n uint to uint\nfree a\nwhile gt get a 0 n\nn = add n 1\n' > "$T/p"; "$G" complete "$T/p")"
[ "$got" = "$(printf 'a = new uint 4 0\nend\nret 0\nend')" ] || bad "complete in a loop whose head uses a freed array: got '$got'"
nxk=$((nxk + 1)); got="$(printf 'fn f n uint to void\nx = new uint 4 n\nwhile gt n 0\nx = new uint 4 n\n' > "$T/p"; "$G" complete "$T/p")"
[ "$got" = "$(printf 'free x\nend\nend')" ] || bad "complete in a loop that gives an array a second one: got '$got'"
# a loop opened at the nesting limit is refused before its head state is set up (no shift past bit 63)
{ echo 'fn f n uint to uint'; for ((i = 0; i < 63; i++)); do echo "if gt n $i"; done; echo 'while gt n 0'; } > "$T/deep.omegese"
nxk=$((nxk + 1)); got="$("$G" emit "$T/deep.omegese" "$T/x.json" 2>&1 || true)"
case "$got" in "REFUSED omegese: line 65: more than 64 nested blocks"*) ;; *) bad "a loop at the nesting limit: got '$got'" ;; esac
# 300 uses of one array inside a loop wait as one check (the same state): accepted, as checker2 does
{ echo 'fn f a own uint 4 n uint to uint'; echo 'for i uint 3'; for ((i = 0; i < 300; i++)); do echo 'n = get a 0'; done; echo 'end'; echo 'free a'; echo 'ret n'; echo 'end'; } > "$T/many.omegese"
nxk=$((nxk + 1)); accept "$T/many.omegese" many-uses-in-a-loop || true
nxeq 'fn f a short to short\nret add a -' 'number: -32768..32767'
nxeq 'fn f a short to short\nret add a -0' 'REFUSED omegese: line 2: no text after this prefix makes a program emit accepts ('"'"'-0'"'"' is not a number (digits, an optional minus sign, no leading zeros, no -0))'
# the number ranges each slot allows: every scalar type, a minus sign where none fits, and the fixed
# limits (trap codes, array lengths, loop bounds and counts)
nxnum() { # <prefix> <lo..hi> <what>: the number line is exactly that interval
  nxk=$((nxk + 1))
  local got; got="$(nx "$1")"$'\n'
  [[ $got == *"number: $2"$'\n'* ]] || bad "next $3: got '$got'"
}
for tr in uchar:0..255 ushort:0..65535 uint:0..4294967295 ulong:0..18446744073709551615 byte:-128..127 \
          short:-32768..32767 int:-2147483648..2147483647 long:-9223372036854775808..9223372036854775807; do
  nxnum "fn f a ${tr%%:*} to ${tr%%:*}\\nret add a " "${tr#*:}" "range of ${tr%%:*}"
done
nxcase 'fn f a uint to uint\nret add a -' 'REFUSED omegese: line 2: no text after this prefix*' 'a minus sign in an unsigned slot: not refused'
nxnum 'fn f a uint to uint\ntrap ' '1..14' 'trap codes'
nxnum 'fn f a uint to uint\nx = new uint ' '1..64' 'array lengths'
nxnum 'fn f a uint to uint\nwhile lt a 3 bound ' '1..1000000' 'loop bounds'
nxnum 'fn f a uint to uint\nfor i uint ' '0..1000000' 'loop counts'
# the machine form: one item per line
nxm() { printf '%b' "$1" > "$T/p"; "$G" next --machine "$T/p" || true; }
nxk=$((nxk + 1)); got="$(nxm 'fn f a uint to uint\nb = add a 1\nre')"
[ "$got" = "$(printf 'word ret\nnewname\n(\nspace')" ] || bad "next --machine after 're': got '$got'"
nxk=$((nxk + 1)); got="$(nxm 'fn f a uint b ulong to uint\nret a\nend\n')"
[ "$got" = "$(printf 'word fn\nspace\nnewline\neof')" ] || bad "next --machine after a function: got '$got'"
nxk=$((nxk + 1)); got="$(nxm 'fn f a uint b ulong to uint\nx = add a')"
[ "$got" = "$(printf 'word add\nword and\nword a\n(\nspace')" ] || bad "next --machine inside a name: got '$got'"
nxk=$((nxk + 1)); got="$(nxm 'fn f a uint to uint\n')"
[ "$got" = "$(printf 'word if\nword while\nword for\nword ret\nword trap\nword a\nnewname a f\nspace\nnewline')" ] || bad "next --machine at a line start (OMEGESE.md 6.2): got '$got'"
nxk=$((nxk + 1)); got="$(nxm 'fn f ab uint b uint to uint\nfor abc uint 3\nx = add abc a')"
[ "$got" = "$(printf 'word add\nword and\nword ab\nword abc')" ] || bad "next --machine, names that extend the word: got '$got'"
nxk=$((nxk + 1)); got="$(nxm 'fn f ab uint b uint to uint\nfor abc uint 3\na')"
[ "$got" = "$(printf 'word ab\nnewname ab abc\n(\nspace')" ] || bad "next --machine, a new name that extends names in use: got '$got'"
# complete: the witness text, which emit accepts after the prefix
nxk=$((nxk + 1)); got="$(printf 'fn f a uint v view uint 4 to uint\nx = view v\nif gt a 2\ny = add a 1\nret ' > "$T/p"; "$G" complete "$T/p")"
[ "$got" = "$(printf ' y\nend\nx = x\nret 0\nend')" ] || bad "complete inside an if: got '$got'"
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
== still holds its array at this return on every path
fn f a own uint 4 to uint
ret 1
end
-- control
fn f a own uint 4 to uint
free a
ret 1
end
== still holds its array at this return on every path
fn f n uint to void
x = new uint 4 n
put x 0 n
end
-- control
fn f n uint to void
x = new uint 4 n
put x 0 n
free x
end
== was already freed or moved on every path
fn f a own uint 4 to void
free a
free a
end
-- control
fn f a own uint 4 n uint to void
while gt n 0
free a
a = new uint 4 n
n = sub n 1
end
free a
end
== was freed or moved on every path to this line
fn g b own uint 4 to void
free b
end
fn f a own uint 4 to uint
g own a
x = get a 0
ret x
end
-- control
fn g b own uint 4 to void
free b
end
fn f a own uint 4 to uint
x = get a 0
g own a
ret x
end
== still holds an array on every path to this line
fn f n uint to uint
x = new uint 4 n
x = new uint 4 0
y = get x 0
free x
ret y
end
-- control
fn f n uint to uint
x = new uint 4 n
free x
x = new uint 4 0
y = get x 0
free x
ret y
end
== still holds its array at this return on every path
fn f n uint to uint
x = new uint 4 n
if gt n 3
ret 0
end
free x
ret 1
end
-- control
fn f n uint to uint
x = new uint 4 n
if gt n 3
free x
end
free x
ret 1
end
== out of scope here still holds its data on every path to this return
fn f n uint to uint
if gt n 3
y = new uint 4 n
put y 0 n
else
trap 1
end
ret 0
end
-- control
fn f n uint to uint
if gt n 3
y = new uint 4 n
put y 0 n
end
ret 0
end
== 'a' holds no array at line 3 on every path, once this loop is closed
fn f a own uint 4 n uint to uint
free a
while gt get a 0 n
n = add n 1
end
ret n
end
-- control
fn f a own uint 4 n uint to uint
free a
while gt get a 0 n
a = new uint 4 n
n = add n 1
end
ret n
end
== 'a' holds no array at line 4 on every path, once this loop is closed
fn f a own uint 4 n uint to void
free a
while gt n 0
free a
n = sub n 1
end
end
-- control
fn f a own uint 4 n uint to void
free a
while gt n 0
free a
a = new uint 4 n
n = sub n 1
end
end
== 'x' still holds an array at line 4 on every path, once this loop is closed
fn f n uint to void
x = new uint 4 n
while gt n 0
x = new uint 4 n
n = sub n 1
end
free x
end
-- control
fn f n uint to void
x = new uint 4 n
while gt n 0
free x
x = new uint 4 n
n = sub n 1
end
free x
end
== 'x' still holds its array at the return on line 4 on every path, once this loop is closed
fn f n uint to uint
x = new uint 4 n
while gt n 0
ret n
end
free x
ret 0
end
-- control
fn f n uint to uint
x = new uint 4 n
while gt n 0
free x
ret n
end
free x
ret 0
end
== 'a' holds no array at line 5 on every path, once this loop is closed
fn f a own uint 4 n uint to void
free a
while gt n 0
while gt n 1
x = get a 0
n = sub n x
end
n = sub n 1
end
end
-- control
fn f a own uint 4 n uint to void
free a
while gt n 0
while gt n 1
x = get a 0
n = sub n x
end
free a
a = new uint 4 n
n = sub n 1
end
free a
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


# 5. next is exact (docs/one_layer/OMEGESE.md section 6). `next --machine` prints the legal items, one
#    per line: word <w>, newname <excluded>..., number <lo> <hi>, (, ), space, newline, eof.
#    Oracle: at every word boundary of every accepted program (dev, nested, trial-2, coverage, refusal
#    controls) the word that follows is legal, and eof is legal at the end. Truncation fuzz: every
#    byte prefix of the nested, trial-2 and coverage programs goes through emit and next and ends with
#    exit 0 or 1 (no crash; the suite also runs on an ASan/UBSan build); for accepted programs next
#    must also allow the byte that follows, and complete must finish the prefix into an accepted program.
FIXEDW=" $("$G" words | tr '\n' ' ')u8 u16 u32 u64 i8 i16 i32 i64 "
dec_le() { # <a> <b>: a <= b, for canonical decimal integers of any size
  local a="$1" b="$2" x y
  if [[ $a == -* ]]; then
    [[ $b == -* ]] || return 0
    x="${a#-}" y="${b#-}"
    [ ${#x} -ne ${#y} ] && { [ ${#x} -gt ${#y} ]; return; }
    [[ ! $x < $y ]]; return
  fi
  [[ $b == -* ]] && return 1
  [ ${#a} -ne ${#b} ] && { [ ${#a} -lt ${#b} ]; return; }
  [[ ! $a > $b ]]
}
mallowed() { # <items> <word>: do the items allow the whole word (NL a newline, EOF the end of input)
  local items="$1" w="$2" k lo hi rest
  local it=$'\n'"$items"$'\n'
  case "$items" in REFUSED*|"") return 1 ;; esac
  case "$w" in
    NL) [[ $it == *$'\nnewline\n'* ]]; return ;;
    EOF) [[ $it == *$'\neof\n'* ]]; return ;;
    "("|")") [[ $it == *$'\n'"$w"$'\n'* ]]; return ;;
  esac
  [[ $it == *$'\n'"word $w"$'\n'* ]] && return 0
  if [[ $w =~ ^(0|-?[1-9][0-9]*)$ ]]; then
    while read -r k lo hi; do [ "$k" = number ] && dec_le "$lo" "$w" && dec_le "$w" "$hi" && return 0; done <<< "$items"
    return 1
  fi
  [[ $w =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] && [ ${#w} -le 63 ] && [[ $FIXEDW != *" $w "* ]] || return 1
  while read -r k rest; do
    [ "$k" = newname ] || continue
    [[ " $rest " == *" $w "* ]] && return 1
    return 0
  done <<< "$items"
  return 1
}
toks() { # <text>: TO[] the byte offset and TW[] the text of every word (NL for a newline)
  local s="$1" i=0 n=${#1} j c
  TO=() TW=()
  while ((i < n)); do
    c="${s:i:1}"
    case "$c" in
      " "|$'\t'|$'\r') i=$((i + 1)) ;;
      $'\n') TO+=("$i"); TW+=(NL); i=$((i + 1)) ;;
      "("|")") TO+=("$i"); TW+=("$c"); i=$((i + 1)) ;;
      *) j=$i
         while ((j < n)); do case "${s:j:1}" in " "|$'\t'|$'\r'|$'\n'|"("|")") break ;; esac; j=$((j + 1)); done
         TO+=("$i"); TW+=("${s:i:j-i}"); i=$j ;;
    esac
  done
}
mnext() { printf '%s' "$1" > "$T/p"; "$G" next --machine "$T/p" 2>&1 || true; }
NB=0
walk_in() { # <text>: every word is legal at its position and eof at the end; else WHERE says where not
  local s="$1" k items
  toks "$s"
  for ((k = 0; k < ${#TW[@]}; k++)); do
    items="$(mnext "${s:0:TO[k]}")"
    NB=$((NB + 1))
    mallowed "$items" "${TW[k]}" || { WHERE="'${TW[k]}' at byte ${TO[k]}: $(head -3 <<< "$items" | tr '\n' '|')"; return 1; }
  done
  items="$(mnext "$s")"
  mallowed "$items" EOF || { WHERE="the end of input: $(head -3 <<< "$items" | tr '\n' '|')"; return 1; }
}
progs=("$ROOT"/tests/one_layer/omegese/dev/*.omegese "$ROOT"/tests/one_layer/omegese/nested/*.omegese "$T"/c*.omegese "$T"/r*.ok)
for f in "$ROOT"/tests/one_layer/omegese/trial2/*.omegese; do [ "$(basename "$f")" = dev07.omegese ] || progs+=("$f"); done
for f in "${progs[@]}"; do
  s="$(cat "$f")"$'\n'
  walk_in "$s" || bad "next blocks $WHERE of $(basename "$f")"
done
echo "next oracle: every word and the end of input legal at all $NB word boundaries of ${#progs[@]} accepted programs"

# 6. completeness and soundness against emit: single-word mutants of every accepted program (a word
#    deleted, doubled or replaced by a fixed word, a name of the program, a number or a fresh name).
#    emit accepts a mutant iff every word of it was legal at its position and eof at its end.
RANDOM=445
NUMS=(0 1 -1 7 14 15 64 65 127 128 255 256 -128 -129 1000000 1000001 4294967295 4294967296 -0 007)
read -ra FIXA <<< "$FIXEDW"
nmut=0 nacc=0 NB=0
for f in "${progs[@]}"; do
  s="$(cat "$f")"$'\n'
  toks "$s"
  ow=("${TW[@]}")
  ids=()
  for w in "${ow[@]}"; do [[ $w =~ ^[A-Za-z_][A-Za-z0-9_]*$ ]] && [[ $FIXEDW != *" $w "* ]] && ids+=("$w"); done
  for ((m = 0; m < ${OMEGESE_MUTANTS:-2}; m++)); do
    n=${#ow[@]} k=$((RANDOM % ${#ow[@]})) how=$((RANDOM % 6))
    case $how in
      0) rep=() ;;
      1) rep=("${ow[k]}" "${ow[k]}") ;;
      2) rep=("${NUMS[RANDOM % ${#NUMS[@]}]}") ;;
      3) rep=("${ids[RANDOM % ${#ids[@]}]}") ;;
      4) rep=("zq$m") ;;
      *) rep=("${FIXA[RANDOM % ${#FIXA[@]}]}") ;;
    esac
    out="" prev=NL
    for ((q = 0; q < n; q++)); do
      if ((q == k)); then set -- "${rep[@]}"; else set -- "${ow[q]}"; fi
      for w in "$@"; do
        if [ "$w" = NL ]; then out+=$'\n'; else [ "$prev" = NL ] || out+=" "; out+="$w"; fi
        prev="$w"
      done
    done
    printf '%s' "$out" > "$T/mut.omegese"
    acc=0; "$G" emit "$T/mut.omegese" "$T/mut.json" > /dev/null 2>&1 && acc=1
    in=0; walk_in "$out" && in=1
    nmut=$((nmut + 1)) nacc=$((nacc + acc))
    [ "$acc" = "$in" ] || bad "mutant of $(basename "$f") (word $k -> '${rep[*]}'): emit accepts=$acc, every word legal=$in (${WHERE:-})"
  done
done
echo "completeness: $nmut single-word mutants ($nacc accepted by emit): emit accepts iff every word was legal ($NB word boundaries)"

# 7. soundness fuzz: from random accepted prefixes, pick random legal items and extend; the legal set is
#    never empty, a word picked is still legal after any of its first characters, and `omegese
#    complete` (close everything) finishes the walk into a program emit accepts, in a bounded number of
#    lines (at most the words of the prefix plus 3).
RANDOM=4452
nwalk=0 nstep=0
# walk_items <steps>: extend P by up to <steps> random legal items (stops at eof); every set must be
# nonempty and a word picked must stay legal after each of its first characters. Returns 1 (and counts a
# failure) when not.
walk_items() {
  local st items it x lo hi v q d j c
  for ((st = 0; st < $1; st++)); do
    items="$(mnext "$P")"
    if [ -z "$items" ] || [[ $items == REFUSED* ]]; then bad "soundness: no legal item after $(printf '%s' "$P" | tail -c 70 | tr '\n' '|') ($items)"; return 1; fi
    mapfile -t IT <<< "$items"
    it="${IT[RANDOM % ${#IT[@]}]}" x=""
    nstep=$((nstep + 1))
    case "$it" in
      eof) return 0 ;;
      newline) P+=$'\n' ;;
      space) P+=" " ;;
      "("|")") P+="$it" ;;
      "word "*) x="${it#word }" ;;
      newname*) for ((q = 0; q < 9; q++)); do x="n$RANDOM"; mallowed "$items" "$x" && break; done ;;
      number*) read -r _ lo hi <<< "$it"; c=()
               for v in "$lo" "$hi" 0 1 -1 9 100; do dec_le "$lo" "$v" && dec_le "$v" "$hi" && c+=("$v"); done
               # and a random value inside: by arithmetic when the interval fits, else random digits
               if [ ${#lo} -le 17 ] && [ ${#hi} -le 17 ]; then
                 c+=("$((lo + (RANDOM << 30 | RANDOM << 15 | RANDOM) % (hi - lo + 1)))")
               else
                 for ((q = 0; q < 9; q++)); do
                   v=$((1 + RANDOM % 9)); for ((d = RANDOM % 20; d > 0; d--)); do v+=$((RANDOM % 10)); done
                   [[ $lo == -* ]] && ((RANDOM % 2)) && v="-$v"
                   dec_le "$lo" "$v" && dec_le "$v" "$hi" && { c+=("$v"); break; }
                 done
               fi
               x="${c[RANDOM % ${#c[@]}]}" ;;
      *) bad "soundness: unknown item '$it'"; return 1 ;;
    esac
    [ -n "$x" ] || continue
    if [ ${#x} -ge 2 ]; then
      j=$((1 + RANDOM % (${#x} - 1)))
      mallowed "$(mnext "$P${x:0:j}")" "$x" || { bad "soundness: '$x' legal, but not after its first $j characters: $(printf '%s' "$P" | tail -c 50 | tr '\n' '|')"; return 1; }
    fi
    P+="$x "
  done
}
# close_walk <out file>: `omegese complete` finishes P; the whole text goes to <out file>, which emit must
# accept (returns 1 and counts a failure when complete refuses or emit refuses the result)
close_walk() {
  printf '%s' "$P" > "$T/w.omegese"
  if ! "$G" complete "$T/w.omegese" > "$T/w.tail" 2> "$T/w.err"; then bad "soundness: complete refused after a legal walk: $(cat "$T/w.tail" "$T/w.err" | head -2)"; return 1; fi
  cat "$T/w.omegese" "$T/w.tail" > "$1"
  "$G" emit "$1" "$T/w2.json" > "$T/w2.out" || { bad "soundness: walk + completion refused: $(cat "$T/w2.out")"; return 1; }
}
for ((wk = 0; wk < ${OMEGESE_WALKS:-40}; wk++)); do
  f="${progs[RANDOM % ${#progs[@]}]}"
  s="$(cat "$f")"$'\n'
  toks "$s"
  P="${s:0:TO[RANDOM % ${#TO[@]}]}"
  case "${P: -1}" in ""|" "|$'\n'|"("|")") ;; *) P+=" " ;; esac # start at a word boundary
  walk_items "${OMEGESE_STEPS:-25}" || continue
  close_walk "$T/w2.omegese" || continue
  toks "$P"
  lines=$(grep -c '' "$T/w.tail" || true)
  [ "$lines" -le $((${#TW[@]} + 3)) ] || bad "soundness: completion of $lines lines for a prefix of ${#TW[@]} words"
  nwalk=$((nwalk + 1))
done
echo "soundness fuzz: $nwalk/${OMEGESE_WALKS:-40} random walks ($nstep legal items picked) never met an empty set and closed into accepted programs"
# 8. checker2 residual (OMEGESE.md 6.4): `omegese next` is exact relative to emit, not to omega_emit and
#    checker2. Random walks from the empty prefix (whole programs) and from cut points of the suite's
#    programs, each closed by `omegese complete` (so emit accepts it, checked), then omega_emit and
#    checker2: every rejection after emit is counted by stage and reason. The suite's own programs are
#    the zero line (all accepted, sections 1 and 1a). OMEGESE_RESIDUAL_WALKS sets the number of walks
#    of each kind; OMEGESE_RESIDUAL_MAX the rejections allowed (default 0: none is known after the
#    rules folded into the oracle; a measured rate is printed either way).
declare -A RES=()
chain_after_emit() { # <program> -> "" when omega_emit and checker2 accept, else "<stage> <reason>"
  "$G" emit "$1" "$T/r.json" > "$T/r.out" 2>&1 || { echo "emit $(head -c 80 "$T/r.out")"; return; }
  if ! "$OME" emit "$T/r.json" "$T/r.oscir" > "$T/r.out" 2>&1; then
    echo "omega_emit $(head -n 1 "$T/r.out" | sed -E "s/'[^']*'/X/g; s/[0-9]+/N/g" | cut -c1-70)"; return
  fi
  local v; v="$("$C2" check "$T/r.oscir" 2>&1 | head -n 1 || true)"
  case "$v" in checker2=ACCEPT*) ;; *) echo "checker2 $(grep -oE 'C2-[A-Z0-9-]+' <<< "$v" | head -n 1)" ;; esac
}
nres=0 nrej=0
RANDOM=4453
for ((wk = 0; wk < ${OMEGESE_RESIDUAL_WALKS:-40}; wk++)); do
  for start in empty cut; do
    if [ "$start" = empty ]; then P=""
    else
      f="${progs[RANDOM % ${#progs[@]}]}"; s="$(cat "$f")"$'\n'; toks "$s"
      P="${s:0:TO[RANDOM % ${#TO[@]}]}"
      case "${P: -1}" in ""|" "|$'\n'|"("|")") ;; *) P+=" " ;; esac
    fi
    walk_items "${OMEGESE_STEPS:-25}" || continue
    close_walk "$T/res.omegese" || continue
    nres=$((nres + 1))
    why="$(chain_after_emit "$T/res.omegese")"
    [ -n "$why" ] || continue
    nrej=$((nrej + 1)); RES[$why]=$((${RES[$why]:-0} + 1))
    [ -n "${OMEGESE_RESIDUAL_KEEP:-}" ] && cp "$T/res.omegese" "$OMEGESE_RESIDUAL_KEEP/rej-$nrej.omegese"
  done
done
for k in "${!RES[@]}"; do echo "  residual: ${RES[$k]} x $k"; done | LC_ALL=C sort
echo "checker2 residual: $nrej of $nres completed walks that emit accepts are rejected later (omega_emit or checker2)"
[ "$nres" -gt 0 ] || bad "residual: no completed walk"
[ "$nrej" -le "${OMEGESE_RESIDUAL_MAX:-0}" ] || bad "residual: $nrej walks pass emit and are rejected later (allowed ${OMEGESE_RESIDUAL_MAX:-0})"
nf=0 nfs=0
WC=$'[^ \t\r\n()]'
for f in "$ROOT"/tests/one_layer/omegese/nested/*.omegese "$ROOT"/tests/one_layer/omegese/trial2/*.omegese "$T"/c*.omegese; do
  s="$(cat "$f")"$'\n'
  # an accepted program: every byte prefix must also be open (next exit 0), allow the byte that
  # follows (the whole word it belongs to, a space, a newline, a bracket or the end), and be finished
  # by `omegese complete` into a program emit accepts. A refused one (trial2/dev07): no crash only.
  full=0; printf '%s' "$s" > "$T/p"; "$G" emit "$T/p" "$T/p.json" > /dev/null 2>&1 && full=1
  for ((i = 0; i <= ${#s}; i++)); do
    printf '%s' "${s:0:i}" > "$T/p"
    rc=0; "$G" emit "$T/p" "$T/p.json" > /dev/null 2>&1 || rc=$?
    [ "$rc" -le 1 ] || { bad "emit exit $rc on byte prefix $i of $(basename "$f")"; break; }
    rc=0; "$G" next "$T/p" > /dev/null 2>&1 || rc=$?
    [ "$rc" -le 1 ] || { bad "next exit $rc on byte prefix $i of $(basename "$f")"; break; }
    rc=0; items="$("$G" next --machine "$T/p" 2>&1)" || rc=$?
    [ "$rc" -le 1 ] || { bad "next --machine exit $rc on byte prefix $i of $(basename "$f")"; break; }
    nf=$((nf + 1))
    [ "$full" = 1 ] || continue
    [ "$rc" -eq 0 ] || { bad "byte prefix $i of $(basename "$f") refused: $items"; break; }
    c="${s:i:1}"
    case "$c" in
      "") w=EOF ;;
      " "|$'\t') w=space ;;
      $'\n') w=NL ;;
      "("|")") w="$c" ;;
      *) [[ ${s:0:i} =~ ($WC*)$ ]]; w="${BASH_REMATCH[1]}"; [[ ${s:i} =~ ^($WC*) ]]; w+="${BASH_REMATCH[1]}" ;;
    esac
    if [ "$w" = space ]; then [[ $'\n'"$items"$'\n' == *$'\nspace\n'* ]]
    else mallowed "$items" "$w"; fi || { bad "byte prefix $i of $(basename "$f"): the next byte ('$w') is not allowed"; break; }
    rc=0; "$G" complete "$T/p" > "$T/p.c" 2>&1 || rc=$?
    { [ "$rc" -eq 0 ] && cat "$T/p" "$T/p.c" > "$T/pc.omegese" && "$G" emit "$T/pc.omegese" "$T/p.json" > /dev/null 2>&1; } \
      || { bad "byte prefix $i of $(basename "$f"): complete does not finish it into an accepted program"; break; }
    nfs=$((nfs + 1))
  done
done
echo "truncation fuzz: $nf byte prefixes through emit, next and next --machine, exit 0 or 1; at the $nfs prefixes of accepted programs, next allows the byte that follows and complete finishes into a program emit accepts"
if [ "${OMEGESE_SANITIZED:-0}" = 1 ]; then
  if [ -s "$T/san.log" ]; then bad "sanitizer reports: $(grep -m3 -E '^==|ERROR|runtime error' "$T/san.log")"
  else echo "sanitizers: no ASan or UBSan report in the whole run"; fi
fi
[ "$fails" -eq 0 ] || { echo "test-omegese: $fails failure(s)"; exit 1; }
echo "test-omegese: PASS"
