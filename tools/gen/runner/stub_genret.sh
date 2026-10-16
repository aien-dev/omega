#!/usr/bin/env bash
# tools/gen/runner/stub_genret.sh -- TEST STUB of the retrieval provider, same call shape and output files as
# genret-cli-v2 (Agent E, tools/gen/retrieval/CONTRACT.md at a4878d69, sha256 e1f2c1cf...49ab9), for the GEN
# runner's CPU-only tests until the real genret lands (omega#477, GEN-2). NOT the contract's ranker:
#   - records: gen-record 1, each validated by `<recheck> --receipts <MANIFEST dir>/receipts <record>` which must
#     print exactly `OK <id>` (contract section 2); eligibility as contract section 5 (kind, split corpus,
#     validity verified, --exclude-family, fence), checked in manifest order;
#   - ranking: manifest order (score column 0), then the contract's k / cap walk (beyond_k, cap_bytes);
#   - rendering (header, blocks), outputs, receipt keys and exit codes as the contract.
#   - --mode empty selects nothing; irrelevant and length-matched are refused (usage) by the stub.
# `version` prints "genret-cli-v2 stub": the runner refuses a stub provider outside test mode.
set -euo pipefail
fatal() { echo "genret: FATAL $1 ${2:-}" >&2; exit 2; }
[ "${1:-}" = version ] && { echo "genret-cli-v2 stub"; exit 0; }
[ "${1:-}" = query ] || fatal usage "first argument must be query or version"
shift
CORPUS="" RC="" MSHA="" KIND="" K="" CAP="" REQ="" PR="" OUT="" MODE="" TB="" EXF=()
declare -A SEEN
while [ $# -gt 0 ]; do
  [ $# -ge 2 ] || fatal usage "$1 needs a value"
  case "$1" in --exclude-family) ;; *) [ -z "${SEEN[$1]:-}" ] || fatal usage "$1 twice"; SEEN[$1]=1 ;; esac
  case "$1" in
    --corpus) CORPUS="$2" ;; --recheck) RC="$2" ;; --manifest-sha256) MSHA="$2" ;; --kind) KIND="$2" ;; --k) K="$2" ;; --cap-bytes) CAP="$2" ;;
    --requirement) REQ="$2" ;; --prompt) PR="$2" ;; --out) OUT="$2" ;; --mode) MODE="$2" ;; --target-bytes) TB="$2" ;;
    --exclude-family) EXF+=("$2") ;;
    *) fatal usage "unknown option $1" ;;
  esac
  shift 2
done
[ -n "$CORPUS" ] && [ -n "$RC" ] && [ -n "$MSHA" ] && [ -n "$KIND" ] && [ -n "$K" ] && [ -n "$CAP" ] && [ -n "$REQ" ] && [ -n "$PR" ] && [ -n "$OUT" ] || fatal usage "missing option"
MODE="${MODE:-normal}"
case "$MODE" in normal|empty) ;; *) fatal usage "the stub implements --mode normal and empty only" ;; esac
[ -z "$TB" ] || fatal usage "--target-bytes only with --mode length-matched"
case "$KIND" in raw|abstraction) ;; *) fatal usage "--kind raw|abstraction" ;; esac
[[ "$K" =~ ^[1-9][0-9]?$ ]] && [ "$K" -le 64 ] || fatal usage "--k 1..64"
[[ "$CAP" =~ ^[1-9][0-9]{0,6}$ ]] && [ "$CAP" -le 1048576 ] || fatal usage "--cap-bytes 1..1048576"
[[ "$MSHA" =~ ^[0-9a-f]{64}$ ]] || fatal usage "--manifest-sha256"
[ ! -e "$OUT" ] || fatal out_exists "$OUT"
for f in "$REQ" "$PR"; do case "/$(realpath -m "$f")/" in */hidden/*) fatal hidden_path "$f" ;; esac; done
[ -f "$CORPUS" ] && [ -r "$CORPUS" ] || fatal manifest_unreadable "$CORPUS"
[ "$(sha256sum < "$CORPUS" | cut -d' ' -f1)" = "$MSHA" ] || fatal manifest_digest_mismatch "$CORPUS"
grep -qvE '^[0-9a-f]{64}  [A-Za-z0-9._/-]+$' "$CORPUS" && fatal manifest_malformed "$CORPUS"
[ "$(grep -c . "$REQ" 2> /dev/null)" -ge 1 ] || fatal requirement_invalid "$REQ"
for key in entry params ret; do [ "$(grep -cE "^$key( |\$)" "$REQ")" = 1 ] || fatal requirement_invalid "$key"; done
[ -s "$PR" ] && [ "$(wc -c < "$PR")" -le 4096 ] && ! LC_ALL=C grep -q '[^ -~]' "$PR" || fatal prompt_invalid "$PR"
CD="$(cd "$(dirname "$CORPUS")" && pwd)"
[ -x "$RC" ] || fatal recheck_failed "recheck $RC missing"
W="$(mktemp -d "${TMPDIR:-/tmp}/stubgenret.XXXXXXXX")"; trap 'rm -rf "$W"' EXIT
tok() { tr 'A-Z' 'a-z' | tr -c 'a-z0-9' '\n' | grep . || true; }
{ grep -E '^(entry|params|ret)( |$)' "$REQ" | sed -E 's/^(entry|params|ret) ?//'; cat "$PR"; } | tok | LC_ALL=C sort -u > "$W/query.txt"
noun="solutions to earlier tasks"; [ "$KIND" = abstraction ] && noun="helper functions"
hv() { awk -v k="$1" '$0 == "body" { exit } index($0, k " ") == 1 { print substr($0, length(k) + 2); exit } $0 == k { print ""; exit }' "$2"; }
: > "$W/suffix.txt"; : > "$W/sel"; : > "$W/exc"
n=0 stop=""
while read -r dig path; do
  f="$CD/$path"
  [ -f "$f" ] || fatal record_missing "$path"
  [ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$dig" ] || fatal stale_digest "$path"
  [ "$(head -n 1 "$f")" = "gen-record 1" ] && grep -qx body "$f" || fatal invalid_record "$path"
  id="$(hv id "$f")" kd="$(hv kind "$f")" sp="$(hv split "$f")" va="$(hv validity "$f")" fam="$(hv family "$f")"
  [ -n "$id" ] || fatal invalid_record "$path"
  rco="$("$RC" --receipts "$CD/receipts" "$f" 2>&1)" && rce=0 || rce=$?
  [ "$rce" != 1 ] || fatal invalid_record "$(grep -m1 REFUSED <<< "$rco")"
  [ "$rce" = 0 ] && [ "$rco" = "OK $id" ] || fatal recheck_failed "$path"
  [ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$dig" ] || fatal stale_digest "$path"
  body="$(sed -n '/^body$/,$p' "$f" | tail -n +2; echo x)"; body="${body%x}"
  desc="$(hv desc "$f")"
  why=""
  if [ "$kd" != "$KIND" ]; then why="kind:$kd"
  elif [ "$sp" != corpus ]; then why="split:$sp"
  elif [ "$va" != verified ]; then why="validity:$va"
  else
    for x in "${EXF[@]+"${EXF[@]}"}"; do for fm in $fam; do [ -z "$why" ] && [ "$fm" = "$x" ] && why="family_excluded:$fm"; done; done
    if [ -z "$why" ]; then
      if printf '%s\n%s' "$desc" "$body" | grep -qF '```'; then why="poisoned:fence"; fi
    fi
  fi
  if [ -z "$why" ] && [ "$MODE" = empty ]; then why=ablation_empty; fi
  if [ -z "$why" ] && [ -n "$stop" ]; then why="$stop"; fi
  if [ -z "$why" ]; then
    i=$((n + 1))
    pl="params"; for p in $(hv params "$f"); do pl="$pl $p"; done
    printf '\nRecord %d.\n\nentry %s\n%s\nret %s\n\n%s\n\n```\n%s```\n' "$i" "$(hv entry "$f")" "$pl" "$(hv ret "$f")" "$desc" "$body" > "$W/block"
    hb=0; [ "$n" = 0 ] && hb="$(printf '\nRetrieved records: verified %s, most relevant first.\n' "$noun" | wc -c)"
    if [ "$n" -ge "$K" ]; then why=beyond_k stop=beyond_k
    elif [ $(( $(wc -c < "$W/suffix.txt") + hb + $(wc -c < "$W/block") )) -gt "$CAP" ]; then why=cap_bytes stop=cap_bytes
    else
      [ "$n" = 0 ] && printf '\nRetrieved records: verified %s, most relevant first.\n' "$noun" >> "$W/suffix.txt"
      cat "$W/block" >> "$W/suffix.txt"; n=$i
      printf '%d\t%s\t%s\t0\t%d\ttop\n' "$i" "$id" "$dig" "$(wc -c < "$W/block")" >> "$W/sel"
    fi
  fi
  if [ -n "$why" ]; then sc=0; case "$why" in kind:*|split:*|validity:*|family_excluded:*|poisoned:*) sc=- ;; esac
    printf '%s\t%s\t%s\t%s\n' "$id" "$dig" "$sc" "$why" >> "$W/exc"; fi
done < "$CORPUS"
mkdir "$W/o"
{ printf 'rank\tid\tdigest\tscore\tbytes\trole\n'; cat "$W/sel"; } > "$W/o/selected.tsv"
{ printf 'id\tdigest\tscore\treason\n'; LC_ALL=C sort -t$'\t' -k1,1 "$W/exc"; } > "$W/o/excluded.tsv"
cp "$W/suffix.txt" "$W/query.txt" "$W/o/"
s() { sha256sum < "$1" | cut -d' ' -f1; }
ex="-"; [ "${#EXF[@]}" = 0 ] || ex="$(IFS=,; echo "${EXF[*]}")"
{
  echo "contract=genret-cli-v2"; echo "mode=$MODE"; echo "kind=$KIND"; echo "k=$K"; echo "cap_bytes=$CAP"; echo "target_bytes=-"
  echo "exclude_family=$ex"; echo "manifest_sha256=$MSHA"; echo "requirement_sha256=$(s "$REQ")"; echo "prompt_sha256=$(s "$PR")"
  echo "query_sha256=$(s "$W/o/query.txt")"; echo "records=$(grep -c . "$CORPUS")"; echo "indexed=$(($(grep -c . "$CORPUS") - $(awk -F'\t' '$3 == "-"' "$W/exc" | grep -c . || true)))"
  echo "total_tokens=0"; echo "selected=$n"; echo "pad=-"; echo "suffix_bytes=$(wc -c < "$W/o/suffix.txt")"; echo "suffix_sha256=$(s "$W/o/suffix.txt")"
  echo "selected_sha256=$(s "$W/o/selected.tsv")"; echo "excluded_sha256=$(s "$W/o/excluded.tsv")"
} > "$W/o/receipt.txt"
(cd "$W/o" && sha256sum selected.tsv excluded.tsv suffix.txt query.txt receipt.txt) > "$W/o/SHA256SUMS"
mkdir -p "$(dirname "$OUT")" && mv "$W/o" "$OUT" || fatal write_failed "$OUT"
