#!/bin/sh
# Print the PD0B_FREEZE facts in a stable text format (print only; nothing is written).
# usage: pd0b_freeze.sh <harness-binary>
# Output: git tree hash of each frozen directory, sha256 of every tracked file under them
# (sorted by path, C locale), a dirty-tree flag, and the sha256 of the harness binary
# (which embeds the learner, the ladder checker, the scorer and the controls).
set -eu
H=${1:?usage: pd0b_freeze.sh <harness-binary>}
[ -f "$H" ] || { echo "pd0b_freeze: harness binary not found: $H" >&2; exit 2; }
export LC_ALL=C
DIRS="src/physics0/learner src/physics0/ladder src/physics0/score src/physics0/controls"
echo "freeze_head: $(git rev-parse HEAD)"
for d in $DIRS; do echo "tree $d $(git rev-parse HEAD:$d)"; done
dirty=$(git status --porcelain -- $DIRS tests/physics0/verify | wc -l | tr -d ' ')
echo "frozen_dirs_modified_in_worktree: $dirty"
for d in $DIRS; do git ls-files "$d"; done | sort | while read -r f; do echo "file $(sha256sum "$f" | cut -d' ' -f1) $f"; done
echo "harness_binary_sha256: $(sha256sum "$H" | cut -d' ' -f1)"
