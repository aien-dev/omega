#!/bin/sh
# PD-0b run: the frozen learner against an external world binary, protocol v2 (play and truth from one binary).
# usage: pd0b_run.sh <harness-binary> <world-binary> <seeds-file> [note]
# seeds file: lines "level_index seed" (level_index 0..6 or null); blank lines and # comments ignored;
# every level that appears must appear exactly five times. Writes ONE new receipt
# evidence/physics0/pd0b/PD0B_RUN-<harness commit>-<world sha256[0:12]>.txt and refuses to overwrite.
set -eu
H=${1:?harness}; W=${2:?world binary}; S=${3:?seeds file}; NOTE=${4:-}
export LC_ALL=C
die() { echo "pd0b-run: $*" >&2; exit 2; }
[ -x "$H" ] || die "harness binary not executable: $H"
[ -x "$W" ] || die "PD0_WORLD_BIN not executable: $W"
[ -f "$S" ] || die "PD0B_SEEDS not found: $S"
lines=$(grep -v '^[[:space:]]*\(#\|$\)' "$S" || true)
[ -n "$lines" ] || die "seeds file has no instances"
echo "$lines" | awk '
  NF != 2 { printf "pd0b-run: bad seeds line (need: level_index seed): %s\n", $0 > "/dev/stderr"; bad = 1; next }
  $1 !~ /^([0-6]|null)$/ { printf "pd0b-run: bad level_index: %s\n", $1 > "/dev/stderr"; bad = 1; next }
  $2 !~ /^[0-9]+$/ { printf "pd0b-run: bad seed: %s\n", $2 > "/dev/stderr"; bad = 1; next }
  { n[$1]++ }
  END { for (l in n) if (n[l] != 5) { printf "pd0b-run: level %s has %d instances, need exactly 5\n", l, n[l] > "/dev/stderr"; bad = 1 } exit bad }' || exit 2
HEAD_FULL=$(git rev-parse HEAD); HEAD_SHORT=$(git rev-parse --short=12 HEAD)
if [ -n "$(git status --porcelain --untracked-files=no)" ] && [ "${PD0B_ALLOW_DIRTY:-0}" != 1 ]; then die "worktree has uncommitted changes; commit first (or PD0B_ALLOW_DIRTY=1 for a rehearsal that is not filed)"; fi
WSHA=$(sha256sum "$W" | cut -d' ' -f1); SSHA=$(sha256sum "$S" | cut -d' ' -f1)
OUTDIR=evidence/physics0/pd0b; mkdir -p "$OUTDIR"
OUT=$OUTDIR/PD0B_RUN-$HEAD_SHORT-$(echo "$WSHA" | cut -c1-12).txt
[ ! -e "$OUT" ] || die "receipt already exists, refusing to overwrite: $OUT"
LAWDIR=${OUT%.txt}-laws
[ ! -e "$LAWDIR" ] || die "laws directory already exists, refusing to overwrite: $LAWDIR"
LEDGER_ARCHIVE=${PD0B_LEDGER_ARCHIVE:-$HOME/workspace/evidence-archive/omega/physics0/pd0b}
LDIR=$LEDGER_ARCHIVE/$(basename "${OUT%.txt}")-ledgers   # ledgers are large: kept outside the repo, listed by hash in LEDGERS.sha256
[ ! -e "$LDIR" ] || die "ledger archive directory already exists, refusing to overwrite: $LDIR"
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/out"; : > "$WORK/results.txt"
echo "$lines" | while read -r lvl seed; do
  line=$("$H" "$W" - "$lvl" "$seed" "$WORK/out") || { echo "pd0b-run: harness failed on level $lvl seed $seed" >&2; exit 3; }
  echo "$line" >> "$WORK/results.txt"
done
grep "^PD0L " "$WORK/results.txt" > "$WORK/pd0l.txt" || true; grep "^PD0F " "$WORK/results.txt" > "$WORK/pd0f.txt" || true
NINST=$(echo "$lines" | wc -l)
[ "$(wc -l < "$WORK/pd0l.txt")" -eq "$NINST" ] || die "result count does not match instance count"
VALID=VALID
[ "$(wc -l < "$WORK/pd0f.txt")" -eq "$NINST" ] && [ "$(grep -c " valid=VALID$" "$WORK/pd0f.txt")" -eq "$NINST" ] || VALID=INVALID
# per-instance summary with the retained law record: law=1 iff pd0_ladder_emit_law produced bytes and pd0_ladder_verify_law accepted them
fld() { printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1; }
: > "$WORK/summary.txt"
while read -r pl; do
  lv=$(fld "$pl" level); sd=$(fld "$pl" seed); st=$(fld "$pl" state); lrc=$(fld "$pl" law); lf="$WORK/out/pd0l-$lv-s$sd.law"
  if [ "$lrc" = 0 ] && [ -f "$lf" ]; then law=1; lsha=$(sha256sum "$lf" | cut -d' ' -f1); else law=0; lsha=-; fi
  [ "$st" != REPLICATED ] || [ "$law" = 1 ] || VALID=INVALID
  printf 'level=%s seed=%s state=%s code=%s score=%s score_code=%s law=%s law_sha256=%s\n' "$lv" "$sd" "$st" "$(fld "$pl" code)" "$(fld "$pl" score)" "$(fld "$pl" score_code)" "$law" "$lsha" >> "$WORK/summary.txt"
done < "$WORK/pd0l.txt"
( cd "$WORK/out" && for x in *.ledger; do [ -f "$x" ] && sha256sum "$x"; done ) > "$WORK/LEDGERS.sha256" || true
{
  echo "receipt: PD0B_RUN"
  echo "run_validity: $VALID"
  echo "validity_rule: VALID needs, per instance, a final accepted once, shape dimensions unchanged across final (and, in PD0 mode with the omega world, S* equal to the public table), an audit echoing the final hash, and every refusal counter 0; and law=1 (a law record emitted and accepted by the verifier) for every instance whose ladder_state is REPLICATED"
  [ -z "$NOTE" ] || echo "note: $NOTE"
  echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "harness_commit: $HEAD_FULL"
  echo "world_binary: $W"
  echo "world_binary_sha256: $WSHA"
  echo "seeds_file_sha256: $SSHA"
  echo "ledger_archive: $LDIR"
  echo "ledgers_manifest_sha256: $(sha256sum "$WORK/LEDGERS.sha256" | cut -d' ' -f1)  (LEDGERS.sha256 in the laws directory)"
  echo "mode: external world, PD0 protocol v2 (play and truth from the one world binary; no pd0-truth)"
  echo "--- freeze (learner, ladder, scorer, controls)"
  sh tests/physics0/learner/pd0b_freeze.sh "$H"
  echo "--- seeds file"
  echo "$lines"
  echo "--- per instance: level seed ladder_state checker_code scorer_verdict scorer_code law law_sha256"
  cat "$WORK/summary.txt"
  echo "--- per instance: final bundle hash and the world refusal counters (protocol v2 rev 2)"
  sed "s/^PD0F //" "$WORK/pd0f.txt"
  echo "--- state count per level"
  awk '{ for (i = 1; i <= NF; i++) { split($i, kv, "="); v[kv[1]] = kv[2] } c[v["level"] " " v["state"] " score=" v["score"]]++ } END { for (k in c) printf "%s x%d\n", k, c[k] }' "$WORK/pd0l.txt" | sort
  echo "--- raw harness lines"
  cat "$WORK/pd0l.txt"
} > "$WORK/receipt.txt"
ln "$WORK/receipt.txt" "$OUT" || die "could not create $OUT without overwriting"
mkdir "$LAWDIR" && for x in "$WORK"/out/*; do case "$x" in *.ledger) ;; *) cp "$x" "$LAWDIR"/ ;; esac; done && cp "$WORK/LEDGERS.sha256" "$LAWDIR/LEDGERS.sha256" || die "could not retain the law and json files in $LAWDIR"
mkdir -p "$LDIR" && cp "$WORK"/out/*.ledger "$LDIR"/ || die "could not archive the ledgers in $LDIR"
echo "pd0b-run: wrote $OUT (law and json in $LAWDIR, ledgers in $LDIR)"
[ "$VALID" = VALID ] || { echo "pd0b-run: run is INVALID (see receipt)" >&2; exit 4; }
