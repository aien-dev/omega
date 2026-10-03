#!/bin/bash
# runjob.sh RUN_NAME REPS "gate args..." [more "gate args..."]
# Runs build/omegatool with each arg string, REPS times, in the omega worktree (cwd).
# Immutable: refuses an existing run dir. Never times out or kills the job.
set -u
EV="$HOME/workspace/evidence-out/M18-UNCACHED-20261003"
PHYS="$HOME/workspace/hive-worktrees/qual-physics-6d7cf0d"
RUN="$EV/$1"; REPS="$2"; shift 2
[ -e "$RUN" ] && { echo "run dir exists: $RUN" >&2; exit 2; }
mkdir -p "$RUN"
git rev-parse HEAD > "$RUN/omega-head.txt"
git -C "$PHYS" rev-parse HEAD > "$RUN/physics-head.txt"
git status --short > "$RUN/git-status-before.txt"
sha256sum build/omegatool > "$RUN/omegatool.sha256"
date -u +%FT%TZ > "$RUN/started.txt"
overall=0
for r in $(seq 1 "$REPS"); do
  for args in "$@"; do
    tag=$(echo "$args" | tr -c 'A-Za-z0-9\n' '_')
    log="$RUN/rep$r-$tag.log"
    echo "### rep=$r cmd=build/omegatool $args" > "$log"
    build/omegatool $args >> "$log" 2>&1
    rc=$?
    echo "rep=$r args=$args rc=$rc" >> "$RUN/exit-codes.txt"
    echo "### exit=$rc" >> "$log"
    [ $rc -ne 0 ] && overall=1
  done
done
git status --short > "$RUN/git-status-after.txt"
mkdir -p "$RUN/receipts"
git status --short | awk '{print $2}' | while read -r p; do [ -e "$p" ] && cp -r --parents "$p" "$RUN/receipts/" 2>/dev/null; done
git checkout -- . ; git clean -fdq -e build
echo "$overall" > "$RUN/exit.txt"
date -u +%FT%TZ > "$RUN/finished.txt"
( cd "$RUN" && find . -type f ! -name SHA256SUMS -print0 | sort -z | xargs -0 sha256sum > SHA256SUMS )
exit $overall
