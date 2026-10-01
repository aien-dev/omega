# quietlock: the Spark quiet flag as a real lock (HD-13)

The quiet flag (`~/workspace/.spark-quiet`) says "one heavy run owns the machine, everyone else waits".
`quietlock` makes that a lock that the **real entry points** enforce:

- **omega make**: `mk/quiet.mk` runs `quietlock check` for every goal before anything builds.
- **scripts**: run them as `quietlock run -- <cmd>` (refused with exit 75 while someone else holds) or own the machine with `quietlock hold --owner ID --minutes N -- <cmd>`.
- **lanes.sh queue/flush/forge**: once wired as described below.

The decision is never made by matching command text. `quiet-guard.sh` in this folder is a courtesy filter for Claude Code tool calls. If it lets something through by mistake, make still refuses it.

## Commands and exit codes
| Command | Meaning |
|---|---|
| `quietlock hold --owner ID --minutes N [--reason TEXT] -- CMD...` | Takes the flag, runs CMD with `QUIETLOCK_HOLD=<id>`, releases on exit. Exit = CMD's exit. 75 = already held. 77 = more than 20 minutes without a valid approval token. |
| `quietlock check` | 0 when clear, stale, no state dir, or `QUIETLOCK_HOLD` matches. 75 + `QUIETLOCK_REFUSED` when held. |
| `quietlock run -- CMD...` | check, then exec CMD. |
| `quietlock release-stale` | Removes the flag only when the holder pid is dead AND expected_end (UTC, must end in Z) has passed. 3 = live, left alone. |

- **Overrun**: at expected_end, `hold` releases the flag and logs `overrun: hold expired while command still running`. It never kills the command. `--minutes` is therefore a real cap on how long others wait.
- **Approval token** `.spark-quiet-approval` (`owner=`, `max_minutes=`, `expires_at=`): a **policy file, not cryptographic**. Every use and refusal is logged to `.spark-quiet.history`.
- **Not a security boundary**: `QUIETLOCK_DIR` moves the state dir (the tests need this), and anyone can delete the flag. The lock prevents accidents, not malice.
- **Legacy** flags without `hold=`: `QUIET_HOLDER=1` still lets their holder through. This is DEPRECATED and logged, for the transition only.

## Follow-up for the queen (files under ~/.claude, not edited by this PR)
1. **Hook**: install `tools/quietlock/quiet-guard.sh` as `~/.claude/hooks/quiet-guard.sh`, with `quietlock` on PATH (`~/.local/bin/quietlock`) or `QUIETLOCK_BIN` set.
2. **lanes.sh `cmd_release_stale`** (and the old hook's stale branch): replace the read-then-`rm -f` with `quietlock release-stale`. A bare rm can delete a NEW live hold taken between the read and the rm. quietlock does the check and the delete under one flock.
3. **lanes.sh `cmd_flush` / forge**: run the whole flush under the forge's own hold, e.g. `quietlock hold --owner forge --minutes 20 -- <flush loop>`, so every job inherits `QUIETLOCK_HOLD` and its own makes pass. Run each job as `quietlock run -- <job>`. When a job exits 75, or prints `QUIETLOCK_REFUSED` (make exits 2 in that case), record it as **REFUSED_QUIET** and requeue it. Never record it as FAIL.
4. Retire `QUIET_HOLDER=1` and the `allow <regex>` line once every holder uses `quietlock hold`.
