# Not a result: smoke score with superseded tooling

This directory scored the one-task smoke run `smoke-dev01-a` (`protocol=OVERRIDDEN`, dev01 only)
with an early score.sh. Its `results.txt` says "harness: RUN VALID" and counts dev02 to dev10 as
`no_candidate`, so it reads as config A at 0/10. That reading is wrong: only dev01 was generated.

`smoke-dev01-a` and `smoke-dev01-b` were made before the driver kept `rN.transport`,
`weights.check` and `server.json`, so the current score.sh cannot re-verify them.

Kept because evidence is append-only. Use it as a record of the tooling smoke test on 2026-10-14,
never as a score of any configuration.
