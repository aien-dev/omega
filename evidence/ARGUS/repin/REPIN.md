# ARGUS producer re-pin to aienos d39dd5b (native authority observer)

Branch feat/argus-producer, from cf6f45d. Date 2026-09-29.

## What moved
- aienos.lock: c8ab65e (32-bit generations + backport patch) -> d39dd5bc3deb1a24e26a5059477a1342c110c71b
  (aienos main; 64-bit generations with a boot-time floor, observer hook 12add16 native). Same line as omega PR #72.
- tools/argus/aienos-cap-observer-c8ab65e.patch removed. The separate patched authority build
  (ARGUS_CAP_REPO/BASE/PATCH/DIR and its rule) is gone; ARGUS targets link $(AIENOS_CAP_LIB), the
  same aienos.lock extraction the default targets use.
- argus.lock: kept at b375dca. It IS on origin now (`git branch -r --contains b375dca`:
  origin/feat/argus-0, origin/feat/argus-1-spec), so CI can fetch it. Comment updated.
  `git diff --stat b375dca a64bc55 -- native/argus`: docs only,
  ARGUS0_GATES.md +102, HOSTILE_REVIEW.md +87/-5 (2 files, 184+/5-); no code or ABI change.
- .github/workflows/rx-host.yml: aienos.lock and argus.lock added to both trigger path lists.
  Note: the workflow builds no ARGUS target; a pin change re-runs the default R3/R7/R8/R9/... suites.

## 64-bit generations: taken from main, not re-derived
Omega main already carries the 32->64-bit migration (PR #62). Cherry-picked verbatim (-x) from main:
607515f, f5b6ff1, 50e8bea, dce48cf, 9ef49b7 (all applied without conflict). Main's two aienos.lock
commits (96b27c2, 40b64d0) were skipped. The branch still needs a merge of main before it can land.

Omega hand-copies the authority header: src/runtime/aienos_cap.h is a name/layout copy of aienos
native/capability/aienos_capability.h (the aienos header says so). After the cherry-picks it matched
d39dd5b's structs and prototypes except the observer; the observer typedef, AIENOS_CAP_OBS_* (1..8),
aienos_cap_set_observer and aienos_cap_authorize were added. Prototypes and struct bodies were
diffed against d39dd5b: identical. rx_argus.c dropped its local copy of the observer declarations.
No other producer change was needed: rx_argus already carried generations as uint64_t.

Files touched by this round (beyond the cherry-picks): aienos.lock, argus.lock, Makefile,
.github/workflows/rx-host.yml, src/runtime/aienos_cap.h, src/runtime/rx_argus.c,
tools/argus/aienos-cap-observer-c8ab65e.patch (deleted), evidence/ARGUS/repin/.
rx_aegis.c and rx_generation.c: no hand edits (rx_generation.c changed only via cherry-pick f5b6ff1).

## Build
- RX_ARGUS undefined: default test-r7 (oracle match), test-r8 (checks 116 failures 0), test-r9 (barrier
  kept one generation) PASS against d39dd5b (raw/default-r7-r8-r9.txt).
- RX_ARGUS=0, 1, 2 (ARGUS_AUTH=observer) build with -Werror.

## Correctness (RX_ARGUS=2 ingest, ARGUS_AUTH=observer, detectors b375dca; two runs of make test-argus-runtime)
Streams: ~/workspace/argus-runtime-streams/repin-d39dd5b-run1, -run2.

| suite | before (cf6f45d, c8ab65e+patch) | after (d39dd5b), run1 / run2 |
|---|---|---|
| R7 | 320 events, 2 findings (code 13 AUTHORITY_REPLAY) | 329 / 329 events, **0 findings** |
| R8 | 1,989 / 2,393 events, 0 findings | 1,842 / 707 events, 0 findings (204,987 / 202,913 uses -> 1,363 / 532 summaries) |
| R9 | 37 events, 0 findings | 37 / 37 events, 0 findings |

Every run: ring_refused 0, summary_refused 0, lost_transitions 0, late_events 0, ingest_errors 0,
events_rejected 0, received == pushed.
R7 code 13 is gone: the 64-bit authority takes each boot generation from a process-wide monotonic,
time-seeded counter, so the second authority instance (pair_stop + pair_start) and restart start above
every generation already seen. R7 now has 329 events because main's R7 test (607515f) adds authority
operations (observer calls: mint 289, revoke 6, reclaim 3, epoch 2, clock 3, kill 1, restart 2).

## Determinism
Every stream replayed twice gives the same state digest, 0 findings (raw/replay-digests.txt).
R7 live streams are no longer byte-identical across runs: generations are time-seeded. With cap
generations and the summary min-generation field masked, the two R7 runs are identical event for
event (same kinds, caps, codes, order). R8/R9 live streams differ run to run (flush timing), as before.

## Performance
Not re-measured (by instruction). The speed-2 evidence at cf6f45d stands. The observer cost should be
unchanged: the backport patch was the same hook (one NULL check per admin op with no observer; calls
after the table lock is released).
