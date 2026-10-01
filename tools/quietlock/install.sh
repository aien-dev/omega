#!/bin/sh
# tools/quietlock/install.sh -- install quietlock for the hive (HD-13). Run by the queen, not by workers.
#
#   sh tools/quietlock/install.sh [--dry-run]
#
# Installs three things as ONE transaction:
#   $HOME/.local/bin/quietlock                       built from tools/quietlock/quietlock.c
#   $HOME/.claude/hooks/quiet-guard.sh               copied from tools/quietlock/quiet-guard.sh
#   $HOME/.claude/skills/orchestrate-lanes/lanes.sh  patched with tools/quietlock/lanes-quietlock.patch
#
# Order:
#   1. preflight: every source exists, patch(1) present, the patch applies to the LIVE lanes.sh
#      (patch -F 0 --dry-run, no fuzz; refuses if lanes.sh changed since the patch was made), the binary builds.
#      All new files are staged as <target>.tmp.<pid> next to their targets. Nothing live changes.
#   2. backups: each existing target is copied to <target>.bak.<UTC stamp>.
#   3. swap: binary, hook, lanes.sh are renamed into place.
#   4. post-checks. If any step of 3-4 fails, every target is restored from its backup
#      (or removed, if it did not exist before), so the hook and lanes.sh are never half-updated.
# Temp, .rej and .orig files are always removed on exit. Never touches the quiet flag itself.
# --dry-run runs step 1 only (it builds into a temp file and deletes it) and changes nothing.
# Test hook: QUIETLOCK_INSTALL_FAILPOINT=build|patch|swap-hook|swap-lanes|postcheck forces a failure there.
# POSIX sh; needs cc (or $CC), patch, cmp.
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
BIN=$HOME/.local/bin/quietlock
HOOK=$HOME/.claude/hooks/quiet-guard.sh
LANES=$HOME/.claude/skills/orchestrate-lanes/lanes.sh
MARK='HD-13: the quiet flag is a real lock'
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
CC=${CC:-cc}
FP=${QUIETLOCK_INSTALL_FAILPOINT:-}
T=tmp.$$

say() { echo "install: $*"; }
fp() { [ "$FP" = "$1" ] && { say "FAILPOINT $1"; return 1; }; return 0; }
cleanup() {
	rm -f "$BIN.$T" "$HOOK.$T" "$LANES.$T" "$LANES.$T.rej" "$LANES.$T.orig"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# ---- 1. preflight (nothing live changes) ----
for f in "$SRC" "$HOOK_SRC" "$PATCH" "$LANES"; do
	[ -f "$f" ] || { say "REFUSED: missing $f. Nothing changed."; exit 1; }
done
command -v patch >/dev/null 2>&1 || { say "REFUSED: patch(1) not found. Nothing changed."; exit 1; }
if grep -qF "$MARK" "$LANES"; then
	LANES_ACTION=skip
	say "lanes.sh already patched; leaving it"
elif patch -F 0 --dry-run -s -o /dev/null "$LANES" < "$PATCH" >/dev/null 2>&1; then
	LANES_ACTION=apply
	say "lanes.sh patch applies cleanly to the live file"
else
	say "REFUSED: lanes-quietlock.patch does not apply to $LANES (lanes.sh changed since the patch was made). Nothing changed."
	exit 1
fi
mkdir -p "$(dirname "$BIN")" "$(dirname "$HOOK")" || { say "REFUSED: cannot create target dirs. Nothing changed."; exit 1; }
fp build && "$CC" -std=gnu11 -Wall -Wextra -Werror -O2 -D_GNU_SOURCE -o "$BIN.$T" "$SRC" ||
	{ say "REFUSED: quietlock.c does not build. Nothing changed."; exit 1; }
{ cp "$HOOK_SRC" "$HOOK.$T" && chmod +x "$HOOK.$T"; } || { say "REFUSED: cannot stage hook. Nothing changed."; exit 1; }
if [ "$LANES_ACTION" = apply ]; then
	{ cp -p "$LANES" "$LANES.$T" && fp patch && patch -F 0 -s -r "$LANES.$T.rej" "$LANES.$T" < "$PATCH" >/dev/null 2>&1; } ||
		{ say "REFUSED: patching the staged lanes.sh copy failed. Nothing changed."; exit 1; }
fi
if [ "$DRY" = 1 ]; then
	say "DRY-RUN complete: patch applies, binary builds, hook staged; nothing changed"
	exit 0
fi

# ---- 2. backups ----
NEW_BIN=1
NEW_HOOK=1
if [ -f "$BIN" ]; then
	cp -p "$BIN" "$BIN.bak.$STAMP" || { say "REFUSED: backup failed. Nothing changed."; exit 1; }
	NEW_BIN=0
fi
if [ -f "$HOOK" ]; then
	cp -p "$HOOK" "$HOOK.bak.$STAMP" || { say "REFUSED: backup failed. Nothing changed."; exit 1; }
	NEW_HOOK=0
fi
cp -p "$LANES" "$LANES.bak.$STAMP" || { say "REFUSED: backup failed. Nothing changed."; exit 1; }

rollback() {
	say "ROLLBACK: $1; restoring every target from its backup"
	if [ "$NEW_BIN" = 1 ]; then rm -f "$BIN"; else cp -p "$BIN.bak.$STAMP" "$BIN"; fi
	if [ "$NEW_HOOK" = 1 ]; then rm -f "$HOOK"; else cp -p "$HOOK.bak.$STAMP" "$HOOK"; fi
	cp -p "$LANES.bak.$STAMP" "$LANES"
	say "rolled back; live files are as before (backups kept as *.bak.$STAMP)"
	exit 1
}

# ---- 3. swap ----
mv -f "$BIN.$T" "$BIN" || rollback "binary swap failed"
{ fp swap-hook && mv -f "$HOOK.$T" "$HOOK"; } || rollback "hook swap failed"
if [ "$LANES_ACTION" = apply ]; then
	{ fp swap-lanes && mv -f "$LANES.$T" "$LANES"; } || rollback "lanes.sh swap failed"
fi

# ---- 4. post-checks ----
"$BIN" check >/dev/null 2>&1
rc=$?
[ "$rc" = 0 ] || [ "$rc" = 75 ] || rollback "installed quietlock does not run (check exit $rc)"
cmp -s "$HOOK_SRC" "$HOOK" || rollback "hook copy differs from source"
grep -qF "$MARK" "$LANES" || rollback "lanes.sh is not patched"
if grep -q 'rm -f "\$FLAG"' "$LANES"; then rollback "lanes.sh still removes the flag with rm"; fi
fp postcheck || rollback "postcheck failpoint"
say "done: $BIN, $HOOK, $LANES (backups *.bak.$STAMP)"
