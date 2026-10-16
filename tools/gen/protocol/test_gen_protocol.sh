#!/usr/bin/env bash
# tools/gen/protocol/test_gen_protocol.sh <builddir> -- tests of the GEN protocol tools (aien-spark/omega#476).
# CPU only; reads only public files of this repository; writes only under <builddir> and a mktemp dir.
#   1. genstats: selftest (Newcombe 1998 Table III, McNemar, Wilson) and byte-identical regeneration of
#      tests/one_layer/generalization/stats_vectors.tsv
#   2. famsig: known answers (canonical forms, the reserved dev family tokens), RULE PASS on the dev META,
#      and mutants (bad shapes and files, R2..R5 violations) that must each be refused
#   3. gencheck: PASS on the real protocol.json, then mutants of the manifest (unknown, duplicate, missing
#      keys, missing or changed splits, duplicate task ids, digest drift, changed constants, bad types) and
#      of the tree (shared signature across splits, extra case directory) that must each be refused; then
#      the reviewer's split check on a SYNTHETIC held-out split (generated here, test-only, not a task) and
#      its mutants
#   4. dev pack: pack_dev.sh twice (byte-identical, sha256 = splits.dev.pack_sha256) and a negative control
#      (one wrong hidden/expected line must refuse)
# GEN_TASKS_CHECK=1 also runs ola.sh tasks-check on the dev pack (needs a clean git tree and built ola tools).
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
B="${1:?usage: test_gen_protocol.sh <builddir>}"
mkdir -p "$B"; B="$(realpath -e "$B")"
T="$(mktemp -d "${TMPDIR:-/tmp}/gen-proto-test.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
CFLAGS="-std=gnu11 -Wall -Wextra -Werror -O2"
G="$ROOT/tests/one_layer/generalization"
P="$G/protocol.json"
ok=0; bad=0
pass() { ok=$((ok + 1)); echo "ok   $1"; }
fail() { bad=$((bad + 1)); echo "FAIL $1"; }

${CC:-cc} $CFLAGS -I"$ROOT/src" -o "$B/famsig" "$ROOT/tools/gen/protocol/famsig.c" "$ROOT/src/sha256.c" || exit 1
${CC:-cc} $CFLAGS -I"$ROOT/src" -o "$B/gencheck" "$ROOT/tools/gen/protocol/gencheck.c" "$ROOT/src/sha256.c" || exit 1
${CC:-cc} $CFLAGS -o "$B/genstats" "$ROOT/tools/gen/protocol/genstats.c" -lm || exit 1
FS="$B/famsig"; GC="$B/gencheck"; GS="$B/genstats"

# ---- 1. statistics ----
if "$GS" selftest | tail -1 | grep -q '^SELFTEST PASS'; then pass "genstats selftest"; else fail "genstats selftest"; fi
if "$GS" vectors | cmp -s - "$G/stats_vectors.tsv"; then pass "stats_vectors.tsv regenerates byte-identical"; else fail "stats_vectors.tsv regenerates byte-identical"; fi

# ---- 2. famsig ----
expect_out() { # name expected command...
  local n="$1" e="$2"; shift 2
  local got; got="$("$@" 2>/dev/null)"
  if [ "$got" = "$e" ]; then pass "$n"; else fail "$n (got '$got')"; fi
}
expect_out "canon flattens seq and sorts" "gs1:seq(array,loop(array,branch(arith)))" "$FS" canon 'seq(loop(branch(arith),array),array)'
expect_out "canon loop(seq(a,b)) = loop(a,b)" "gs1:loop(arith,array)" "$FS" canon 'loop(seq(array,arith))'
expect_out "nesting order matters" "gs1:branch(loop(arith))" "$FS" canon 'branch(loop(arith))'
expect_out "token devgen01 family" "gs1-bd9c105e5ae45397" "$FS" token 'loop(call(nested))'
expect_out "token devgen04 family" "gs1-e05d1c5dffd08b06" "$FS" token 'branch(loop(arith),nested)'
expect_out "token devgen09 family" "gs1-f63e09ec03a454ee" "$FS" token 'loop(loop(branch(arith)))'
expect_out "token devgen07 family" "gs1-30938c5c7504ef0e" "$FS" token 'seq(array,loop(array,branch(arith)))'
if "$FS" rule "$G/dev/META" >/dev/null 2>&1; then pass "famsig rule dev/META: RULE PASS"; else fail "famsig rule dev/META"; fi

refuse() { # name command... : must exit non-zero
  local n="$1"; shift
  if "$@" >/dev/null 2>&1; then fail "mutant not caught: $n"; else pass "refused: $n"; fi
}
refuse "shape unknown primitive" "$FS" canon 'loop(sort)'
refuse "shape leaf with children" "$FS" canon 'arith(loop)'
refuse "shape container without child" "$FS" canon 'loop()'
refuse "shape seq with one child" "$FS" canon 'seq(arith)'
refuse "shape with a space" "$FS" canon 'loop(arith, array)'
refuse "shape deeper than 8" "$FS" canon 'loop(loop(loop(loop(loop(loop(loop(loop(loop(arith)))))))))'
m() { printf '%s\n' "$@" > "$T/m.txt"; echo "$T/m.txt"; }
refuse "meta count mismatch (R1)" "$FS" meta "$(m 'gen-meta 1' 'c1 corpus 2 0 1 0 0 0 loop(arith)')"
refuse "meta heldout line in a public file" "$FS" meta "$(m 'gen-meta 1' 'h1 heldout 1 1 0 0 0 0 branch(arith)')"
refuse "meta bad task id" "$FS" meta "$(m 'gen-meta 1' 'C1 corpus 1 0 1 0 0 0 loop(arith)')"
refuse "meta leading zero count" "$FS" meta "$(m 'gen-meta 1' 'c1 corpus 01 0 1 0 0 0 loop(arith)')"
refuse "meta double space" "$FS" meta "$(m 'gen-meta 1' 'c1  corpus 1 0 1 0 0 0 loop(arith)')"
printf 'gen-meta 1\nc1 corpus 1 0 1 0 0 0 loop(arith)' > "$T/nonl"; refuse "meta no final newline" "$FS" meta "$T/nonl"
printf 'gen-meta 1\nc1 corpus 1 0 1 0 0 0 loop(arith)\n# caf\xc3\xa9\n' > "$T/nonascii"; refuse "meta non-ASCII byte" "$FS" meta "$T/nonascii"
SALT=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
refuse "split ids not ascending" "$FS" split "$(m 'gen-split 1' "salt $SALT" 'h2 heldout 1 1 0 0 0 0 branch(arith)' 'h1 heldout 1 1 0 0 0 0 branch(arith)')"
refuse "split with a comment" "$FS" split "$(m 'gen-split 1' "salt $SALT" '# x' 'h1 heldout 1 1 0 0 0 0 branch(arith)')"
refuse "split bad salt" "$FS" split "$(m 'gen-split 1' 'salt 0123' 'h1 heldout 1 1 0 0 0 0 branch(arith)')"
refuse "split corpus line" "$FS" split "$(m 'gen-split 1' "salt $SALT" 'c1 corpus 1 0 1 0 0 0 loop(arith)')"
printf 'gen-meta 1\nc1 corpus 1 1 1 0 0 0 loop(branch(arith))\nc2 corpus 0 0 0 1 0 0 array\n' > "$T/corpus.ok"
refuse "R2 duplicate id across files" "$FS" rule "$G/dev/META" "$(m 'gen-meta 1' 'devgen01 corpus 1 0 1 0 0 0 loop(arith)')"
refuse "R3 corpus uses a dev signature" "$FS" rule "$G/dev/META" "$(m 'gen-meta 1' 'c9 corpus 0 0 1 0 1 1 loop(call(nested))')"
printf 'gen-split 1\nsalt %s\nh1 heldout 0 0 0 0 1 1 call(nested)\n' "$SALT" > "$T/r4"
refuse "R4 held-out primitive absent from corpus" "$FS" rule "$T/corpus.ok" "$T/r4"
printf 'gen-split 1\nsalt %s\nh1 heldout 0 0 0 2 0 0 seq(array,array)\n' "$SALT" > "$T/r5"
refuse "R5 held-out family with one primitive" "$FS" rule "$T/corpus.ok" "$T/r5"
printf 'gen-split 1\nsalt %s\nh1 heldout 1 1 1 0 0 0 loop(branch(arith))\n' "$SALT" > "$T/r3h"
refuse "R3 held-out shares a corpus signature" "$FS" rule "$T/corpus.ok" "$T/r3h"
printf 'gen-split 1\nsalt %s\nh1 heldout 1 1 1 1 0 0 seq(array,branch(loop(arith)))\n' "$SALT" > "$T/hok"
if "$FS" rule "$T/corpus.ok" "$T/hok" >/dev/null 2>&1; then pass "rule positive control (valid corpus + split)"; else fail "rule positive control"; fi
if ! "$FS" rule "$T/corpus.ok" "$T/hok" 2>/dev/null | grep -q 'seq(array,branch'; then pass "rule hides held-out signatures without --show-heldout"; else fail "rule leaks held-out signatures"; fi

# ---- 3. gencheck ----
if "$GC" manifest "$P" "$ROOT" > "$T/gc.out" 2>&1 && tail -1 "$T/gc.out" | grep -qx 'GENCHECK PASS'; then pass "gencheck manifest protocol.json"; else fail "gencheck manifest protocol.json"; cat "$T/gc.out"; fi

gmut() { # name expected-stderr-substring sed-or-perl-expression [root]
  local n="$1" want="$2" ex="$3" r="${4:-$ROOT}"
  sed -zE "$ex" "$P" > "$T/mut.json"
  if cmp -s "$T/mut.json" "$P"; then fail "mutant did not apply: $n"; return; fi
  if "$GC" manifest "$T/mut.json" "$r" > /dev/null 2> "$T/mut.err"; then fail "mutant not caught: $n"; return; fi
  if grep -qF -- "$want" "$T/mut.err"; then pass "refused: $n"; else fail "refused for another reason: $n ($(cat "$T/mut.err"))"; fi
}
gmut "unknown top-level key" "unknown key extra" 's/^\{\n/{\n  "extra": 1,\n/'
gmut "unknown nested key" "unknown key retrieval.top_n" 's/"k": 3,/"k": 3, "top_n": 3,/'
gmut "duplicate key" "duplicate key \"revision\"" 's/"revision": 1,/"revision": 1, "revision": 1,/'
gmut "missing key" "missing key issue" 's/  "issue": "aien-spark\/omega#476",\n//'
gmut "missing split heldout" "missing key splits.heldout" 's/,\n    "heldout": \{[^}]*\}//'
gmut "missing split dev" "missing key splits.dev" 's/    "dev": \{[^]]*\][^]]*\]\n    \},\n//'
gmut "changed split: heldout count" "splits.heldout.count changed" 's/"count": 60/"count": 59/'
gmut "changed split: min families" "splits.heldout.min_families changed" 's/"min_families": 6/"min_families": 5/'
gmut "changed split: dev task renamed" "devgen11" 's/"devgen10"\]/"devgen11"]/'
gmut "changed split: dev task dropped" "splits.dev.tasks: 9 tasks" 's/, "devgen10"\]/]/'
gmut "duplicate dev task id" "duplicate or unsorted task id devgen09" 's/"devgen10"\]/"devgen09"]/'
gmut "dev family list changed" "splits.dev.families" 's/"gs1:loop\(call\(nested\)\)", //'
gmut "digest drift: pinned file" "digest drift: tools/exp445/PARAMS" 's/("path": "tools\/exp445\/PARAMS", "sha256": ")a/\1b/'
gmut "digest drift: dev META" "digest drift: tests/one_layer/generalization/dev/META" 's/("meta_sha256": ")1/\12/'
gmut "pin removed" "required file tools/exp445/PARAMS is not pinned" 's/    \{"path": "tools\/exp445\/PARAMS"[^\n]*\n//'
gmut "pins out of order" "not strictly ascending" 's/(    \{"path": "docs\/one_layer\/GEN_PROTOCOL_V1.md"[^\n]*\n)(    \{"path": "docs\/one_layer\/OMEGESE.md"[^\n]*\n)/\2\1/'
gmut "unsafe pin path" "unsafe path" 's/"path": "tools\/exp445\/PARAMS"/"path": "..\/omega\/tools\/exp445\/PARAMS"/'
gmut "changed constant: delta" "decision.delta changed" 's/"delta": 0.10/"delta": 0.20/'
gmut "changed constant: k" "retrieval.k changed" 's/"k": 3/"k": 5/'
gmut "changed constant: max_tokens" "generation.max_tokens changed" 's/"max_tokens": 3072/"max_tokens": 4096/'
gmut "changed constant: masking on" "config.masking changed" 's/"masking": false/"masking": true/'
gmut "condition order changed" "conditions[0].name changed" 's/\{"name": "A", "prefix_examples": 1/{"name": "B", "prefix_examples": 1/'
gmut "extra condition" "conditions: 5 elements" 's/(\{"name": "D"[^\n]*)\n/\1,\n    {"name": "E", "prefix_examples": 14, "retrieval": "raw"}\n/'
gmut "wrong type" "revision: type s" 's/"revision": 1/"revision": "1"/'
gmut "null where not allowed" "splits.dev.meta_sha256: type z" 's/"meta_sha256": "[0-9a-f]{64}"/"meta_sha256": null/'
gmut "record set frozen before corpus" "record_sets.raw_manifest_sha256 is set while the corpus META is not frozen" 's/"raw_manifest_sha256": null/"raw_manifest_sha256": "0000000000000000000000000000000000000000000000000000000000000000"/'
gmut "trailing bytes" "trailing bytes" 's/\}\n$/}\n}\n/'
gmut "non-ASCII byte" "ASCII only" 's/"question": "Does/"question": "D\xc3\xb6es/'
gmut "bad escape" "escape" 's/"issue": "aien-spark\/omega#476"/"issue": "aien-spark\\u002fomega#476"/'
gmut "empty array" "empty array" 's/"controls": \["A", "B", "C"\]/"controls": []/'

# tree mutants: a private copy of every file gencheck reads
R="$T/root"
mkdir -p "$R"
(cd "$ROOT" && tar -cf - docs/one_layer/GEN_PROTOCOL_V1.md docs/one_layer/OMEGESE.md tests/one_layer/acceptance/HARNESS_INTERFACE_V2.md \
  tests/one_layer/acceptance/ola.sh tests/one_layer/generalization tools/exp445/PARAMS tools/exp445/arms tools/exp445/reader.sh \
  tools/exp445/sampling-gate.sh tools/exp445/weights.A.sha256 tools/gen/protocol) | (cd "$R" && tar -xf -)
if "$GC" manifest "$P" "$R" >/dev/null 2>&1; then pass "gencheck on the private tree copy"; else fail "gencheck on the private tree copy"; fi
mkdir "$R/tests/one_layer/generalization/dev/tasks/cases/devgen11"
if "$GC" manifest "$P" "$R" >/dev/null 2>"$T/e"; then fail "extra dev case directory not caught"
elif grep -q 'dev cases: 11 directories' "$T/e"; then pass "refused: extra dev case directory"
else fail "extra dev case directory: other reason ($(cat "$T/e"))"; fi
rmdir "$R/tests/one_layer/generalization/dev/tasks/cases/devgen11"
mkdir -p "$R/tests/one_layer/generalization/corpus"
CM="$R/tests/one_layer/generalization/corpus/META"
corpus_json() { # write $T/c.json with the corpus META digest set
  local s; s="$(sha256sum "$CM" | cut -c1-64)"
  sed "s/\"meta_sha256\": null/\"meta_sha256\": \"$s\"/" "$P" > "$T/c.json"
}
printf 'gen-meta 1\nc9 corpus 0 0 1 0 1 1 loop(call(nested))\n' > "$CM"; corpus_json
if "$GC" manifest "$T/c.json" "$R" >/dev/null 2>"$T/e"; then fail "mutant not caught: corpus shares a dev signature"; else grep -q 'R1..R5' "$T/e" && pass "refused: corpus family shared with dev (R3)" || fail "corpus/dev R3: other reason"; fi
printf 'gen-meta 1\ndevgen01 corpus 1 0 1 0 0 0 loop(arith)\n' > "$CM"; corpus_json
if "$GC" manifest "$T/c.json" "$R" >/dev/null 2>"$T/e"; then fail "mutant not caught: corpus reuses a dev task id"; else grep -q 'R1..R5' "$T/e" && pass "refused: duplicate task id across splits (R2)" || fail "R2: other reason"; fi
printf 'gen-meta 1\nc1 dev 1 0 1 0 0 0 loop(arith)\n' > "$CM"; corpus_json
refuse "corpus META with a dev line" "$GC" manifest "$T/c.json" "$R"
# synthetic valid corpus: every primitive, no dev signature
{
  echo 'gen-meta 1'
  echo 'c01 corpus 1 1 1 0 0 0 loop(branch(arith))'
  echo 'c02 corpus 0 0 0 1 0 0 array'
  echo 'c03 corpus 0 0 0 0 1 1 call(nested)'
  echo 'c04 corpus 1 0 1 1 0 0 seq(array,loop(arith))'
} > "$CM"; corpus_json
if "$GC" manifest "$T/c.json" "$R" >/dev/null 2>&1; then pass "gencheck with a frozen synthetic corpus"; else fail "gencheck with a frozen synthetic corpus"; fi
s="$(sha256sum "$CM" | cut -c1-64)"; d="$(printf '%s' "$s" | tr '0-9a-f' '1-9a-f0')"
sed "s/$s/$d/" "$T/c.json" > "$T/c2.json"
refuse "corpus META digest drift" "$GC" manifest "$T/c2.json" "$R"

# reviewer split check on a SYNTHETIC held-out split (test-only)
SP="$T/SPLIT"; PUB="$T/HELDOUT.pub"
shapes='branch(call(nested)) loop(call(arith)) seq(array,branch(nested)) branch(array,loop(nested)) call(loop(branch(arith))) seq(call(array),loop(nested))'
{
  echo 'gen-split 1'; echo "salt $SALT"
  i=0
  for k in $(seq 1 10); do for sh in $shapes; do
    i=$((i + 1)); id="$(printf 'q%02d' "$i")"
    c="$(for p in arith branch loop array call nested; do printf '%s' "$sh" | grep -o "$p" | wc -l; done | tr '\n' ' ' | sed 's/ $//')"
    echo "$id heldout $c $sh"
  done; done
} > "$SP.unsorted"
{ head -2 "$SP.unsorted"; tail -n +3 "$SP.unsorted" | LC_ALL=C sort; } > "$SP"
mkpub() { # tasks-sha split-file
  local out; out="$("$FS" split "$2")" || return 1
  printf 'gen-heldout-pub 1\ntasks_sha256 %s\nsplit_sha256 %s\nfamlist_sha256 %s\ntasks %s\nfamilies %s\n' "$1" \
    "$(echo "$out" | sed -n 's/^split_sha256=//p')" "$(echo "$out" | sed -n 's/^famlist_sha256=//p')" \
    "$(echo "$out" | sed -n 's/^tasks=//p')" "$(echo "$out" | sed -n 's/^families=//p')"
  tail -n +3 "$2" | cut -d' ' -f1 | sed 's/^/task /'
}
TS=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
mkpub "$TS" "$SP" > "$PUB"
if "$GC" split "$T/c.json" "$R" "$SP" "$PUB" > "$T/sp.out" 2>&1 && tail -1 "$T/sp.out" | grep -qx 'GENCHECK PASS'; then pass "gencheck split (synthetic 60 tasks, 6 families)"; else fail "gencheck split positive"; cat "$T/sp.out"; fi
if ! grep -q 'branch(call' "$T/sp.out"; then pass "gencheck split prints no held-out signature"; else fail "gencheck split leaks a held-out signature"; fi
refuse "split check without a frozen corpus" "$GC" split "$P" "$R" "$SP" "$PUB"
sed 's/^famlist_sha256 ./famlist_sha256 0/' "$PUB" > "$T/p1"; cmp -s "$T/p1" "$PUB" && sed -i 's/^famlist_sha256 ./famlist_sha256 1/' "$T/p1"
refuse "HELDOUT.pub famlist digest wrong" "$GC" split "$T/c.json" "$R" "$SP" "$T/p1"
sed '$d' "$PUB" > "$T/p2"; refuse "HELDOUT.pub missing a task line" "$GC" split "$T/c.json" "$R" "$SP" "$T/p2"
sed 's/^families 6$/families 7/' "$PUB" > "$T/p3"; refuse "HELDOUT.pub families count wrong" "$GC" split "$T/c.json" "$R" "$SP" "$T/p3"
sed 's/^task q60$/task q61/' "$PUB" > "$T/p4"; refuse "HELDOUT.pub task id differs" "$GC" split "$T/c.json" "$R" "$SP" "$T/p4"
sed "s/^salt .*/salt $(printf '%s' "$SALT" | tr '0-9a-f' '1-9a-f0')/" "$SP" > "$T/s5"; refuse "SPLIT salt changed after publication" "$GC" split "$T/c.json" "$R" "$T/s5" "$PUB"
head -61 "$SP" > "$T/s6"; mkpub "$TS" "$T/s6" > "$T/p6"; refuse "held-out split with 59 tasks" "$GC" split "$T/c.json" "$R" "$T/s6" "$T/p6"
{ head -2 "$SP"; tail -n +3 "$SP" | sed 's/ 0 0 1 1 1 1 seq(call(array),loop(nested))$/ 0 1 0 0 1 1 branch(call(nested))/'; } > "$T/s7"
mkpub "$TS" "$T/s7" > "$T/p7"; refuse "held-out split with 5 families" "$GC" split "$T/c.json" "$R" "$T/s7" "$T/p7"
{ head -2 "$SP"; tail -n +3 "$SP" | sed 's/^q01 .*/q01 heldout 1 1 1 0 0 0 loop(branch(arith))/'; } > "$T/s8"
mkpub "$TS" "$T/s8" > "$T/p8"; refuse "held-out family shared with corpus (R3)" "$GC" split "$T/c.json" "$R" "$T/s8" "$T/p8"
sed "s/\"tasks_sha256\": null/\"tasks_sha256\": \"$(printf '%s' "$TS" | tr a b)\"/" "$T/c.json" > "$T/c3.json"
refuse "HELDOUT.pub tasks_sha256 differs from protocol" "$GC" split "$T/c3.json" "$R" "$SP" "$PUB"

# ---- 4. dev pack ----
PK="$ROOT/tests/one_layer/generalization/dev/pack_dev.sh"
if bash "$PK" "$T/pack1" >/dev/null && bash "$PK" "$T/pack2" >/dev/null && cmp -s "$T/pack1/tasks.tar" "$T/pack2/tasks.tar"; then pass "dev pack reproducible"; else fail "dev pack reproducible"; fi
want="$(sed -n 's/.*"pack_sha256": "\([0-9a-f]\{64\}\)".*/\1/p' "$P")"
if [ "$(cut -c1-64 "$T/pack1/TASKS.sha256")" = "$want" ]; then pass "dev pack sha256 = splits.dev.pack_sha256"; else fail "dev pack sha256 differs from protocol.json"; fi
cp -r "$ROOT/tests/one_layer/generalization/dev" "$T/devneg"
sed -i '1s/.*/1/' "$T/devneg/tasks/cases/devgen04/hidden/expected"
if bash "$T/devneg/pack_dev.sh" "$T/packneg" >/dev/null 2>&1; then fail "wrong hidden/expected not refused"; else pass "refused: dev pack with one wrong expected line (oracle check)"; fi
if [ "${GEN_TASKS_CHECK:-0}" = 1 ]; then
  if bash "$ROOT/tests/one_layer/acceptance/ola.sh" tasks-check "$T/pack1/tasks.tar" "$T/pack1/TASKS.sha256" 2>"$T/tc.err" | grep -q 'RUN VALID'; then pass "ola.sh tasks-check dev pack: RUN VALID"; else fail "ola.sh tasks-check dev pack"; fi
fi

echo "test-gen-protocol: $ok ok, $bad failed"
[ "$bad" = 0 ]
