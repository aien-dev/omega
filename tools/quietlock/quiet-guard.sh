#!/bin/sh
# quiet-guard.sh -- PROPOSED drop-in replacement for ~/.claude/hooks/quiet-guard.sh
# (Claude Code PreToolUse hook). The queen installs it; nothing here edits ~/.claude.
#
# ADVISORY ONLY. This hook is a courtesy filter; it can be fooled (quoted or
# escaped command names, `eval` of built strings, a script path held in a variable,
# programs that start builds without a shell script). It cannot stop work that
# is ALREADY RUNNING when a hold starts (a background suite launched before the
# hold): long suites must call `quietlock check` between stages themselves. The REAL lock is enforced at actual
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
#   shell keywords (if then elif else do while until ...). After a known wrapper
#   (env sudo nice ionice nohup setsid time timeout stdbuf taskset chrt xargs
#   exec eval ...) ANY later heavy word blocks, so wrapper options need no
#   parsing. For sh/bash/dash/zsh/ksh with any -<letters>c cluster (-c, -lc),
#   the command text, quoted or not, is checked. So `echo "make test" >> x.md`,
#   `sed -i 's/make/x/' a.md` and heredocs into handoff notes are never blocked.
# - Scripts and test programs (2026-10-08, three overlaps during a GB10 hold): a
#   script the command runs (`bash|sh FILE`, `./FILE` or any path at command
#   position, `source FILE`, a path after a wrapper) is read, comments dropped,
#   and scanned by the same rules, following the scripts it runs up to 3 levels.
#   Relative paths resolve against the tool call's cwd, `cd DIR` in the same
#   command and the calling script's folder. Test programs are blocked by name:
#   target/**/deps/<bin>, tests/<x>, test-*/*_test(s) files run by path,
#   test-*.sh / *_test(s).sh / run_tests*.sh. `bash -n FILE` (syntax only) runs
#   nothing. The refusal names the holder (from `quietlock check`).
# Needs: POSIX sh, awk, sed, grep, jq; quietlock on PATH or $QUIETLOCK_BIN.
input=$(cat)
tool=$(printf '%s' "$input" | jq -r '.tool_name // ""' 2>/dev/null)
[ "$tool" = Bash ] || exit 0
cmd=$(printf '%s' "$input" | jq -r '.tool_input.command // ""' 2>/dev/null)
[ -n "$cmd" ] || exit 0

QL=${QUIETLOCK_BIN:-$(command -v quietlock 2>/dev/null || echo "$HOME/.local/bin/quietlock")}
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

# The working directory of the tool call (relative script paths resolve against it).
cwd=$(printf '%s' "$input" | jq -r '.cwd // ""' 2>/dev/null)
[ -n "$cwd" ] || cwd=$PWD

# scan_text DEPTH BASEDIR... (text on stdin): print what makes the text heavy, or nothing.
# Same rules for a typed command and for the text of a script it runs (2026-10-08: a script
# that runs cargo test inside, started as `bash scripts/x.sh`, passed the command-word check).
scan_text() {
	_depth=$1; shift
	# Strip heredoc bodies, collect `-c '...'` / `-c "..."` texts, strip quoted text.
	# Comments are dropped only in script files (depth > 1): in a typed command a "#" may sit
	# inside an unbalanced quote, and the command-word check never needed it.
	_body=$(awk -v nocomment=$((_depth > 1)) '
	function delim(s,   d) {
		d = s; sub(/.*<<-?[ \t]*/, "", d); sub(/[ \t;|&].*/, "", d); gsub(/["\047\\]/, "", d); return d
	}
	stop != "" { t = $0; if (strip) sub(/^\t+/, "", t); if (t == stop) stop = ""; next }
	nocomment && /^[ \t]*#/ { next }
	{ print }
	/<<-?[ \t]*["\047]?[A-Za-z_][A-Za-z0-9_]*/ && $0 !~ /<<</ { strip = ($0 ~ /<<-/); stop = delim($0) }')
	# Queueing is not running (queen 2026-10-01): a lanes.sh queue / queue-light / idea / ledger /
	# brief / status call only writes a line for the forge, so its arguments (the queued command text)
	# are data. Each such call is replaced by ':' up to the next unquoted separator, so any command
	# AFTER it (`lanes.sh queue x d 'make'; make`) is still scanned. Only single-quoted args and
	# double-quoted args without $( or backquote are swallowed; anything else stays and is scanned.
	_body=$(printf '%s\n' "$_body" | sed -E \
		"s#(^|[;&|(][[:space:]]*)(([A-Za-z_][A-Za-z0-9_]*=[^ ;&|]* +)*)((ba|da)?sh +)?[^ ;&|'\"]*lanes\\.sh +(queue|queue-light|idea|ledger|brief|status)( +('[^']*'|\"[^\"\$\`]*\"|[^ ;&|'\"()\`\$]+))*#\\1\\2:#g")
	# Quoted text after a shell's -<letters>c flag cluster (sh -c, bash -lc, dash -xc ...) is a
	# command: collect it. Only after a shell name, so `grep -c "make" notes.md` stays allowed.
	_inner=$(printf '%s\n' "$_body" | tr '\n' '\001' \
		| grep -a -o -E -e "(^|[^A-Za-z0-9_])(sh|bash|dash|zsh|ksh)( +-[A-Za-z]+)* +-[A-Za-z]*c[A-Za-z]* +'[^']*'" \
			-e '(^|[^A-Za-z0-9_])(sh|bash|dash|zsh|ksh)( +-[A-Za-z]+)* +-[A-Za-z]*c[A-Za-z]* +"[^"]*"' \
		| sed -e "s/^[^'\"]*.//" -e 's/.$//' | tr '\001' '\n')
	# A quoted single word holding a slash is a path ("$HOME/x/run.sh"): keep it, unquoted.
	_scan=$( { printf '%s\n' "$_body" | unquote_paths | strip_quotes; printf '%s\n' "$_inner" | unquote_paths | strip_quotes; } \
		| if [ "$_depth" -gt 1 ]; then sed -E 's/(^|[[:space:]])#.*$/\1/'; else cat; fi)
	_out=$(printf '%s\n' "$_scan" | awk "$AWK_HEAVY")
	_h=$(printf '%s\n' "$_out" | grep -v -E '^(SCRIPT|CD) ' | head -1)
	if [ -n "$_h" ]; then printf '%s\n' "$_h"; return; fi
	[ "$_depth" -lt 3 ] || return
	_cds=$(printf '%s\n' "$_out" | sed -n 's/^CD //p')
	printf '%s\n' "$_out" | sed -n 's/^SCRIPT //p' | head -20 | while IFS= read -r _w; do
		_f=$(resolve "$_w" "$@" $_cds) || continue
		_h=$(scan_text $((_depth + 1)) "$(dirname "$_f")" "$@" < "$_f")
		if [ -n "$_h" ]; then
			case $_h in *" (in script "*) printf '%s\n' "$_h";; *) printf '%s (in script %s)\n' "$_h" "$_f";; esac
			break
		fi
	done
}
# Flatten newlines to \001 while stripping, so quotes may span lines; then restore.
strip_quotes() { tr '\n' '\001' | sed -e "s/'[^']*'//g" -e 's/"[^"]*"//g' | tr '\001' '\n'; }
unquote_paths() { sed -E -e "s#'([A-Za-z0-9_.~\${}+@%=,-]*/[A-Za-z0-9_./~\${}+@%=,-]*)'#\\1#g" -e "s#\"([A-Za-z0-9_.~\${}+@%=,-]*/[A-Za-z0-9_./~\${}+@%=,-]*)\"#\\1#g"; }
# resolve WORD BASEDIR...: print the readable script file WORD names (absolute, or relative to one
# of the base dirs), or fail. A script is a regular text file starting with #! or named *.sh/*.bash,
# under 1 MiB. ~ and $HOME / ${HOME} are expanded; any other $ makes the word unresolvable.
resolve() {
	_w=$1; shift
	case $_w in "~/"*) _w=$HOME/${_w#\~/};; "\$HOME/"*) _w=$HOME/${_w#\$HOME/};; "\${HOME}/"*) _w=$HOME/${_w#\$\{HOME\}/};; esac
	case $_w in *'$'*|'') return 1;; esac
	case $_w in /*) set -- "$_w";; *) _r=$_w; _l=""; for _b in "$@"; do
			case $_b in "~/"*) _b=$HOME/${_b#\~/};; /*) ;; *) _b=$cwd/$_b;; esac; _l="$_l
$_b/$_r"; done; set -- $(printf '%s\n' "$_l" | sed '/^$/d');;
	esac
	for _p in "$@"; do
		[ -f "$_p" ] && [ -r "$_p" ] || continue
		[ "$(wc -c < "$_p")" -le 1048576 ] || continue
		case $_p in *.sh|*.bash) printf '%s\n' "$_p"; return 0;; esac
		[ "$(head -c 2 "$_p")" = '#!' ] && { printf '%s\n' "$_p"; return 0; }
	done
	return 1
}

# Per shell segment: skip VAR=value words and shell keywords; the next word is the command.
# - Known wrapper (env sudo nice ...): block if ANY later word is heavy (no per-option parsing);
#   a later shell word is handled like a shell at command position; a later word with a slash is
#   a script to look into.
# - Shell (sh bash dash zsh ksh) with a -<letters>c cluster: the next unquoted word is a command.
#   A shell with a file argument runs that file: blocked by name (test program) or looked into
#   (SCRIPT line); -n (syntax check only) runs nothing.
# - source / . FILE: the file is looked into.
# - A command word with a slash runs that file: blocked by name or looked into.
# - `cd DIR` segments are printed (CD lines) so `cd d && bash x.sh` finds d/x.sh.
# Test programs by name: target/**/deps/<bin>, tests/<x> or test-*/ *_test(s) files run by path,
# and test-*.sh / *_test(s).sh / run_tests*.sh scripts.
AWK_HEAVY='
	BEGIN {
		wrap = " env sudo doas nice ionice nohup setsid time timeout stdbuf taskset chrt xargs exec command builtin eval watch "
		kw = " if then elif else fi do done while until for in case esac ! { } "
		shells = " sh bash dash zsh ksh "
		interps = " python python3 node perl ruby "
	}
	function interpargs(k, m,   j) { # w[k] is an interpreter (python3, $PY ...): its test file or test module
		for (j = k + 1; j <= m; j++) {
			if (w[j] == "-m" && w[j+1] ~ /^(pytest|unittest)$/) return w[k] " -m " w[j+1]
			if (w[j] ~ /^-[A-Za-z0-9.]*[ecE]/) return ""  # inline program (perl -pi -e, python3 -c): files are data
			if (w[j] ~ /^[-<>0-9]/) continue
			return testname(w[j]) ? w[j] " (test program, run by " w[k] ")" : ""
		}
		return ""
	}
	function isinterp(c) { return index(interps, " " base(c) " ") || c ~ /^\$[A-Za-z_]/ }
	function base(s) { sub(/.*\//, "", s); return s }
	function isheavy(c, nxt, notest,   b) {
		b = base(c)
		if (b ~ /^(make|gmake|cmake|ninja|ctest|gcc|g\+\+|clang|clang\+\+|cc|rustc|verify_all\.sh|est_load.*|run_gate.*|run_.*gates\.sh)$/ || b ~ /^qemu-system/) return c
		if (b == "cargo" && nxt ~ /^(build|test|run|bench|clippy|nextest)$/) return c " " nxt
		if (c ~ /^\.\/test[-_a-z0-9]*$/) return c
		if (!notest && testname(c)) return c " (test program)"
		return ""
	}
	function testname(c,   b) { # a test program or test script, by name
		b = base(c)
		if (c ~ /(^|\/)target\/(.*\/)?deps\/[^\/]+$/) return 1
		if (b ~ /^(test[-_].*|.*[-_]tests?|run[-_]?tests.*)\.(sh|bash|py)$/) return 1
		if (index(c, "/") && (c ~ /(^|\/)tests?\/[^\/]/ || b ~ /^tests?[-_]/ || b ~ /[-_]tests?$/)) return 1
		return 0
	}
	function shellargs(k, m,   j, h) { # w[k] is a shell; return heavy text or "" (prints SCRIPT)
		for (j = k + 1; j <= m; j++) {
			if (w[j] ~ /^-[A-Za-z]*c[A-Za-z]*$/) { h = isheavy(w[j+1], w[j+2]); return h == "" ? "" : h " (in " base(w[k]) " " w[j] ")" }
			if (w[j] ~ /^-[A-Za-z]*n[A-Za-z]*$/) return ""
			if (w[j] ~ /^[-+]o$/) { j++; continue }
			if (w[j] ~ /^[-+]/) continue
			if (w[j] == "<") { if (w[j+1] != "") print "SCRIPT " w[j+1]; return "" }  # bash < FILE runs FILE
			if (w[j] ~ /^<[^<]/) { print "SCRIPT " substr(w[j], 2); return "" }
			if (w[j] ~ /^[0-9]*[<>]/) { if (w[j] ~ /^[0-9]*[<>]+&?$/) j++; continue }
			if (testname(w[j])) return w[j] " (test script, run by " base(w[k]) ")"
			print "SCRIPT " w[j]; return ""
		}
		return ""
	}
	{
		# Text after ")" or "}" is not a command position for the new file rules (X=$(cd a)/tests/f,
		# cmp <(...) tests/f, ${HOME}/tests/f): such segments start with \002 and get only the
		# command-word check the hook always had.
		s = $0  # ${NAME} -> $NAME first, so its "}" does not end a command
		while (match(s, /\$\{[A-Za-z_][A-Za-z0-9_]*\}/)) s = substr(s, 1, RSTART) substr(s, RSTART + 2, RLENGTH - 3) substr(s, RSTART + RLENGTH)
		$0 = s
		gsub(/[)}]/, "\n\002")
		gsub(/\$\(|`|\(|\{|&&|\|\||[;&|!]/, "\n")
		n = split($0, seg, "\n")
		for (i = 1; i <= n; i++) {
			closed = (substr(seg[i], 1, 1) == "\002"); if (closed) seg[i] = substr(seg[i], 2)
			m = split(seg[i], w, /[ \t]+/); k = 1
			# cargo -q test, cargo +nightly test: the subcommand is the first word that is not an option.
			for (q = 1; q < m; q++) if (base(w[q]) == "cargo") { r = q + 1; while (r <= m && w[r] ~ /^[-+]/) r++; if (r > q + 1 && r <= m) w[q+1] = w[r] }
			while (k <= m && (w[k] == "" || w[k] ~ /^[A-Za-z_][A-Za-z0-9_]*=/ || index(kw, " " w[k] " "))) k++
			if (k > m) continue
			if (closed) { h = isheavy(w[k], w[k+1], 1); if (h != "") { print h; exit }; continue }
			h = isheavy(w[k], w[k+1]); if (h != "") { print h; exit }
			b = base(w[k])
			if ((b == "command" || b == "type" || b == "which") && w[k+1] ~ /^-[vVp]*$/) continue  # lookup, not a run
			if (b == "cd" && w[k+1] != "" && w[k+1] !~ /^-/) { print "CD " w[k+1]; continue }
			if (b == "source" || b == ".") { if (w[k+1] != "") print "SCRIPT " w[k+1]; continue }
			if (index(wrap, " " b " ")) {
				# Any later heavy word blocks (as always). The file rules apply only to the wrapped
				# command itself: the first word that is not an option, an option argument, a
				# number (timeout 60, taskset 0-3), VAR=value, another wrapper or a redirect, so
				# `timeout 5 cat tests/a.txt` and `nohup x > tests/out.log` stay allowed.
				found = 0; skip = 0
				for (j = k + 1; j <= m; j++) {
					h = isheavy(w[j], w[j+1], 1); if (h != "") { print h " (after " b ")"; exit }
					if (found) continue
					if (skip) { skip = 0; continue }  # an option argument or redirect target (still checked above)
					if (w[j] ~ /^[0-9]*[<>]/) { if (w[j] ~ /^[0-9]*[<>]+&?$/) skip = 1; continue }
					if (w[j] ~ /^-/) { if (w[j] ~ /^-[ugncskIPLdp]$/) skip = 1; continue }
					if (w[j] ~ /^[0-9][0-9a-fx.,:-]*[smhd]?$/ || w[j] ~ /^[A-Za-z_][A-Za-z0-9_]*=/ || index(wrap, " " base(w[j]) " ")) continue
					found = 1
					if (index(shells, " " base(w[j]) " ")) { h = shellargs(j, m); if (h != "") { print h " (after " b ")"; exit }; continue }
					if (isinterp(w[j])) { h = interpargs(j, m); if (h != "") { print h " (after " b ")"; exit }; continue }
					if (testname(w[j])) { print w[j] " (test program) (after " b ")"; exit }
					if (index(w[j], "/")) print "SCRIPT " w[j]
				}
			} else if (index(shells, " " b " ")) {
				h = shellargs(k, m); if (h != "") { print h; exit }
			} else if (isinterp(w[k])) {
				h = interpargs(k, m); if (h != "") { print h; exit }
			} else if (index(w[k], "/")) {
				print "SCRIPT " w[k]
			}
		}
	}'
heavy=$(printf '%s\n' "$cmd" | scan_text 1 "$cwd")
[ -n "$heavy" ] || exit 0
echo "QUIETLOCK_REFUSED BLOCKED by quiet-guard: '$heavy' is a build/test command and the Spark quiet flag is held by someone else. $msg Do code-only work or wait. The holder runs its commands under 'quietlock hold' (or prefixes QUIETLOCK_HOLD=<its hold id>)." >&2
exit 2
