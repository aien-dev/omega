#!/usr/bin/env bash
# tools/glyph/test_glyph.sh <glyph> <omega_emit> <checker2> -- Glyph checks (docs/one_layer/GLYPH.md).
#  1. every public dev program (tests/one_layer/glyph/dev) -> glyph emit -> omega_emit emit -> checker2
#     ACCEPT;
#  2. refusals: each bad program is refused by glyph with its expected reason, and a control (the
#     same program with the defect fixed) is accepted;
#  3. loop guard negative control: the dev04 op list with the hidden loop counter increment removed
#     still passes omega_emit, and checker2 must REJECT it as C2-LOOP-UNBOUNDED;
#  4. tokenizer gate (only when GLYPH_TOKCOUNT and GLYPH_TOKENIZER are set): every fixed word of
#     the language, with its leading space, is exactly one token.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ $# -eq 3 ] || { echo "usage: test_glyph.sh <glyph> <omega_emit> <checker2>" >&2; exit 2; }
G="$1" OME="$2" C2="$3"
T="$(mktemp -d "${TMPDIR:-/tmp}/glyph-test.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
fails=0
bad() { echo "FAIL $*"; fails=$((fails + 1)); }

# 1. dev programs
n=0
for f in "$ROOT"/tests/one_layer/glyph/dev/dev*.glyph; do
  id="$(basename "$f" .glyph)"
  if ! "$G" emit "$f" "$T/$id.json" > "$T/$id.out"; then bad "$id glyph: $(cat "$T/$id.out")"; continue; fi
  if ! "$OME" emit "$T/$id.json" "$T/$id.oscir" > "$T/$id.out"; then bad "$id omega_emit: $(cat "$T/$id.out")"; continue; fi
  v="$("$C2" check "$T/$id.oscir" 2>&1 | head -1 || true)"
  case "$v" in checker2=ACCEPT*) n=$((n + 1)) ;; *) bad "$id checker2: $v" ;; esac
done
[ "$n" -eq 10 ] || bad "dev programs accepted $n/10"
echo "dev: $n/10 glyph -> omega_emit -> checker2 ACCEPT"

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
== is not part of Glyph
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
== is not an op, a type or a function
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
EOF
for want in "$T"/r*.want; do
  k="${want%.want}"
  w="$(cat "$want")"
  out="$("$G" emit "$k.bad" "$T/x.json" || true)"
  case "$out" in "REFUSED glyph: "*"$w"*) r=$((r + 1)) ;; *) bad "refusal '$w': got '$out'" ;; esac
  if ! "$G" emit "$k.ok" "$T/x.json" > "$T/x.out" || ! "$OME" emit "$T/x.json" "$T/x.oscir" > "$T/x.out"; then
    bad "control for '$w': $(cat "$T/x.out")"
  else
    v="$("$C2" check "$T/x.oscir" 2>&1 | head -1 || true)"
    case "$v" in checker2=ACCEPT*) ;; *) bad "control for '$w' checker2: $v" ;; esac
  fi
done
echo "refusals: $r/$(ls "$T"/r*.want | wc -l) refused with the expected reason, every control accepted"

# 3. loop guard negative control
sed 's/{"op": "add", "dst": 5, "a": 5, "b": 4}, //' "$T/dev04.json" > "$T/m.json"
if cmp -s "$T/dev04.json" "$T/m.json"; then bad "loop mutant: the counter increment was not found"; fi
if "$OME" emit "$T/m.json" "$T/m.oscir" > "$T/m.out"; then
  v="$("$C2" check "$T/m.oscir" 2>&1 | head -1 || true)"
  case "$v" in *"rule=C2-LOOP-UNBOUNDED"*) echo "loop guard: mutant without the counter increment REJECTED (C2-LOOP-UNBOUNDED)" ;;
    *) bad "loop mutant not rejected: $v" ;; esac
else bad "loop mutant refused earlier than checker2: $(cat "$T/m.out")"; fi

# 4. tokenizer gate
if [ -n "${GLYPH_TOKCOUNT:-}" ] && [ -n "${GLYPH_TOKENIZER:-}" ]; then
  multi="$("$GLYPH_TOKCOUNT" "$GLYPH_TOKENIZER" words $("$G" words) bound | awk '$1 != 1 {print $3}')"
  if [ -n "$multi" ]; then bad "words that are not one token: $multi"; else echo "tokenizer gate: every fixed word is one token"; fi
else
  echo "tokenizer gate: SKIPPED (set GLYPH_TOKCOUNT and GLYPH_TOKENIZER)"
fi

[ "$fails" -eq 0 ] || { echo "test-glyph: $fails failure(s)"; exit 1; }
echo "test-glyph: PASS"
