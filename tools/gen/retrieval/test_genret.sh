#!/bin/sh
# tools/gen/retrieval/test_genret.sh <genret binary> <recheck binary>: tests of the GEN-4 file-backed retrieval
# provider (CONTRACT.md, genret-cli-v2) on the SYNTHETIC record set tools/gen/retrieval/fixtures/corpus/
# (Agent D's record schema v1, written by fixtures/make_fixtures.sh) and the fixture queries fixtures/tasks/.
# CPU only, no network, no hidden data. Scores are re-derived by an independent sh + bc implementation
# (tokens with tr, counts with grep, integer BM25 and the fixed-point log2 in bc) and the suffix by an
# independent sh renderer.
set -eu
G="$1" RC="$2"
case "$G" in /*) ;; *) G="$(pwd)/$G" ;; esac
case "$RC" in /*) ;; *) RC="$(pwd)/$RC" ;; esac
HERE="$(cd "$(dirname "$0")" && pwd)"
FX="$HERE/fixtures"
W="$(mktemp -d "${TMPDIR:-/tmp}/genret-test.XXXXXXXX")"
trap 'rm -rf "$W"' EXIT
LC_ALL=C; export LC_ALL
pass=0 fail=0
ok() { pass=$((pass + 1)); }
bad() { echo "FAIL: $*" >&2; fail=$((fail + 1)); }
t() { tdesc="$1"; shift; if "$@"; then ok; else bad "$tdesc"; fi; }
sha() { sha256sum "$1" | cut -c1-64; }
REC="$FX/corpus/records"
MAN="$REC/MANIFEST"
TK="$FX/tasks"

# q <manifest> <kind> <task dir> <out> [extra args...]: one query with the manifest's own digest pinned.
# K and CAP default to the preregistered settings (k 3, cap 4096; GEN protocol).
q() {
  qm="$1" qk="$2" qt="$3" qo="$4"; shift 4
  "$G" query --corpus "$qm" --manifest-sha256 "$(sha "$qm")" --recheck "$RC" --kind "$qk" --k "${K:-3}" \
    --cap-bytes "${CAP:-4096}" --requirement "$qt/requirement" --prompt "$qt/prompt" --out "$qo" "$@"
}
rv() { sed -n "s/^$2=//p" "$1/receipt.txt"; }                       # receipt value
col() { tail -n +2 "$1" | cut -f"$2"; }                              # column of a tsv without header
reason_of() { awk -F'\t' -v i="$2" '$1 == i { print $4 }' "$1/excluded.tsv"; }
# fatal_case <name> <reason> <out> <command...>: exit 2, the reason on stderr, nothing written
fatal_case() {
  fn="$1" fr="$2" fo="$3"; shift 3
  set +e; "$@" > "$W/fatal.out" 2> "$W/fatal.err"; st=$?; set -e
  if [ "$st" -ne 2 ]; then bad "$fn: exit $st, expected 2"; return; fi
  if ! grep -q "^genret: FATAL $fr " "$W/fatal.err"; then bad "$fn: stderr '$(cat "$W/fatal.err")' lacks FATAL $fr"; return; fi
  if [ -e "$fo" ] && [ "$fr" != out_exists ]; then bad "$fn: $fo was created"; return; fi
  ok
}
# copy_corpus <dir>: writable copy of the fixture record set (records/ layout); remanifest <dir> rewrites MANIFEST
copy_corpus() { rm -rf "$1"; mkdir -p "$1"; cp -R "$REC/." "$1/"; }
remanifest() { (cd "$1" && find raw abstraction -name '*.rec' | sort | xargs sha256sum > MANIFEST); }
# mkreceipt <records dir> <id>: writes a receipt for record <id>, prints its sha256 (= file name)
mkreceipt() {
  printf 'gen-receipt 1\nrecord %s\nnote test receipt\n' "$2" > "$1/receipts/tmp"
  mr="$(sha "$1/receipts/tmp")"; mv "$1/receipts/tmp" "$1/receipts/$mr"; printf '%s' "$mr"
}
hdr() { awk '/^body$/ { exit } { print }' "$1" | sed -n "s/^$2 //p"; }   # header value (empty for bare 'params')
body() { sed '1,/^body$/d' "$1"; }
recf() { case "$2" in raw-*) echo "$1/raw/$2.rec" ;; *) echo "$1/abstraction/$2.rec" ;; esac; }   # recf <records dir> <id>
aid() { basename "$(grep -l "^entry $1\$" "$REC"/abstraction/*.rec)" .rec; }                   # abstraction id by entry
famtok() { printf 'gs1-%s' "$(printf 'gs1:fixture-%s' "$1" | sha256sum | cut -c1-16)"; }       # fixture family token
A_SAT="$(aid sat_sub)" A_DIG="$(aid ndigits)" A_DOT="$(aid dot)" A_RNG="$(aid range_sum)" A_OLD="$(aid sat_sub_old)"

# ---- 1 version and the fixture pin --------------------------------------------------------------------
t "version" [ "$("$G" version)" = genret-cli-v2 ]
t "fixture manifest matches its records" sh -c "cd '$REC' && sha256sum -c --quiet MANIFEST"
t "SET.sha256 is the manifest digest" [ "$(cat "$REC/SET.sha256")" = "$(sha "$MAN")" ]
t "recheck accepts every fixture record" sh -c "'$RC' --receipts '$REC/receipts' \$(sed 's|^[0-9a-f]*  |$REC/|' '$MAN') > /dev/null"
t "fixtures regenerate byte-identically" sh -c "cp -R '$FX' '$W/fxcopy' && sh '$W/fxcopy/make_fixtures.sh' && diff -r '$FX/corpus' '$W/fxcopy/corpus' > /dev/null"

# ---- 2 synthetic queries retrieve the expected permitted records ------------------------------------
for c in "devq_clamp raw raw-clamp_sub" "devq_clamp abstraction $A_SAT" "devq_digits raw raw-count_digits" \
         "devq_digits abstraction $A_DIG" "devq_dot raw raw-dot3" "devq_dot abstraction $A_DOT"; do
  set -- $c
  q "$MAN" "$2" "$TK/$1" "$W/s.$1.$2"
  t "$1 $2 rank 1 is $3" [ "$(col "$W/s.$1.$2/selected.tsv" 2 | head -n 1)" = "$3" ]
  t "$1 $2 selects 3" [ "$(rv "$W/s.$1.$2" selected)" = 3 ]
  for id in $(col "$W/s.$1.$2/selected.tsv" 2); do
    f="$(recf "$REC" "$id")"
    t "$1 $2 selected $id is eligible" [ "$(hdr "$f" kind) $(hdr "$f" split) $(hdr "$f" validity)" = "$2 corpus verified" ]
    t "$1 $2 selected $id digest = manifest" [ "$(awk -F'\t' -v i="$id" '$2 == i { print $3 }' "$W/s.$1.$2/selected.tsv")" = "$(sha "$f")" ]
  done
done
for kd in raw abstraction; do
  q "$MAN" $kd "$TK/devq_none" "$W/none.$kd"
  t "devq_none $kd selects nothing" [ "$(rv "$W/none.$kd" selected)" = 0 ]
  t "devq_none $kd empty suffix" [ ! -s "$W/none.$kd/suffix.txt" ]
  t "devq_none $kd every indexed record no_match" [ "$(awk -F'\t' 'NR > 1 && $3 != "-" && $4 != "no_match"' "$W/none.$kd/excluded.tsv" | wc -l)" -eq 0 ]
done

# ---- 3 determinism: repeat runs and manifest order ----------------------------------------------------
q "$MAN" raw "$TK/devq_clamp" "$W/d1"; q "$MAN" raw "$TK/devq_clamp" "$W/d2"
for f in selected.tsv excluded.tsv suffix.txt query.txt receipt.txt SHA256SUMS; do t "repeat run $f identical" cmp -s "$W/d1/$f" "$W/d2/$f"; done
copy_corpus "$W/rev"; awk '{ l[NR] = $0 } END { for (i = NR; i >= 1; i--) print l[i] }' "$MAN" > "$W/rev/MANIFEST"
t "reversed manifest differs" sh -c "! cmp -s '$MAN' '$W/rev/MANIFEST'"
q "$W/rev/MANIFEST" raw "$TK/devq_clamp" "$W/d3"
for f in selected.tsv excluded.tsv suffix.txt query.txt; do t "manifest order does not change $f" cmp -s "$W/d1/$f" "$W/d3/$f"; done
t "SHA256SUMS lists the five outputs" sh -c "cd '$W/d1' && sha256sum -c --quiet SHA256SUMS && [ \$(wc -l < SHA256SUMS) -eq 5 ]"
t "receipt names the contract" [ "$(rv "$W/d1" contract)" = genret-cli-v2 ]

# ---- 4 stable tie-break by record id byte order ------------------------------------------------------
q "$MAN" raw "$TK/devq_twin" "$W/tie"
t "twins rank 1,2 by id" [ "$(col "$W/tie/selected.tsv" 2 | head -n 2 | tr '\n' ' ')" = "raw-tie_a raw-tie_b " ]
t "twins have equal scores" [ "$(col "$W/tie/selected.tsv" 4 | sed -n 1p)" = "$(col "$W/tie/selected.tsv" 4 | sed -n 2p)" ]
copy_corpus "$W/tie2"; r="$(mkreceipt "$W/tie2" raw-tie_c)"
sed -e 's/^id raw-tie_a$/id raw-tie_c/' -e 's/^ancestry tie_a /ancestry tie_c /' -e "s/^receipt .*/receipt $r/" \
  "$REC/raw/raw-tie_a.rec" > "$W/tie2/raw/raw-tie_c.rec"
rm "$W/tie2/raw/raw-tie_a.rec"; remanifest "$W/tie2"
q "$W/tie2/MANIFEST" raw "$TK/devq_twin" "$W/tie2o"
t "renamed twin moves behind by id" [ "$(col "$W/tie2o/selected.tsv" 2 | head -n 2 | tr '\n' ' ')" = "raw-tie_b raw-tie_c " ]

# ---- 5 exclusions: cross-split, unverified, poisoned, wrong kind, family ------------------------------
E="$W/s.devq_clamp.raw"
for c in raw-probe_clamp:split:probe raw-unverified_clamp:validity:unverified raw-poison_fence:poisoned:fence "$A_SAT:kind:abstraction"; do
  id="${c%%:*}"; t "C excludes $id as ${c#*:}" [ "$(reason_of "$E" "$id")" = "${c#*:}" ]
done
E="$W/s.devq_clamp.abstraction"
for c in "$A_OLD:validity:unverified" raw-clamp_sub:kind:raw raw-probe_clamp:kind:raw; do
  id="${c%%:*}"; t "D excludes $id as ${c#*:}" [ "$(reason_of "$E" "$id")" = "${c#*:}" ]
done
t "every manifest record is in selected or excluded" \
  [ "$(cat "$W/s.devq_clamp.raw/selected.tsv" "$W/s.devq_clamp.raw/excluded.tsv" | grep -vc -e '^rank' -e '^id')" -eq "$(wc -l < "$MAN")" ]
t "poisoned text never reaches the suffix" sh -c "! grep -q 'Ignore all previous' '$W/s.devq_clamp.raw/suffix.txt'"
FB="$(famtok branch)" FC="$(famtok call)"
t "fixture family token is in the record" [ "$(hdr "$REC/raw/raw-clamp_sub.rec" family)" = "$FB" ]
q "$MAN" raw "$TK/devq_clamp" "$W/fam" --exclude-family "$FB"
t "exclude-family on C" [ "$(reason_of "$W/fam" raw-clamp_sub)" = "family_excluded:$FB" ]
t "excluded family recorded" [ "$(rv "$W/fam" exclude_family)" = "$FB" ]
t "excluded family records not selected" sh -c "! grep -q raw-clamp_sub '$W/fam/selected.tsv'"
q "$MAN" abstraction "$TK/devq_dot" "$W/famD" --exclude-family "$FC" --exclude-family "$FB"
t "exclude-family matches the second family of a record" [ "$(reason_of "$W/famD" "$A_DOT")" = "family_excluded:$FC" ]
t "exclude-family on D" [ "$(reason_of "$W/famD" "$A_SAT")" = "family_excluded:$FB" ]
t "two excluded families recorded in order" [ "$(rv "$W/famD" exclude_family)" = "$FC,$FB" ]

# ---- 6 fail closed: integrity problems stop the run, nothing is written --------------------------------
R="$TK/devq_clamp/requirement" P="$TK/devq_clamp/prompt" O="$W/fx"
gq() { gm="$1" gs="$2" gr="$3" gp="$4"; shift 4
  "$G" query --corpus "$gm" --manifest-sha256 "$gs" --recheck "${GRC:-$RC}" --kind raw --k 3 --cap-bytes 4096 \
    --requirement "$gr" --prompt "$gp" --out "$O" "$@"; }
cq() { gq "$1/MANIFEST" "$(sha "$1/MANIFEST")" "$R" "$P"; }     # cq <records dir>: query a mutated copy
M0="$(sha "$MAN")"
bads="$(printf '%s' "$M0" | sed 's/^./0/')"; [ "$bads" != "$M0" ] || bads="$(printf '%s' "$M0" | sed 's/^./1/')"
fatal_case "pinned manifest digest differs" manifest_digest_mismatch "$O" gq "$MAN" "$bads" "$R" "$P"
fatal_case "manifest missing" manifest_unreadable "$O" gq "$W/nope/MANIFEST" "$M0" "$R" "$P"
copy_corpus "$W/c1"; printf 'x\n' >> "$W/c1/raw/raw-clamp_sub.rec"
fatal_case "stale record digest" stale_digest "$O" cq "$W/c1"
copy_corpus "$W/c2"; rm "$W/c2/abstraction/$A_DOT.rec"
fatal_case "listed record missing" record_missing "$O" cq "$W/c2"
for m in "s/^desc /color red\ndesc /:unknown key" "/^receipt /d:missing receipt line" "s/^kind raw$/kind rawish/:bad kind" \
         "s/^gen-record 1$/gen-record 9/:bad magic" "s/^id raw-dot3$/id Raw-Dot3/:bad id" "s/^params .*/params u8/:params disagree with signature" \
         "s/^end$/end /:trailing space" "s/^s = long 0$/s = long 0\x01/:control byte in body" "s/^split corpus$/split heldout/:split outside the schema" \
         "s/^validity verified$/validity revoked/:validity outside the schema"; do
  copy_corpus "$W/c3"; sed "${m%%:*}" "$REC/raw/raw-dot3.rec" > "$W/c3/raw/raw-dot3.rec"; remanifest "$W/c3"
  t "mutant differs: ${m#*:}" sh -c "! cmp -s '$REC/raw/raw-dot3.rec' '$W/c3/raw/raw-dot3.rec'"
  fatal_case "invalid record: ${m#*:}" invalid_record "$O" cq "$W/c3"
done
copy_corpus "$W/c4"; rm "$W/c4/receipts/$(hdr "$REC/raw/raw-dot3.rec" receipt)"
fatal_case "receipt file missing" invalid_record "$O" cq "$W/c4"
copy_corpus "$W/c4b"; printf 'edited\n' >> "$W/c4b/receipts/$(hdr "$REC/raw/raw-dot3.rec" receipt)"
fatal_case "receipt file edited" invalid_record "$O" cq "$W/c4b"
copy_corpus "$W/c5"; cp "$REC/raw/raw-tie_a.rec" "$W/c5/raw/raw-tie_a2.rec"; remanifest "$W/c5"
fatal_case "duplicate record id" duplicate_id "$O" cq "$W/c5"
copy_corpus "$W/c6"; sed '1s|  |  raw/../|' "$MAN" > "$W/c6/MANIFEST"
fatal_case "manifest path with .." manifest_malformed "$O" cq "$W/c6"
copy_corpus "$W/c7"; sed '1s/  / /' "$MAN" > "$W/c7/MANIFEST"
fatal_case "manifest one-space separator" manifest_malformed "$O" cq "$W/c7"
copy_corpus "$W/c8"; printf '%s' "$(cat "$MAN")" > "$W/c8/MANIFEST"
fatal_case "manifest without final newline" manifest_malformed "$O" cq "$W/c8"
copy_corpus "$W/c9"; sed -n 1p "$MAN" >> "$W/c9/MANIFEST"
fatal_case "manifest duplicate path" manifest_malformed "$O" cq "$W/c9"
# recheck itself must be real and must agree record by record
GRC="$W/nope/recheck" fatal_case "recheck binary missing" recheck_failed "$O" gq "$MAN" "$M0" "$R" "$P"
printf '#!/bin/sh\necho OK raw-somebody-else\n' > "$W/fake1"; chmod +x "$W/fake1"
GRC="$W/fake1" fatal_case "recheck names another id" recheck_failed "$O" gq "$MAN" "$M0" "$R" "$P"
printf '#!/bin/sh\nexit 0\n' > "$W/fake2"; chmod +x "$W/fake2"
GRC="$W/fake2" fatal_case "recheck prints nothing" recheck_failed "$O" gq "$MAN" "$M0" "$R" "$P"
printf '#!/bin/sh\nexit 1\n' > "$W/fake3"; chmod +x "$W/fake3"
GRC="$W/fake3" fatal_case "recheck refuses without a reason" recheck_failed "$O" gq "$MAN" "$M0" "$R" "$P"
printf '#!/bin/sh\nshift 2\nfor f; do sed -n "s/^id /OK /p" "$f"; done\necho OK extra\n' > "$W/fake4"; chmod +x "$W/fake4"
GRC="$W/fake4" fatal_case "recheck prints an extra line" recheck_failed "$O" gq "$MAN" "$M0" "$R" "$P"
copy_corpus "$W/c10"
printf '#!/bin/sh\nshift 2\nfor f; do sed -n "s/^id /OK /p" "$f"; done\nprintf "x\\n" >> "%s"\n' "$W/c10/raw/raw-dot3.rec" > "$W/fake5"; chmod +x "$W/fake5"
GRC="$W/fake5" fatal_case "record changed while recheck read it" stale_digest "$O" cq "$W/c10"
mkdir -p "$W/task/hidden"; cp "$R" "$W/task/hidden/requirement"; cp "$P" "$W/task/hidden/prompt"
fatal_case "requirement under hidden/" hidden_path "$O" gq "$MAN" "$M0" "$W/task/hidden/requirement" "$P"
fatal_case "prompt under hidden/" hidden_path "$O" gq "$MAN" "$M0" "$R" "$W/task/hidden/prompt"
grep -v '^entry' "$R" > "$W/task/noentry"; { cat "$R"; echo "ret u8"; } > "$W/task/tworet"
fatal_case "requirement without entry" requirement_invalid "$O" gq "$MAN" "$M0" "$W/task/noentry" "$P"
fatal_case "requirement with two ret lines" requirement_invalid "$O" gq "$MAN" "$M0" "$W/task/tworet" "$P"
: > "$W/task/empty"; printf 'tab\there\n' > "$W/task/tab"
fatal_case "empty prompt" prompt_invalid "$O" gq "$MAN" "$M0" "$R" "$W/task/empty"
fatal_case "prompt with a tab" prompt_invalid "$O" gq "$MAN" "$M0" "$R" "$W/task/tab"
mkdir -p "$W/exists"
fatal_case "out exists" out_exists "$W/exists" "$G" query --corpus "$MAN" --manifest-sha256 "$M0" --recheck "$RC" --kind raw --k 3 --cap-bytes 9 --requirement "$R" --prompt "$P" --out "$W/exists"
t "existing out left empty" [ -z "$(ls -A "$W/exists")" ]
fatal_case "k 0" usage "$O" "$G" query --corpus "$MAN" --manifest-sha256 "$M0" --recheck "$RC" --kind raw --k 0 --cap-bytes 9 --requirement "$R" --prompt "$P" --out "$O"
fatal_case "k 65" usage "$O" "$G" query --corpus "$MAN" --manifest-sha256 "$M0" --recheck "$RC" --kind raw --k 65 --cap-bytes 9 --requirement "$R" --prompt "$P" --out "$O"
fatal_case "kind other" usage "$O" "$G" query --corpus "$MAN" --manifest-sha256 "$M0" --recheck "$RC" --kind text --k 1 --cap-bytes 9 --requirement "$R" --prompt "$P" --out "$O"
fatal_case "no --recheck" usage "$O" "$G" query --corpus "$MAN" --manifest-sha256 "$M0" --kind raw --k 1 --cap-bytes 9 --requirement "$R" --prompt "$P" --out "$O"
fatal_case "unknown option" usage "$O" gq "$MAN" "$M0" "$R" "$P" --hidden "$W"
fatal_case "target without length-matched" usage "$O" gq "$MAN" "$M0" "$R" "$P" --target-bytes 100
fatal_case "target above cap" usage "$O" gq "$MAN" "$M0" "$R" "$P" --mode length-matched --target-bytes 5000
fatal_case "repeated k" usage "$O" gq "$MAN" "$M0" "$R" "$P" --k 2
fatal_case "unknown mode" usage "$O" gq "$MAN" "$M0" "$R" "$P" --mode lucky

# ---- 7 C and D under identical caps; whole records only; independent renderer --------------------------
SETTINGS='^(contract|mode|k|cap_bytes|target_bytes|exclude_family|manifest_sha256|requirement_sha256|prompt_sha256|query_sha256)='
for cap in 600 4096; do
  CAP=$cap q "$MAN" raw "$TK/devq_clamp" "$W/C$cap"; CAP=$cap q "$MAN" abstraction "$TK/devq_clamp" "$W/D$cap"
  for kd in C D; do
    o="$W/$kd$cap"
    t "$kd cap $cap respected" [ "$(rv "$o" suffix_bytes)" -le "$cap" ]
    t "$kd cap $cap suffix_bytes = file size" [ "$(rv "$o" suffix_bytes)" -eq "$(wc -c < "$o/suffix.txt")" ]
  done
  t "C and D receipts share settings at cap $cap" [ "$(grep -E "$SETTINGS" "$W/C$cap/receipt.txt")" = "$(grep -E "$SETTINGS" "$W/D$cap/receipt.txt")" ]
done
t "cap 600 drops whole records (C)" [ "$(rv "$W/C600" selected)" -lt 3 ]
t "cap 600 logs cap_bytes (C)" [ "$(reason_of "$W/C600" raw-dot3)" = cap_bytes ]
t "after the cap stop, lower records are cap_bytes too" [ "$(reason_of "$W/C600" raw-sum_range)" = cap_bytes ]
CAP=60 q "$MAN" raw "$TK/devq_clamp" "$W/C60"
t "cap below the first record: empty" [ "$(rv "$W/C60" selected)" = 0 ]
t "cap below the first record: empty suffix" [ ! -s "$W/C60/suffix.txt" ]
t "cap below the first record: rank 1 logged cap_bytes" [ "$(reason_of "$W/C60" raw-clamp_sub)" = cap_bytes ]
K=2 q "$MAN" raw "$TK/devq_clamp" "$W/K2"
t "k 2 selects 2" [ "$(rv "$W/K2" selected)" = 2 ]
t "rank 3 logged beyond_k" [ "$(reason_of "$W/K2" raw-sum_range)" = beyond_k ]
# render <out dir> <kind>: the suffix rebuilt in sh from selected.tsv and the record files (CONTRACT.md 6)
render() {
  if [ "$(col "$1/selected.tsv" 2 | wc -l)" -eq 0 ]; then return; fi
  if [ "$2" = raw ]; then noun="solutions to earlier tasks"; else noun="helper functions"; fi
  printf '\nRetrieved records: verified %s, most relevant first.\n' "$noun"
  tail -n +2 "$1/selected.tsv" | while IFS="$(printf '\t')" read -r rk id dg sc by rl; do
    f="$(recf "$REC" "$id")"
    printf '\nRecord %s.\n\nentry %s\n' "$rk" "$(hdr "$f" entry)"
    pv="$(hdr "$f" params)"; if [ -n "$pv" ]; then printf 'params %s\n' "$pv"; else printf 'params\n'; fi
    printf 'ret %s\n\n%s\n\n```\n' "$(hdr "$f" ret)" "$(hdr "$f" desc)"
    body "$f"; printf '```\n'
  done
}
for o in "C600 raw" "D600 abstraction" "C4096 raw" "D4096 abstraction" "tie raw"; do
  set -- $o; render "$W/$1" "$2" > "$W/$1.render"
  t "independent renderer matches suffix $1" cmp -s "$W/$1.render" "$W/$1/suffix.txt"
  t "bytes column sums to suffix minus heading $1" [ "$(col "$W/$1/selected.tsv" 5 | awk '{ s += $1 } END { print s + 0 }')" -eq \
    "$(($(wc -c < "$W/$1/suffix.txt") - $(head -n 2 "$W/$1/suffix.txt" | wc -c)))" ]
done
t "D suffix shows no abstraction-only fields" sh -c "! grep -q -e '^pre ' -e '^post ' -e '^example ' -e '^fingerprint ' -e '^receipt ' '$W/D4096/suffix.txt'"

# ---- 8 query = public requirement + prompt only: hidden canary never influences output ----------------
mkdir -p "$W/nohid"; cp "$TK/devq_clamp/requirement" "$TK/devq_clamp/prompt" "$W/nohid/"
mkdir -p "$W/otherhid/hidden"; cp "$TK/devq_clamp/requirement" "$TK/devq_clamp/prompt" "$W/otherhid/"
printf 'twin twin twin sat sub clamp\n' > "$W/otherhid/hidden/expected"; printf 'zqxcanary\n' > "$W/otherhid/hidden/inputs"
q "$MAN" raw "$W/nohid" "$W/h1"; q "$MAN" raw "$W/otherhid" "$W/h2"
for f in selected.tsv excluded.tsv suffix.txt query.txt receipt.txt; do
  t "no hidden dir: $f identical" cmp -s "$W/d1/$f" "$W/h1/$f"
  t "different hidden files: $f identical" cmp -s "$W/d1/$f" "$W/h2/$f"
done
t "the fixture task really has a hidden canary" grep -q zqxcanary "$TK/devq_clamp/hidden/expected"
t "canary token absent from the query" sh -c "! grep -qx zqxcanary '$W/d1/query.txt'"
t "canary record not selected" sh -c "! grep -q raw-canary '$W/d1/selected.tsv'"
mkdir -p "$W/leak"; cp "$TK/devq_clamp/requirement" "$W/leak/"; { cat "$TK/devq_clamp/prompt"; echo "zqxcanary zqxcanary"; } > "$W/leak/prompt"
q "$MAN" raw "$W/leak" "$W/h3"
t "positive control: a leaked canary word gets the canary record selected" grep -q raw-canary "$W/h3/selected.tsv"
t "positive control: the leaked word is in the query" grep -qx zqxcanary "$W/h3/query.txt"
# the requirement contributes the entry/params/ret values only: comment lines do not count
{ echo "# zqxcanary zqxcanary zqxcanary"; cat "$TK/devq_clamp/requirement"; } > "$W/nohid/requirement"
q "$MAN" raw "$W/nohid" "$W/h4"
t "requirement comment lines are not query text" cmp -s "$W/d1/query.txt" "$W/h4/query.txt"
t "requirement comment lines change no selection" cmp -s "$W/d1/selected.tsv" "$W/h4/selected.tsv"

# ---- 9 ablations: irrelevant and empty ----------------------------------------------------------------
for kd in raw abstraction; do
  K=2 q "$MAN" $kd "$TK/devq_clamp" "$W/irr.$kd" --mode irrelevant
  o="$W/irr.$kd"
  t "irrelevant $kd selects 2" [ "$(rv "$o" selected)" = 2 ]
  t "irrelevant $kd scores ascend" [ "$(col "$o/selected.tsv" 4 | sort -n | tr '\n' ' ')" = "$(col "$o/selected.tsv" 4 | tr '\n' ' ')" ]
  minall="$( { col "$o/selected.tsv" 4; awk -F'\t' 'NR > 1 && $3 != "-" { print $3 }' "$o/excluded.tsv"; } | sort -n | head -n 1)"
  t "irrelevant $kd rank 1 has the lowest score" [ "$(col "$o/selected.tsv" 4 | head -n 1)" = "$minall" ]
  top="$(col "$W/s.devq_clamp.$kd/selected.tsv" 2 | head -n 1)"
  t "irrelevant $kd excludes the normal top record" sh -c "! cut -f2 '$o/selected.tsv' | grep -qx '$top'"
  q "$MAN" $kd "$TK/devq_clamp" "$W/emp.$kd" --mode empty
  o="$W/emp.$kd"
  t "empty $kd selects 0" [ "$(rv "$o" selected)" = 0 ]
  t "empty $kd suffix 0 bytes" [ ! -s "$o/suffix.txt" ]
  t "empty $kd logs ablation_empty for every indexed record" \
    [ "$(awk -F'\t' 'NR > 1 && $3 != "-" && $4 == "ablation_empty"' "$o/excluded.tsv" | wc -l)" -eq "$(rv "$o" indexed)" ]
done
t "empty mode keeps exclusion reasons of unindexed records" [ "$(reason_of "$W/emp.raw" raw-poison_fence)" = poisoned:fence ]

# ---- 10 length-matched ablation: C padded with one extra raw record -------------------------------------
K=1 q "$MAN" raw "$TK/devq_clamp" "$W/lm0"
K=1 q "$MAN" raw "$TK/devq_clamp" "$W/lm1" --mode length-matched --target-bytes 700
t "pad: one extra record" [ "$(rv "$W/lm1" selected)" = 2 ]
t "pad role and position" [ "$(tail -n 1 "$W/lm1/selected.tsv" | cut -f1,6)" = "$(printf '2\tpad')" ]
t "pad is the first lower-ranked record that fits (raw-dot3 is too long)" [ "$(rv "$W/lm1" pad)" = raw-sum_range ]
t "pad stays within target" [ "$(rv "$W/lm1" suffix_bytes)" -le 700 ]
t "pad keeps the normal record first" [ "$(sed -n 2p "$W/lm1/selected.tsv")" = "$(sed -n 2p "$W/lm0/selected.tsv")" ]
t "pad longer than unpadded" [ "$(rv "$W/lm1" suffix_bytes)" -gt "$(rv "$W/lm0" suffix_bytes)" ]
K=1 q "$MAN" raw "$TK/devq_clamp" "$W/lm2" --mode length-matched --target-bytes 300
t "no record fits the target: pad=none" [ "$(rv "$W/lm2" pad)" = none ]
t "pad=none leaves the normal selection" cmp -s "$W/lm0/selected.tsv" "$W/lm2/selected.tsv"
K=2 q "$MAN" abstraction "$TK/devq_dot" "$W/lmD"; tgt="$(rv "$W/lmD" suffix_bytes)"
K=2 q "$MAN" raw "$TK/devq_dot" "$W/lmC" --mode length-matched --target-bytes "$tgt"
K=2 q "$MAN" raw "$TK/devq_dot" "$W/lmCn"; cn="$(rv "$W/lmCn" suffix_bytes)"; if [ "$cn" -gt "$tgt" ]; then mx="$cn"; else mx="$tgt"; fi
t "C padded to D's length: never longer than max(target, unpadded C)" [ "$(rv "$W/lmC" suffix_bytes)" -le "$mx" ]
t "C padded to D's length: unpadded rows kept" [ "$(head -n 3 "$W/lmC/selected.tsv")" = "$(cat "$W/lmCn/selected.tsv")" ]
render "$W/lm1" raw > "$W/lm1.render"
t "independent renderer matches the padded suffix" cmp -s "$W/lm1.render" "$W/lm1/suffix.txt"

# ---- 11 independent score oracle (sh tokens + bc integer BM25) ------------------------------------------
tokens() { tr 'A-Z' 'a-z' | tr -cs 'a-z0-9' '\n' | grep -v '^$' || true; }
oracle() { # oracle <out dir> <kind> <task dir>: prints "id score" for every indexed record
  od="$1" okd="$2" ot="$3" oo="$W/oracle"; rm -rf "$oo"; mkdir -p "$oo"
  { grep -E '^(entry|params|ret)( |$)' "$ot/requirement" | sed 's/^[a-z]*//'; cat "$ot/prompt"; } | tokens | sort -u > "$oo/q"
  cmp -s "$oo/q" "$od/query.txt" || { echo "oracle: query terms differ" >&2; return 1; }
  : > "$oo/ids"
  for f in "$REC"/raw/*.rec "$REC"/abstraction/*.rec; do
    [ "$(hdr "$f" kind)" = "$okd" ] && [ "$(hdr "$f" split)" = corpus ] && [ "$(hdr "$f" validity)" = verified ] || continue
    if body "$f" | grep -q '```' || hdr "$f" desc | grep -q '```'; then continue; fi
    id="$(hdr "$f" id)"; echo "$id" >> "$oo/ids"
    { for k in entry params ret desc; do hdr "$f" $k; done; body "$f"; } | tokens > "$oo/$id.tok"
  done
  N="$(wc -l < "$oo/ids")"; T="$(cat "$oo"/*.tok | wc -l)"
  {
    echo 'define g(a, b) { auto k, y, r, i; k = 0; while (a >= 2 * b) { b = 2 * b; k = k + 1; }'
    echo '  y = (a * 2^30) / b; r = k * 2^16; for (i = 15; i >= 0; i--) { y = (y * y) / 2^30; if (y >= 2^31) { y = y / 2; r = r + 2^i; } }'
    echo '  return (r); }'
    while read -r id; do
      dl="$(wc -l < "$oo/$id.tok")"; printf 's = 0\n'
      while read -r term; do
        tf="$(grep -cx "$term" "$oo/$id.tok" || true)"; [ "$tf" -gt 0 ] || continue
        df=0; while read -r j; do if grep -qx "$term" "$oo/$j.tok"; then df=$((df + 1)); fi; done < "$oo/ids"
        printf 's = s + (g(%s, %s) * %s * 44 * %s) / (20 * %s * %s + 6 * %s + 18 * %s * %s)\n' $((2 * N + 2)) $((2 * df + 1)) "$tf" "$T" "$T" "$tf" "$T" "$dl" "$N"
      done < "$oo/q"
      printf 'print "%s ", s, "\\n"\n' "$id"
    done < "$oo/ids"
  } | bc | sort
}
for c in "s.devq_clamp.raw raw devq_clamp" "s.devq_digits.abstraction abstraction devq_digits" "s.devq_dot.raw raw devq_dot" "tie raw devq_twin"; do
  set -- $c
  { tail -n +2 "$W/$1/selected.tsv" | cut -f2,4; awk -F'\t' 'NR > 1 && $3 != "-" { print $1 "\t" $3 }' "$W/$1/excluded.tsv"; } | tr '\t' ' ' | sort > "$W/$1.scores"
  if oracle "$W/$1" "$2" "$TK/$3" > "$W/$1.oracle"; then
    t "oracle scores = genret scores ($1)" cmp -s "$W/$1.oracle" "$W/$1.scores"
  else bad "oracle failed ($1)"; fi
done
t "oracle checked nonzero scores" [ "$(awk '$2 > 0' "$W/s.devq_clamp.raw.scores" | wc -l)" -gt 0 ]
sed '1s/ \([0-9]*\)$/ 1\1/' "$W/s.devq_clamp.raw.scores" > "$W/mut.scores"
t "oracle comparison detects a changed score (mutant)" sh -c "! cmp -s '$W/s.devq_clamp.raw.oracle' '$W/mut.scores'"

echo "test_genret: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
