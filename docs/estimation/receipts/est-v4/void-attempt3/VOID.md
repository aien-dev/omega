# ESTIMATION v4, attempt 3: VOID (foreign host test in the D1 window); no seed left

Attempt 3 was the last recollection the protocol allows (section 7, n = 2: D1 seed
0xE5C4D3, D2 seed 0xD2E5C6). Before any attempt-3 data existed, `void-attempt2/VOID.md`
fixed its validity standard: section 7 pre-check VALID **and** no foreign compiler,
test, QEMU or build process in the watcher log during the window **and** no slip
reported by the coordinator; a failure of that standard ends v4 INCONCLUSIVE.

## What happened

- 14:23:14Z: the launcher saw three quiet checks 20 s apart (no flag, no est_load, no
  GPU lock holder, no gate/test/QEMU process), posted to the lane board, and the
  collector took the quiet flag and started `est_load` (seed 0xE5C4D3).
- 14:23:18Z-14:24:00Z: another lane's `make test-compiler` (worktree
  omega-lane30-handles) ran `test_osc_model`, `test_osc_backend_asan`,
  `test_osc_compil...` at up to 100 % of one core (`watch-d1.log`). It started 4 s
  after the flag was taken.
- 14:24:08Z: lane15 stopped its own collection (SIGTERM to the collector; its trap
  stopped est_load and removed the flag). 54 lines were recorded. The data was never
  pre-checked, fitted or scored, and no D2 was collected.

The window fails the pre-declared standard, so this collection is void. There is no
recollection seed left, so **v4 ends INCONCLUSIVE** (section 7). The seed was not reused
and no new seed was invented: that would move a rule fixed before the data.

Raw folder, kept on record with an `INVALID` note:
`evidence/EST4/raw/20261001T142314Z-est4-fit-silicon/`.
