#!/bin/sh
# tools/quietlock/install.sh -- install quietlock for the hive (HD-13). Run by the queen, not by workers.
#
#   sh tools/quietlock/install.sh [--dry-run]
#
# 1. builds tools/quietlock/quietlock.c into $HOME/.local/bin/quietlock (temp file + rename)
# 2. backs up ~/.claude/hooks/quiet-guard.sh and ~/.claude/skills/orchestrate-lanes/lanes.sh
#    to <file>.bak.<UTC stamp>
# 3. installs tools/quietlock/quiet-guard.sh as ~/.claude/hooks/quiet-guard.sh
# 4. applies tools/quietlock/lanes-quietlock.patch to lanes.sh (skipped if already applied;
#    refuses, changing nothing, if the patch does not apply cleanly)
# --dry-run prints every step and checks that the patch applies, and changes nothing.
# Never touches the quiet flag itself. POSIX sh; needs cc (or $CC), patch, cmp.
set -u
DRY=0
case "${1:-}" in
	--dry-run) DRY=1 ;;
	"") ;;
	*) echo "usage: sh tools/quietlock/install.sh [--dry-run]" >&2; exit 2 ;;
esac
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/quietlock.c
HOOK_SRC=$HERE/quiet-guard.sh
PATCH=$HERE/lanes-quietlock.patch
BIN_DIR=$HOME/.local/bin
BIN=$BIN_DIR/quietlock
HOOK=$HOME/.claude/hooks/quiet-guard.sh
LANES=$HOME/.claude/skills/orchestrate-lanes/lanes.sh
MARK='HD-13: the quiet flag is a real lock'
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
CC=${CC:-cc}

say() { echo "install: $*"; }
do_() { # run a command, or print it in dry-run mode
	if [ "$DRY" = 1 ]; then say "DRY-RUN would run: $*"; else "$@" || { say "FAILED: $*"; exit 1; }; fi
}

for f in "$SRC" "$HOOK_SRC" "$PATCH"; do [ -f "$f" ] || { say "missing $f"; exit 1; }; done
[ -f "$LANES" ] || { say "missing $LANES"; exit 1; }
command -v patch >/dev/null 2>&1 || { say "patch(1) not found"; exit 1; }

# Decide about the lanes.sh patch before changing anything.
if grep -qF "$MARK" "$LANES"; then
	LANES_ACTION=skip
	say "lanes.sh already patched; leaving it"
elif patch --dry-run -s -o /dev/null "$LANES" < "$PATCH" >/dev/null 2>&1; then
	LANES_ACTION=apply
	say "lanes.sh patch applies cleanly"
else
	say "REFUSED: lanes-quietlock.patch does not apply to $LANES (lanes.sh changed since the patch was made). Nothing changed."
	exit 1
fi

# 1. build
do_ mkdir -p "$BIN_DIR"
do_ "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -o "$BIN.tmp.$$" "$SRC"
do_ mv -f "$BIN.tmp.$$" "$BIN"

# 2. backups
[ -f "$HOOK" ] && do_ cp -p "$HOOK" "$HOOK.bak.$STAMP"
do_ cp -p "$LANES" "$LANES.bak.$STAMP"

# 3. hook
do_ mkdir -p "$(dirname "$HOOK")"
do_ cp "$HOOK_SRC" "$HOOK.tmp.$$"
do_ chmod +x "$HOOK.tmp.$$"
do_ mv -f "$HOOK.tmp.$$" "$HOOK"

# 4. lanes.sh
if [ "$LANES_ACTION" = apply ]; then
	do_ cp -p "$LANES" "$LANES.tmp.$$"
	do_ patch -s "$LANES.tmp.$$" "$PATCH"
	do_ mv -f "$LANES.tmp.$$" "$LANES"
fi

if [ "$DRY" = 1 ]; then
	say "DRY-RUN complete; nothing changed"
	exit 0
fi
# Post-checks
"$BIN" check >/dev/null 2>&1; rc=$?
[ "$rc" = 0 ] || [ "$rc" = 75 ] || { say "installed quietlock does not run (check exit $rc)"; exit 1; }
cmp -s "$HOOK_SRC" "$HOOK" || { say "hook copy differs from source"; exit 1; }
grep -qF "$MARK" "$LANES" || { say "lanes.sh is not patched"; exit 1; }
if grep -q 'rm -f "\$FLAG"' "$LANES"; then say "lanes.sh still removes the flag with rm"; exit 1; fi
say "done: $BIN, $HOOK, $LANES (backups *.bak.$STAMP)"
