#!/bin/sh
# HD-09 mutant proof for tests/runtime/rx_deadline_cancel.c.
#
# Copies the tracked tree to a temp dir (the real tree is never modified),
# builds and runs the unmutated test there first (it must PASS, otherwise no
# kill means anything), then applies each mutant M1-M4 to the copy's
# src/runtime/rx_world.c, rebuilds only the test binary, and requires the
# test to FAIL. A mutant that does not apply or does not build is reported
# as NOT_APPLIED / BUILD_FAIL and counts as a survivor.
#
#   M1  `>` -> `>=` in the run_one deadline check
#   M2  the run_one deadline check removed (condition forced false)
#   M3  cancel recorded, but the activation falls through to publishing
#       (writes staged and committed)
#   M4  cancel without refund (charge marked released before end_activation,
#       so uncharge skips it)
#
# Prints MUTANTS_KILLED k/4; exit 0 only when k == 4.
set -u

root=$(cd "$(dirname "$0")/../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/rx_deadline_mut.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM

base="$tmp/base"
mkdir -p "$base"
(cd "$root" && git ls-files) > "$tmp/files" || { echo "MUTANTS: cannot list tree"; exit 1; }
(cd "$root" && tar -cf - -T "$tmp/files") | (cd "$base" && tar -xf -) ||
	{ echo "MUTANTS: copy failed"; exit 1; }

src=src/runtime/rx_world.c
check='if (d->need.deadline .. w->budget.logical_tick > d->need.deadline) {'   # BRE; '..' matches the &&
grep -q 'if (d->need.deadline && w->budget.logical_tick > d->need.deadline) {' "$base/$src" ||
	{ echo "MUTANTS: deadline check not found in $src"; exit 1; }

build_run() { # dir -> 0 pass, 1 fail, 2 build error
	make -C "$1" --no-print-directory build/rx_deadline_cancel > "$1/.mut_build.log" 2>&1 || return 2
	(cd "$1" && ./build/rx_deadline_cancel) > "$1/.mut_run.log" 2>&1 && return 0
	return 1
}

build_run "$base"; rc=$?
if [ $rc -ne 0 ]; then
	echo "BASELINE: FAIL (rc $rc); mutants not judged"
	tail -n 20 "$base/.mut_build.log" "$base/.mut_run.log" 2>/dev/null
	echo "MUTANTS_KILLED 0/4"
	exit 1
fi
echo "BASELINE: PASS"

# Apply mutant $1 to the copy in $2. Edits only inside the HD-09 block
# (from the deadline check line to the first line that is exactly "    }").
mutate() {
	f="$2/$src"
	case "$1" in
	M1) sed "s/$check/if (d->need.deadline \&\& w->budget.logical_tick >= d->need.deadline) {/" "$f" > "$f.new" ;;
	M2) sed "s/$check/if (0) {/" "$f" > "$f.new" ;;
	M3) awk -v pat='if (d->need.deadline && w->budget.logical_tick > d->need.deadline) {' '
		index($0, pat) { inb = 1; print; next }
		inb && $0 == "    }" { inb = 0; print; next }
		inb && ($0 ~ /set_state\(w, r, RX_CANCELLED\);/ || $0 ~ /end_activation\(w, rid\);/ ||
		        $0 ~ /^[ ]*return;[ ]*$/) { next }
		{ print }' "$f" > "$f.new" ;;
	M4) awk -v pat='if (d->need.deadline && w->budget.logical_tick > d->need.deadline) {' '
		index($0, pat) { inb = 1; print; next }
		inb && $0 == "    }" { inb = 0; print; next }
		inb && $0 ~ /end_activation\(w, rid\);/ { print "        r->holding = false;" }
		{ print }' "$f" > "$f.new" ;;
	esac
	if cmp -s "$f" "$f.new"; then rm -f "$f.new"; return 1; fi
	mv "$f.new" "$f"
}

killed=0
for m in M1 M2 M3 M4; do
	d="$tmp/$m"
	mkdir -p "$d"
	(cd "$base" && tar -cf - -T "$tmp/files") | (cd "$d" && tar -xf -)
	if ! mutate "$m" "$d"; then echo "$m: NOT_APPLIED (survivor)"; continue; fi
	build_run "$d"; rc=$?
	case $rc in
	1) killed=$((killed + 1)); echo "$m: KILLED ($(grep -c '  FAIL  ' "$d/.mut_run.log") failed checks)" ;;
	0) echo "$m: SURVIVED" ;;
	*) echo "$m: BUILD_FAIL (survivor)"; tail -n 5 "$d/.mut_build.log" ;;
	esac
	rm -rf "$d"
done

echo "MUTANTS_KILLED $killed/4"
[ "$killed" -eq 4 ]
