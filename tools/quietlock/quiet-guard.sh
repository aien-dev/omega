#!/bin/sh
# quiet-guard.sh -- PROPOSED drop-in replacement for ~/.claude/hooks/quiet-guard.sh
# (Claude Code PreToolUse hook). The queen installs it; nothing here edits ~/.claude.
#
# This hook is a courtesy filter only. The REAL lock is enforced at actual
# entry points: mk/quiet.mk (every omega make goal runs `quietlock check`),
# scripts run under `quietlock run --` / `quietlock hold`, and lanes.sh
# queue/flush/forge once wired to quietlock. Those never use text matching.
# A command this hook lets through by mistake is still stopped by make.
#
# Decision rules
# - DEPRECATED, transition only: QUIET_HOLDER=1 in the command text and an
#   "allow <regex>" line in the flag still let a command through, as before.
# - Held or clear is decided by `quietlock check` (not by reading the flag here).
#   A stale flag (holder pid dead AND expected_end passed) is first removed by
#   `quietlock release-stale`, the same rule as before.
# - Only Bash tool calls are examined. Edit/Write/NotebookEdit and any other
#   tool are never blocked.
# - The holder passes: QUIETLOCK_HOLD in this hook's environment, or a
#   QUIETLOCK_HOLD=<id> assignment in the command, is handed to `quietlock check`.
# - Heavy = a build/test program at COMMAND POSITION of a shell segment, after
#   removing heredoc bodies and quoted text, skipping VAR=value assignments and
#   wrappers (env sudo nice nohup setsid time timeout ...). The text of
#   `sh|bash -c '...'` is checked as a command. So `echo "make test" >> x.md`,
#   `sed -i 's/make/x/' a.md` and heredocs into handoff notes are never blocked.
# Needs: POSIX sh, awk, sed, grep, jq; quietlock on PATH or $QUIETLOCK_BIN.
input=$(cat)
tool=$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)
[ "$tool" = Bash ] || exit 0
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)
[ -n "$cmd" ] || exit 0

QL=${QUIETLOCK_BIN:-quietlock}
hold=$(printf '%s\n' "$cmd" | grep -o 'QUIETLOCK_HOLD=[A-Za-z0-9._-]*' | head -1 | cut -d= -f2)
if command -v "$QL" >/dev/null 2>&1; then
	"$QL" release-stale >/dev/null 2>&1
	if [ -n "$hold" ]; then
		msg=$(QUIETLOCK_HOLD=$hold "$QL" check 2>&1)
	else
		msg=$("$QL" check 2>&1)
	fi
	[ $? = 0 ] && exit 0
else
	flag=${QUIETLOCK_DIR:-$HOME/workspace}/.spark-quiet
	[ -f "$flag" ] || exit 0
	msg="quietlock is not installed; quiet flag present: $(head -1 "$flag")"
fi

# DEPRECATED transition escapes from the old hook (remove after the transition):
# QUIET_HOLDER=1 in the command text, or an "allow <regex>" line in the flag.
flag=${QUIETLOCK_DIR:-$HOME/workspace}/.spark-quiet
case "$cmd" in *QUIET_HOLDER=1*) echo "quiet-guard: allowed by QUIET_HOLDER=1 (DEPRECATED, use quietlock hold)" >&2; exit 0;; esac
allow=$(grep -m1 "^allow " "$flag" 2>/dev/null | cut -d" " -f2-)
if [ -n "$allow" ] && printf "%s" "$cmd" | grep -Eq -- "$allow"; then
	echo "quiet-guard: allowed by the flag allow line (DEPRECATED, use quietlock hold)" >&2; exit 0
fi

# Strip heredoc bodies, collect `-c '...'` / `-c "..."` texts, strip quoted text.
body=$(printf '%s\n' "$cmd" | awk '
	function delim(s,   d) {
		d = s; sub(/.*<<-?[ \t]*/, "", d); sub(/[ \t;|&].*/, "", d); gsub(/["\047\\]/, "", d); return d
	}
	stop != "" { t = $0; if (strip) sub(/^\t+/, "", t); if (t == stop) stop = ""; next }
	{ print }
	/<<-?[ \t]*["\047]?[A-Za-z_][A-Za-z0-9_]*/ && $0 !~ /<<</ { strip = ($0 ~ /<<-/); stop = delim($0) }')
# Quotes may span lines: flatten newlines to \001 while stripping, then restore.
strip_quotes() { tr '\n' '\001' | sed -e "s/'[^']*'//g" -e 's/"[^"]*"//g' | tr '\001' '\n'; }
inner=$(printf '%s\n' "$body" | tr '\n' '\001' | grep -a -o -e "-c '[^']*'" -e '-c "[^"]*"' | sed -e 's/^-c .//' -e 's/.$//' | tr '\001' '\n')
scan=$( { printf '%s\n' "$body" | strip_quotes; printf '%s\n' "$inner" | strip_quotes; } )

heavy=$(printf '%s\n' "$scan" | awk '
	BEGIN { wrap = " env sudo nice ionice nohup setsid time timeout stdbuf taskset chrt xargs exec command builtin " }
	{
		gsub(/\$\(|`|\(|\)|\{|\}|&&|\|\||[;&|!]/, "\n")
		n = split($0, seg, "\n")
		for (i = 1; i <= n; i++) {
			m = split(seg[i], w, /[ \t]+/); k = 1
			while (k <= m && w[k] == "") k++
			while (k <= m) {
				if (w[k] ~ /^[A-Za-z_][A-Za-z0-9_]*=/) { k++; continue }
				if (index(wrap, " " w[k] " ")) { k++; while (k <= m && (w[k] ~ /^-/ || w[k] ~ /^[0-9.]+[smhd]?$/)) k++; continue }
				break
			}
			if (k > m) continue
			c = w[k]; b = c; sub(/.*\//, "", b)
			if (b ~ /^(make|gmake|cmake|ninja|ctest|gcc|g\+\+|clang|clang\+\+|cc|rustc|verify_all\.sh|est_load.*|run_gate.*|run_.*gates\.sh)$/ || b ~ /^qemu-system/) { print c; exit }
			if (b == "cargo" && w[k+1] ~ /^(build|test|run|bench|clippy)$/) { print c " " w[k+1]; exit }
			if (c ~ /^\.\/test[-_a-z0-9]*$/) { print c; exit }
		}
	}')
[ -n "$heavy" ] || exit 0
echo "QUIETLOCK_REFUSED BLOCKED by quiet-guard: '$heavy' is a build/test command and the Spark quiet flag is held. $msg Do code-only work or wait. The holder runs its commands under 'quietlock hold' (or prefixes QUIETLOCK_HOLD=<its hold id>)." >&2
exit 2
