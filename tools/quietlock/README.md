# quietlock: the Spark quiet flag as a real lock (HD-13)

The quiet flag (`~/workspace/.spark-quiet`) says "one heavy run owns the machine, everyone else waits".
`quietlock` makes that a lock that the **real entry points** enforce:

- **omega make**: `mk/quiet.mk` runs `quietlock check` for every goal before anything builds.
- **scripts**: run them as `quietlock run -- <cmd>` (refused with exit 75 while someone else holds) or own the machine with `quietlock hold --owner ID --minutes N -- <cmd>`.
- **lanes.sh queue/flush/forge**: once wired as described below.

**The `mk/` gate is the real lock.** It decides with `quietlock check`, never by matching command text. `quiet-guard.sh` in this folder is **advisory only**: a courtesy filter for Claude Code tool calls that pattern-matches command text and can be fooled (quoted or escaped command names, `eval` of built strings, a script that runs make inside). Anything it misses in omega is still refused by the `mk/` gate. For repos without such a gate (aienos, cargo, QEMU runs), the hook is the only guard until their entry points call `quietlock run`.

## Commands and exit codes
| Command | Meaning |
|---|---|
| `quietlock hold --owner ID --minutes N [--reason TEXT] -- CMD...` | Takes the flag, runs CMD with `QUIETLOCK_HOLD=<id>`, releases on exit. Exit = CMD's exit. 75 = already held. 77 = more than 20 minutes without a valid approval token. |
| `quietlock check` | 0 when clear, stale, no state dir, or `QUIETLOCK_HOLD` matches. 75 + `QUIETLOCK_REFUSED` when held. |
| `quietlock run -- CMD...` | check, then exec CMD. |
| `quietlock release-stale` | Removes the flag only when the holder pid is dead AND expected_end (UTC, must end in Z) has passed. 3 = live, left alone. |

- **Overrun**: at expected_end, `hold` releases the flag and logs `overrun: hold expired while command still running`. It never kills the command. `--minutes` is therefore a real cap on how long others wait. **For chip and energy runs this means the quiet period ends at expected_end even though the run continues**, so declare honest minutes, and for runs over 20 minutes get an approval token.
- **Approval token** `.spark-quiet-approval` (`owner=`, `max_minutes=`, `expires_at=`): a **policy file, not cryptographic**. Every use and refusal is logged to `.spark-quiet.history`.
- **Not a security boundary**: `QUIETLOCK_DIR` moves the state dir (the tests need this), and anyone can delete the flag. The lock prevents accidents, not malice.
- **Legacy** flags without `hold=`: `QUIET_HOLDER=1` still lets their holder through. This is DEPRECATED and logged, for the transition only.

## Installing (the queen runs this; workers never edit ~/.claude)
`sh tools/quietlock/install.sh --dry-run` shows every step and checks that the patch applies, without changing anything. `sh tools/quietlock/install.sh` then does the following:
- builds `~/.local/bin/quietlock`
- backs up `~/.claude/hooks/quiet-guard.sh` and `orchestrate-lanes/lanes.sh` to `*.bak.<UTC stamp>`
- installs the hook
- applies `lanes-quietlock.patch`, which covers points 2 and 3 below

It refuses, changing nothing, if lanes.sh has changed since the patch was made. Neither the hook nor the patched lanes.sh ever deletes the flag except through `quietlock release-stale` under the flock.

What the patch and hook do:
1. **Hook**: install `tools/quietlock/quiet-guard.sh` as `~/.claude/hooks/quiet-guard.sh`, with `quietlock` on PATH (`~/.local/bin/quietlock`) or `QUIETLOCK_BIN` set.
2. **lanes.sh `cmd_release_stale`** (and the old hook's stale branch): replace the read-then-`rm -f` with `quietlock release-stale`. A bare rm can delete a NEW live hold taken between the read and the rm. quietlock does the check and the delete under one flock.
3. **lanes.sh `cmd_flush` / forge**: run **each job under its own hold**: `quietlock hold --owner forge --minutes N -- <job>`, with N no more than 20, or N from a valid approval token for longer jobs. Do not wrap the whole flush loop in one hold: it would overrun and lose the quiet period partway through. The job inherits the forge's `QUIETLOCK_HOLD`, so its own makes pass. When `hold` exits 75 (someone else holds), or the output shows `QUIETLOCK_REFUSED` (make exits 2 in that case), record **REFUSED_QUIET** and requeue. Never record it as FAIL.
4. Retire `QUIET_HOLDER=1` and the `allow <regex>` line once every holder uses `quietlock hold`.
