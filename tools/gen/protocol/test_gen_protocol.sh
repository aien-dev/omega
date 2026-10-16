#!/usr/bin/env bash
# tools/gen/protocol/test_gen_protocol.sh <builddir> -- tests of the GEN protocol tools (aien-spark/omega#476).
# CPU only; reads only public files of this repository; writes only under <builddir> and a mktemp dir.
#   1. genstats: selftest (Newcombe 1998 Table III, McNemar, Wilson), byte-identical regeneration of
#      tests/one_layer/generalization/stats_vectors.tsv, length-match classes; genjac known answers
#   2. famsig: known answers (canonical forms, the reserved dev family tokens), RULE PASS on the dev META,
#      coarse forms, mutants (bad shapes and files, R2..R7 violations) that must each be refused, and
#      famsig compare (blind re-declaration)
#   3. gencheck: PASS on the real protocol.json, then mutants of the manifest (unknown, duplicate, missing
#      keys, missing or changed splits, duplicate task ids, digest drift, changed constants, bad types) and
#      of the tree (shared signature across splits, extra case directory) that must each be refused; then
#      the reviewer's split check on a SYNTHETIC held-out split (generated here, test-only, not a task) and
#      its mutants (including fail-closed on null digests); run-ready and open on a fully frozen SYNTHETIC
#      manifest (interface files taken from git at their pinned commits) and their mutants
#   4. dev pack: pack_dev.sh twice (byte-identical, sha256 = splits.dev.pack_sha256) and a negative control
#      (one wrong hidden/expected line must refuse)
# GEN_TASKS_CHECK=1 also runs ola.sh tasks-check on the dev pack (needs a clean git tree and built ola tools)
# and then removes the run and receipt entries that tasks-check added under evidence/ONE-LAYER-2.
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
expect_out() { # name expected command...
  local n="$1" e="$2"; shift 2
  local got; got="$("$@" 2>/dev/null)"
  if [ "$got" = "$e" ]; then pass "$n"; else fail "$n (got '$got')"; fi
}
refuse() { # name command... : must exit non-zero
  local n="$1"; shift
  if "$@" >/dev/null 2>&1; then fail "mutant not caught: $n"; else pass "refused: $n"; fi
}
refuse_why() { # name expected-output-substring command... : must exit non-zero and say why
  local n="$1" w="$2"; shift 2
  if "$@" > "$T/rw.out" 2>&1; then fail "mutant not caught: $n"; elif grep -qF -- "$w" "$T/rw.out"; then pass "refused: $n"; else fail "refused for another reason: $n ($(tail -2 "$T/rw.out" | tr "\n" " "))"; fi
}

${CC:-cc} $CFLAGS -I"$ROOT/src" -o "$B/famsig" "$ROOT/tools/gen/protocol/famsig.c" "$ROOT/src/sha256.c" || exit 1
${CC:-cc} $CFLAGS -I"$ROOT/src" -o "$B/gencheck" "$ROOT/tools/gen/protocol/gencheck.c" "$ROOT/src/sha256.c" || exit 1
${CC:-cc} $CFLAGS -o "$B/genstats" "$ROOT/tools/gen/protocol/genstats.c" -lm || exit 1
${CC:-cc} $CFLAGS -o "$B/genjac" "$ROOT/tools/gen/protocol/genjac.c" || exit 1
FS="$B/famsig"; GC="$B/gencheck"; GS="$B/genstats"; JC="$B/genjac"

# ---- 1. statistics ----
if "$GS" selftest | tail -1 | grep -q '^SELFTEST PASS'; then pass "genstats selftest"; else fail "genstats selftest"; fi
if "$GS" vectors | cmp -s - "$G/stats_vectors.tsv"; then pass "stats_vectors.tsv regenerates byte-identical"; else fail "stats_vectors.tsv regenerates byte-identical"; fi

expect_out "lenmatch reviewer fixture 295 vs 945: unmatched" "task 1 target_bytes=295 suffix_bytes=945 length_gap=650 tolerance_bytes=256.00 class=unmatched" sh -c "\"$GS\" lenmatch 295:945 | head -1"
expect_out "lenmatch 256 B boundary: matched" "tasks=2 matched=1 unmatched=1 status=AVAILABLE" sh -c "\"$GS\" lenmatch 300:556 300:557 | tail -1"
expect_out "lenmatch 15% boundary, C shorter: matched" "tasks=2 matched=1 unmatched=1 status=AVAILABLE" sh -c "\"$GS\" lenmatch 2000:1700 2000:1699 | tail -1"
expect_out "lenmatch 4 of 10 matched: UNAVAILABLE" "tasks=10 matched=4 unmatched=6 status=UNAVAILABLE" sh -c "\"$GS\" lenmatch 300:300 300:301 300:302 300:303 300:945 300:946 300:947 300:948 300:949 300:950 | tail -1"
expect_out "lenmatch 5 of 10 matched: AVAILABLE" "tasks=10 matched=5 unmatched=5 status=AVAILABLE" sh -c "\"$GS\" lenmatch 300:300 300:301 300:302 300:303 300:304 300:946 300:947 300:948 300:949 300:950 | tail -1"
if grep -q $'^lenset\ts03\t10\t4\t6\tUNAVAILABLE$' "$G/stats_vectors.tsv" && grep -q $'^lenmatch\tl03\t300\t556\t256\t256.00\tmatched$' "$G/stats_vectors.tsv"; then pass "stats_vectors.tsv carries lenmatch and lenset records"; else fail "stats_vectors.tsv lenmatch/lenset records"; fi
refuse "lenmatch bad pair" "$GS" lenmatch 300-556
# genjac: word 3-gram Jaccard
printf 'a b c d\n' > "$T/j1"; printf 'a b c e\n' > "$T/j2"; printf 'A, b; c d e\n' > "$T/j3"; printf 'a b c d f\n' > "$T/j4"; printf 'x y\n' > "$T/j5"
expect_out "genjac pair 1/3" "grams_a=2 grams_b=2 inter=1 union=3 jaccard=0.333333" "$JC" pair "$T/j1" "$T/j2"
expect_out "genjac pair 0.5 exact, case and punctuation folded" "grams_a=3 grams_b=3 inter=2 union=4 jaccard=0.500000" "$JC" pair "$T/j3" "$T/j4"
expect_out "genjac pair identical" "grams_a=2 grams_b=2 inter=2 union=2 jaccard=1.000000" "$JC" pair "$T/j1" "$T/j1"
expect_out "genjac pair under 3 words" "grams_a=0 grams_b=2 inter=0 union=2 jaccard=0.000000" "$JC" pair "$T/j5" "$T/j1"
printf '%s\n' "$T/j3" > "$T/hl"; printf '%s\n' "$T/j4" "$T/j5" > "$T/cl"
expect_out "genjac scan: 0.5 is not above 0.5" "heldout=1 corpus=2 threshold=0.5 max_jaccard=0.500000 above=0" "$JC" scan "$T/hl" "$T/cl"
if "$JC" scan --threshold 0.49 "$T/hl" "$T/cl" > "$T/jc.out"; then fail "genjac scan above threshold must exit 1"; elif grep -qx 'heldout=1 corpus=2 threshold=0.49 max_jaccard=0.500000 above=1' "$T/jc.out" && ! grep -q "$T/j3" "$T/jc.out"; then pass "genjac scan above=1 exits 1 and names no file without --show"; else fail "genjac scan above=1 output ($(cat "$T/jc.out"))"; fi
expect_out "genjac scan --show names the pair" "above $T/j3 $T/j4 0.500000" sh -c "\"$JC\" scan --threshold 0.49 --show \"$T/hl\" \"$T/cl\" | head -1"
"$JC" scan --threshold 1.5 "$T/hl" "$T/cl" >/dev/null 2>&1; [ $? = 2 ] && pass "genjac refuses threshold > 1 (exit 2)" || fail "genjac threshold > 1"
"$JC" scan --threshold 0.1234567 "$T/hl" "$T/cl" >/dev/null 2>&1; [ $? = 2 ] && pass "genjac refuses 7 fraction digits (exit 2)" || fail "genjac 7 fraction digits"
printf '%s\n\n' "$T/j3" > "$T/hlb"; "$JC" scan "$T/hlb" "$T/cl" >/dev/null 2>&1; [ $? = 2 ] && pass "genjac refuses a blank list line (exit 2)" || fail "genjac blank list line"
# ---- 2. famsig ----
expect_out "canon flattens seq and sorts" "gs1:seq(array,loop(array,branch(arith)))" "$FS" canon 'seq(loop(branch(arith),array),array)'
expect_out "canon loop(seq(a,b)) = loop(a,b)" "gs1:loop(arith,array)" "$FS" canon 'loop(seq(array,arith))'
expect_out "nesting order matters" "gs1:branch(loop(arith))" "$FS" canon 'branch(loop(arith))'
expect_out "token devgen01 family" "gs1-bd9c105e5ae45397" "$FS" token 'loop(call(nested))'
expect_out "token devgen04 family" "gs1-e05d1c5dffd08b06" "$FS" token 'branch(loop(arith),nested)'
expect_out "token devgen09 family" "gs1-f63e09ec03a454ee" "$FS" token 'loop(loop(branch(arith)))'
expect_out "token devgen07 family" "gs1-30938c5c7504ef0e" "$FS" token 'seq(array,loop(array,branch(arith)))'
if "$FS" rule "$G/dev/META" >/dev/null 2>&1; then pass "famsig rule dev/META: RULE PASS"; else fail "famsig rule dev/META"; fi

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
expect_out "coarse: nested read as arith" "gc1:loop(branch(arith))" "$FS" coarse 'loop(branch(nested))'
expect_out "coarse: duplicate siblings collapsed" "gc1:loop(branch(arith))" "$FS" coarse 'loop(branch(arith,arith))'
expect_out "coarse: top-level seq wrapper with leaves dropped" "gc1:loop(branch(arith))" "$FS" coarse 'seq(arith,loop(branch(arith)))'
expect_out "coarse: leaf inside a container kept" "gc1:loop(arith,branch(arith))" "$FS" coarse 'loop(branch(arith),arith)'
expect_out "coarse: two containers keep the seq" "gc1:seq(call(array),loop(arith))" "$FS" coarse 'seq(call(array),loop(nested))'
expect_out "coarse devgen family branch(loop(arith),nested)" "gc1:branch(arith,loop(arith))" "$FS" coarse 'branch(loop(arith),nested)'
printf 'gen-split 1\nsalt %s\nh1 heldout 0 1 1 0 0 1 loop(branch(nested))\n' "$SALT" > "$T/r6"
refuse_why "R6 held-out = corpus family with nested for arith" "FAIL R6" "$FS" rule "$T/corpus.ok" "$T/r6"
printf 'gen-split 1\nsalt %s\nh1 heldout 2 1 1 0 0 0 seq(arith,loop(branch(arith)))\n' "$SALT" > "$T/r6b"
refuse_why "R6 held-out = corpus family inside a seq wrapper" "FAIL R6" "$FS" rule "$T/corpus.ok" "$T/r6b"
if ! "$FS" rule "$T/corpus.ok" "$T/r6" 2>/dev/null | grep -q 'gc1:loop'; then pass "R6 failure hides the held-out coarse signature"; else fail "R6 failure leaks the held-out coarse signature"; fi
refuse_why "R6 corpus = dev family with arith for nested" "FAIL R6" "$FS" rule "$G/dev/META" "$(m 'gen-meta 1' 'c9 corpus 1 0 1 0 1 0 loop(call(arith))')"
printf 'gen-meta 1\nc1 corpus 1 1 1 0 0 0 loop(branch(arith))\nc2 corpus 0 0 0 1 0 0 array\nc3 corpus 1 0 1 1 0 0 seq(array,loop(arith))\n' > "$T/corpus.ok2"
printf 'gen-split 1\nsalt %s\nh1 heldout 2 1 1 0 0 0 loop(branch(arith),arith)\n' "$SALT" > "$T/r7"
refuse_why "R7 held-out with no novel coarse edge" "FAIL R7" "$FS" rule "$T/corpus.ok2" "$T/r7"
if "$FS" rule "$T/corpus.ok" "$T/r7" >/dev/null 2>&1; then pass "R6/R7 known limit: loop(branch(arith),arith) passes when loop>arith is new"; else fail "R7 limit control"; fi
# blind re-declaration
printf 'gen-split 1\nsalt %s\nh1 heldout 1 1 1 1 0 0 seq(array,branch(loop(arith)))\nh2 heldout 0 0 1 1 1 0 loop(call(array))\n' "$SALT" > "$T/cmpA"
printf 'gen-split 1\nsalt %s\nh1 heldout 1 1 1 1 0 0 seq(branch(loop(arith)),array)\nh2 heldout 0 0 1 1 1 0 loop(call(array))\n' "$(printf '%s' "$SALT" | tr '0-9a-f' '1-9a-f0')" > "$T/cmpB"
expect_out "compare: same families, other salt and sibling order" "tasks=2 mismatches=0" "$FS" compare "$T/cmpA" "$T/cmpB"
sed 's/^h2 .*/h2 heldout 0 0 1 1 1 0 call(loop(array))/' "$T/cmpB" > "$T/cmpC"
if "$FS" compare "$T/cmpA" "$T/cmpC" > "$T/cmp.out" 2>&1; then fail "compare mismatch must exit 1"; elif [ "$(cat "$T/cmp.out")" = "tasks=2 mismatches=1" ]; then pass "compare: one mismatch, exit 1, no signature printed"; else fail "compare mismatch output ($(cat "$T/cmp.out"))"; fi
sed 's/^h2 /h3 /' "$T/cmpB" > "$T/cmpD"; refuse "compare: different task ids" "$FS" compare "$T/cmpA" "$T/cmpD"
head -3 "$T/cmpB" > "$T/cmpE"; refuse "compare: different task counts" "$FS" compare "$T/cmpA" "$T/cmpE"

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
gmut "changed constant: floor_max" "decision.floor_max changed" 's/"floor_max": 3/"floor_max": 4/'
gmut "changed constant: G0 seed s8" "gates.g0.seed_values[7] changed" 's/476908\]/476909]/'
gmut "G0 seed list shortened" "missing frozen value gates.g0.seed_values[7]" 's/, 476908\]/]/'
gmut "changed constant: length-match tolerance" "retrieval.length_match_frac changed" 's/"length_match_frac": 0.15/"length_match_frac": 0.25/'
gmut "changed constant: near-duplicate threshold" "splits.heldout.near_dup_threshold changed" 's/"near_dup_threshold": 0.5/"near_dup_threshold": 0.6/'
gmut "changed constant: one request in flight" "generation.in_flight changed" 's/"in_flight": 1/"in_flight": 4/'
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
if "$GC" manifest "$T/c.json" "$R" >/dev/null 2>"$T/e"; then fail "mutant not caught: corpus shares a dev signature"; else grep -qF 'R1..R7' "$T/e" && pass "refused: corpus family shared with dev (R3)" || fail "corpus/dev R3: other reason"; fi
printf 'gen-meta 1\ndevgen01 corpus 1 0 1 0 0 0 loop(arith)\n' > "$CM"; corpus_json
if "$GC" manifest "$T/c.json" "$R" >/dev/null 2>"$T/e"; then fail "mutant not caught: corpus reuses a dev task id"; else grep -qF 'R1..R7' "$T/e" && pass "refused: duplicate task id across splits (R2)" || fail "R2: other reason"; fi
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
shapes='branch(call(nested)) loop(call(array)) seq(array,branch(loop(nested))) branch(array,loop(nested)) call(loop(branch(arith))) seq(call(array),loop(nested))'
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
anchor() { # in-json out-json pub-file tasks-sha: anchor the held-out digests in a manifest copy
  sed "s/\"pub_sha256\": null/\"pub_sha256\": \"$(sha256sum "$3" | cut -c1-64)\"/; s/\"tasks_sha256\": null/\"tasks_sha256\": \"$4\"/" "$1" > "$2"
}
spl() { # SPLIT HELDOUT.pub: gencheck split with the digests of that HELDOUT.pub anchored
  anchor "$T/c.json" "$T/cs.json" "$2" "$TS"
  "$GC" split "$T/cs.json" "$R" "$1" "$2"
}
if spl "$SP" "$PUB" > "$T/sp.out" 2>&1 && tail -1 "$T/sp.out" | grep -qx 'GENCHECK PASS'; then pass "gencheck split (synthetic 60 tasks, 6 families, R1..R7)"; else fail "gencheck split positive"; cat "$T/sp.out"; fi
if ! grep -q 'branch(call' "$T/sp.out"; then pass "gencheck split prints no held-out signature"; else fail "gencheck split leaks a held-out signature"; fi
refuse_why "split check with null held-out digests (fail closed)" "fail closed on null" "$GC" split "$T/c.json" "$R" "$SP" "$PUB"
anchor "$T/c.json" "$T/cn.json" "$PUB" "$TS"; sed -i 's/"tasks_sha256": "a\{64\}"/"tasks_sha256": null/' "$T/cn.json"
refuse_why "split check with only tasks_sha256 null (fail closed)" "fail closed on null" "$GC" split "$T/cn.json" "$R" "$SP" "$PUB"
anchor "$P" "$T/pn.json" "$PUB" "$TS"; refuse "split check without a frozen corpus" "$GC" split "$T/pn.json" "$R" "$SP" "$PUB"
sed 's/^famlist_sha256 ./famlist_sha256 0/' "$PUB" > "$T/p1"; cmp -s "$T/p1" "$PUB" && sed -i 's/^famlist_sha256 ./famlist_sha256 1/' "$T/p1"
refuse_why "HELDOUT.pub famlist digest wrong" "famlist" spl "$SP" "$T/p1"
sed '$d' "$PUB" > "$T/p2"; refuse "HELDOUT.pub missing a task line" spl "$SP" "$T/p2"
sed 's/^families 6$/families 7/' "$PUB" > "$T/p3"; refuse "HELDOUT.pub families count wrong" spl "$SP" "$T/p3"
sed 's/^task q60$/task q61/' "$PUB" > "$T/p4"; refuse "HELDOUT.pub task id differs" spl "$SP" "$T/p4"
sed "s/^salt .*/salt $(printf '%s' "$SALT" | tr '0-9a-f' '1-9a-f0')/" "$SP" > "$T/s5"; refuse "SPLIT salt changed after publication" spl "$T/s5" "$PUB"
head -61 "$SP" > "$T/s6"; mkpub "$TS" "$T/s6" > "$T/p6"; refuse "held-out split with 59 tasks" spl "$T/s6" "$T/p6"
{ head -2 "$SP"; tail -n +3 "$SP" | sed 's/ 0 0 1 1 1 1 seq(call(array),loop(nested))$/ 0 1 0 0 1 1 branch(call(nested))/'; } > "$T/s7"
mkpub "$TS" "$T/s7" > "$T/p7"; refuse_why "held-out split with 5 families" "famil" spl "$T/s7" "$T/p7"
{ head -2 "$SP"; tail -n +3 "$SP" | sed 's/^q01 .*/q01 heldout 1 1 1 0 0 0 loop(branch(arith))/'; } > "$T/s8"
mkpub "$TS" "$T/s8" > "$T/p8"; refuse_why "held-out family shared with corpus (R3)" "FAIL R3" spl "$T/s8" "$T/p8"
{ head -2 "$SP"; tail -n +3 "$SP" | sed 's/^q01 .*/q01 heldout 0 1 1 0 0 1 loop(branch(nested))/'; } > "$T/s9"
mkpub "$TS" "$T/s9" > "$T/p9"; refuse_why "held-out coarse family shared with corpus (R6)" "FAIL R6" spl "$T/s9" "$T/p9"
{ head -2 "$SP"; tail -n +3 "$SP" | sed 's/^q01 .*/q01 heldout 2 1 1 0 0 0 loop(branch(arith),arith)/'; } > "$T/s10"
mkpub "$TS" "$T/s10" > "$T/p10"; refuse_why "held-out family with no novel coarse edge (R7)" "FAIL R7" spl "$T/s10" "$T/p10"
anchor "$T/c.json" "$T/c3.json" "$PUB" "$(printf '%s' "$TS" | tr a b)"
refuse_why "HELDOUT.pub tasks_sha256 differs from protocol" "tasks_sha256 mismatch" "$GC" split "$T/c3.json" "$R" "$SP" "$PUB"
anchor "$T/c.json" "$T/c4.json" "$PUB" "$TS"; sed -i 's/"pub_sha256": "\([0-9a-f]\)/"pub_sha256": "f\1/; s/"pub_sha256": "\([0-9a-f]\{64\}\)[0-9a-f]"/"pub_sha256": "\1"/' "$T/c4.json"
refuse_why "HELDOUT.pub differs from the anchored pub_sha256" "digest drift: HELDOUT.pub" "$GC" split "$T/c4.json" "$R" "$SP" "$PUB"

# run-ready and seal-open on a fully frozen SYNTHETIC manifest (test-only values)
refuse_why "run-ready on the committed protocol.json (nulls pending)" "GENCHECK FAIL" "$GC" run-ready "$P" "$ROOT"
{
  echo 'gen-meta 1'
  echo 'c01 corpus 1 1 1 0 0 0 loop(branch(arith))'
  echo 'c02 corpus 0 0 0 1 0 0 array'
  echo 'c03 corpus 0 0 0 0 1 1 call(nested)'
  echo 'c04 corpus 1 0 1 1 0 0 seq(array,loop(arith))'
  for i in 05 06 07 08 09 10 11 12 13 14; do echo "c$i corpus 1 1 1 0 0 0 loop(branch(arith))"; done
} > "$CM"; corpus_json
mkdir -p "$R/records"; printf 'raw manifest (test)\n' > "$R/records/MANIFEST.C"; printf 'abstraction manifest (test)\n' > "$R/records/MANIFEST.D"
ifok=1
for ic in 937b033c78d087f1a0c64f28a29f4bab69f0a18a:tools/gen/abstract/RECORD_SCHEMA.md 9d81b78fb10cbc57d1178df0def8acb992927318:tools/gen/runner/FORMATS.md a26be5771acd2ca731d98ab94de51d24b4ffe300:tools/gen/retrieval/CONTRACT.md; do
  mkdir -p "$R/$(dirname "${ic#*:}")"; git -C "$ROOT" show "$ic" > "$R/${ic#*:}" 2>/dev/null || ifok=0
done
[ "$ifok" = 1 ] && pass "interface files available from git at their pinned commits" || fail "interface files not available from git (fetch origin)"
ids="$(for i in 01 02 03 04 05 06 07 08 09 10 11 12 13 14; do printf '"c%s", ' "$i"; done | sed 's/, $//')"
hC="$(sha256sum "$R/records/MANIFEST.C" | cut -c1-64)"; hD="$(sha256sum "$R/records/MANIFEST.D" | cut -c1-64)"
sed -E "s|\"record_sets\": \{[^}]*\}|\"record_sets\": {\"prefix_ids\": [$ids], \"prefix_sha256\": \"$(printf '%064d' 7)\", \"raw_manifest_sha256\": \"$hC\", \"abstraction_manifest_sha256\": \"$hD\", \"raw_manifest\": \"records/MANIFEST.C\", \"abstraction_manifest\": \"records/MANIFEST.D\"}|" "$T/c.json" > "$T/f0.json"
anchor "$T/f0.json" "$T/F.json" "$PUB" "$TS"
if "$GC" run-ready "$T/F.json" "$R" > "$T/rr.out" 2>&1 && tail -1 "$T/rr.out" | grep -qx 'GENCHECK PASS'; then pass "run-ready on a fully frozen synthetic manifest"; else fail "run-ready positive"; tail -3 "$T/rr.out"; fi
fmut() { # name expected sed-expression [cmd]: mutate F.json, expect run-ready refusal with that reason
  sed -zE "$3" "$T/F.json" > "$T/fm.json"
  if cmp -s "$T/fm.json" "$T/F.json"; then fail "mutant did not apply: $1"; return; fi
  refuse_why "$1" "$2" "$GC" "${4:-run-ready}" "$T/fm.json" "$R"
}
fmut "run-ready with prefix_sha256 null" "not run-ready: record_sets.prefix_sha256 is null" 's/"prefix_sha256": "[0-9]{64}"/"prefix_sha256": null/'
fmut "run-ready without the genret-cli-v3 interface" "retrieval contract genret-cli-v3 is not pinned" 's/,\n?    \{"name": "genret-cli-v3"[^}]*\}//'
fmut "record-set path set, digest null" "must both be null or both be set" 's/"raw_manifest_sha256": "[0-9a-f]{64}"/"raw_manifest_sha256": null/' manifest
fmut "record-set manifest digest drift" "digest drift: record-set manifest records/MANIFEST.D" "s/\"abstraction_manifest_sha256\": \"$hD\"/\"abstraction_manifest_sha256\": \"$hC\"/" manifest
fmut "record-set manifest path missing" "record-set manifest missing" 's|"raw_manifest": "records/MANIFEST.C"|"raw_manifest": "records/MANIFEST.X"|' manifest
fmut "prefix id not a corpus task" "is not a corpus task" 's/"c14"\]/"c99"]/' manifest
mv "$R/tools/gen/retrieval/CONTRACT.md" "$T/contract.keep"
refuse_why "run-ready with an interface file missing from the tree" "is not in this tree" "$GC" run-ready "$T/F.json" "$R"
if "$GC" manifest "$T/F.json" "$R" 2>/dev/null | grep -q 'pending merge'; then pass "manifest mode still allows a pending interface (run-ready does not)"; else fail "manifest mode pending interface"; fi
mv "$T/contract.keep" "$R/tools/gen/retrieval/CONTRACT.md"
mkopen() { # protocol-sha tasks-sha pub-sha > OPEN
  printf 'gen-open 1\nomega_commit %s\nprotocol_sha256 %s\ntasks_sha256 %s\nheldout_pub_sha256 %s\n' "$(printf '%040d' 1)" "$1" "$2" "$3"
}
hF="$(sha256sum "$T/F.json" | cut -c1-64)"; hP="$(sha256sum "$PUB" | cut -c1-64)"
mkopen "$hF" "$TS" "$hP" > "$T/OPEN"
if "$GC" open "$T/F.json" "$R" "$T/OPEN" > "$T/op.out" 2>&1 && tail -1 "$T/op.out" | grep -qx 'GENCHECK PASS'; then pass "gencheck open: OPEN receipt matches the frozen manifest"; else fail "gencheck open positive"; tail -3 "$T/op.out"; fi
mkopen "$(printf '%s' "$hF" | tr '0-9a-f' '1-9a-f0')" "$TS" "$hP" > "$T/O1"; refuse_why "OPEN with another protocol.json sha256" "protocol_sha256" "$GC" open "$T/F.json" "$R" "$T/O1"
mkopen "$hF" "$(printf '%s' "$TS" | tr a b)" "$hP" > "$T/O2"; refuse_why "OPEN with another tasks_sha256" "OPEN: tasks_sha256 differs" "$GC" open "$T/F.json" "$R" "$T/O2"
mkopen "$hF" "$TS" "$(printf '%s' "$hP" | tr '0-9a-f' '1-9a-f0')" > "$T/O3"; refuse_why "OPEN with another heldout_pub_sha256" "OPEN: heldout_pub_sha256 differs" "$GC" open "$T/F.json" "$R" "$T/O3"
head -4 "$T/OPEN" > "$T/O4"; refuse_why "OPEN with 4 lines" "exactly 5 lines" "$GC" open "$T/F.json" "$R" "$T/O4"
sed 's/^gen-open 1$/gen-open 2/' "$T/OPEN" > "$T/O5"; refuse_why "OPEN with another header" "exactly 5 lines" "$GC" open "$T/F.json" "$R" "$T/O5"
sed -E 's/"prefix_sha256": "[0-9]{64}"/"prefix_sha256": null/' "$T/F.json" > "$T/Fn.json"; mkopen "$(sha256sum "$T/Fn.json" | cut -c1-64)" "$TS" "$hP" > "$T/O6"
refuse_why "OPEN over a manifest with a null digest" "is null" "$GC" open "$T/Fn.json" "$R" "$T/O6"

# ---- 4. dev pack ----
PK="$ROOT/tests/one_layer/generalization/dev/pack_dev.sh"
if bash "$PK" "$T/pack1" >/dev/null && bash "$PK" "$T/pack2" >/dev/null && cmp -s "$T/pack1/tasks.tar" "$T/pack2/tasks.tar"; then pass "dev pack reproducible"; else fail "dev pack reproducible"; fi
want="$(sed -n 's/.*"pack_sha256": "\([0-9a-f]\{64\}\)".*/\1/p' "$P")"
if [ "$(cut -c1-64 "$T/pack1/TASKS.sha256")" = "$want" ]; then pass "dev pack sha256 = splits.dev.pack_sha256"; else fail "dev pack sha256 differs from protocol.json"; fi
cp -r "$ROOT/tests/one_layer/generalization/dev" "$T/devneg"
sed -i '1s/.*/1/' "$T/devneg/tasks/cases/devgen04/hidden/expected"
if bash "$T/devneg/pack_dev.sh" "$T/packneg" >/dev/null 2>&1; then fail "wrong hidden/expected not refused"; else pass "refused: dev pack with one wrong expected line (oracle check)"; fi
if [ "${GEN_TASKS_CHECK:-0}" = 1 ]; then
  # ola.sh always writes evidence/ONE-LAYER-2/{runs,receipts}; OLA_EVIDENCE_ROOT would redirect it but forces
  # RUN INVALID (ola.sh line 27), so the test removes exactly the entries this run added, nothing else.
  EV="$ROOT/evidence/ONE-LAYER-2"
  ls -1 "$EV/runs" "$EV/receipts" 2>/dev/null | LC_ALL=C sort > "$T/ev.before"
  if bash "$ROOT/tests/one_layer/acceptance/ola.sh" tasks-check "$T/pack1/tasks.tar" "$T/pack1/TASKS.sha256" 2>"$T/tc.err" | grep -q 'RUN VALID'; then pass "ola.sh tasks-check dev pack: RUN VALID"; else fail "ola.sh tasks-check dev pack"; fi
  for d in runs receipts; do
    for e in $(ls -1 "$EV/$d" 2>/dev/null); do grep -qxF "$e" "$T/ev.before" || rm -rf "${EV:?}/$d/$e"; done
  done
fi

echo "test-gen-protocol: $ok ok, $bad failed"
[ "$bad" = 0 ]
