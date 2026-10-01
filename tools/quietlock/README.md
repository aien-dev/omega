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
`sh tools/quietlock/install.sh --dry-run` runs the preflight only and changes nothing. `sh tools/quietlock/install.sh` is one transaction:
1. **Preflight** (nothing live changes): all sources present; `patch -F 0 --dry-run` against the LIVE `lanes.sh` (no fuzz; refuses if lanes.sh changed since the patch was made); the binary builds; the new binary, hook and patched lanes.sh are staged as `<target>.tmp.<pid>` next to their targets.
2. **Backups**: each existing target is copied to `*.bak.<UTC stamp>`.
3. **Swap**: binary, hook and lanes.sh are renamed into place.
4. **Post-checks**. If anything in steps 3-4 fails, every target is restored from its backup (or removed if it was new), so the hook and lanes.sh are never half-updated. Temp, `.rej` and `.orig` files are removed on every exit.

The patch is made against the pinned copy `testdata/lanes.sh.base` (and the live hook is pinned as `testdata/quiet-guard.sh.base`). The tests run the installer only on those pinned copies in a fake `$HOME`; they print a non-failing note `applies to live lanes.sh: yes/no`. When lanes.sh changes, re-pin and regenerate the patch. Installing needs the queen, and Drake if it changes hold policy.

What the patch and hook do:
1. **Hook**: `tools/quietlock/quiet-guard.sh` becomes `~/.claude/hooks/quiet-guard.sh`, with `quietlock` on PATH (`~/.local/bin/quietlock`) or `QUIETLOCK_BIN` set. Queueing is not running: `lanes.sh queue / queue-light / idea / ledger / brief / status` is allowed while the flag is held, whatever build words the queued text holds. The exemption is anchored at command position and stops at the next `;`, `&&`, `|`, so a build after it is still checked.
2. **lanes.sh `cmd_release_stale`**: the read-then-`rm -f` becomes `quietlock release-stale`. A bare rm can delete a NEW live hold taken between the read and the rm; quietlock does the check and the delete under one flock.
3. **lanes.sh forge (`flush` and `flush-light`)**: the forge **never takes a hold**. Before each job it waits until `quietlock check` says clear (releasing stale flags through quietlock every `FORGE_WAIT_SECONDS`, default 60 main / 30 light), then runs the job as `quietlock run -- bash -c <job>` with stdin from /dev/null. Jobs that need quiet raise their own hold in their own scripts. Verdict:
   - **REFUSED_QUIET** only when the job exited **75 AND the final line of its log is quietlock's own refusal** (`quietlock: QUIETLOCK_REFUSED ...`). The marker word anywhere else in the log means nothing; such a job is judged by its exit code (PASS / FAIL(rc=N)).
   - A REFUSED_QUIET job is requeued with a counter (`LANE@rN`); after `REQUEUE_MAX` (default 3) requeues it is recorded **REFUSED_QUIET_GAVE_UP** and not requeued.
   - Main flush requeues go to the next flush; the light loop picks them up on its next pass. The light loop keeps the live `LANES_LIGHT_IDLE_MIN` knob (default 30 idle minutes; 0 = exit as soon as the queue is empty, which every test uses under a bounded `timeout`). A zero exit whose log shows NOT_RUN or a skip is still SUSPECT, as in the live lanes.sh.
   - Limit (advisory, recorded): a job that itself prints a line starting `quietlock: QUIETLOCK_REFUSED ` as its very last output and exits 75 would be counted as a refusal. Nothing else can fake it.
4. **Per-job minutes**: a job that raises its own hold declares `--minutes N` (20 or less without an approval token). At expected_end the flag is released even if the job runs on (overrun is logged, the job is never killed). The main forge does not pause the light lane during a main-forge job; whether it should is the queen's call.
5. Retire `QUIET_HOLDER=1` and the `allow <regex>` line once every holder uses `quietlock hold`.
