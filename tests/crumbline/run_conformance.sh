#!/bin/bash
# Crumb v1 conformance: Omega's visible-crumb reader must decode every vector
# produced by the crumbs crate (aien-sovereign-core, crates/crumbs) exactly as
# crumbs itself parses it. Vectors: `crumbs vectors DIR` (one per registered
# family, seed 7) plus expected.txt.
#
# Also checks that malformed bytes are rejected and that the learner binary
# carries no sealed-side symbols.
set -u
LEARNER=${1:-build/crumbline-learner}
DIR=${2:-tests/crumbline/vectors}
pass=0
fail=0
while read -r name rest; do
    got=$("$LEARNER" --decode "$DIR/$name")
    if [ "$got" == "$rest" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "MISMATCH $name"
        echo "  want: $rest"
        echo "  got:  $got"
    fi
done < "$DIR/expected.txt"

# Negative vectors: truncated, wrong magic, trailing byte, non-canonical decimal.
tmp=$(mktemp -d)
head -c 20 "$DIR/v001.crb" > "$tmp/trunc.crb"
printf 'XRB1' | cat - <(tail -c +5 "$DIR/v001.crb") > "$tmp/magic.crb"
cat "$DIR/v001.crb" <(printf '\0') > "$tmp/trail.crb"
printf 'CRB1\x01\x00\x01\x00\x01\x01\x00\x00\x01\x00\x00\x00\x01\x00\x00\x001\x02\x00\x00\x0003' > "$tmp/zero.crb"
for bad in trunc magic trail zero; do
    if "$LEARNER" --decode "$tmp/$bad.crb" | grep -q '^decode-error'; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        echo "ACCEPTED MALFORMED $bad"
    fi
done
rm -rf "$tmp"

# The learner links no generator, sealed record or gate code.
if nm "$LEARNER" | grep -Eiq 'sealed|heldout|generator|nvrm|_gates'; then
    fail=$((fail + 1))
    echo "LEARNER LINKS SEALED OR GATE SYMBOLS"
else
    pass=$((pass + 1))
fi

echo "CRUMBLINE_CONFORMANCE pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
