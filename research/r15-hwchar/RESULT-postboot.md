# R15 hardware characterization: reboot-and-repeat (2026-09-28)

Status: PRE-QUALIFICATION DIAGNOSTIC EVIDENCE. None of this data is R15
qualification evidence.

## Observed

- Before the full power-off, a sustained X925 clock cut existed: under
  sustained load the X925 fell from ~3.89 GHz to ~3.0-3.5 GHz.
- Package power fell with the clock and stayed around 19.5-21.5 W.
- The A725 stayed stable (~2.797 GHz).
- The measuring process had essentially full CPU residency (on_cpu_frac ~1).
- SPBM reads were not responsible (with and without the reader: same cut).
- After a complete power-off (boot 17:34:39), the X925 held ~3.891 GHz for
  the full 2-minute test on both tested cores; package power rose to
  29-32 W when required.
- Historical short-burst performance is back (table below).
- The sustained optimized/A725 ratio is now comfortably below 0.55 (~0.38).

## Unknown

- What originally caused the power-limited state. The MOK owner-key
  enrollment boot sequence and the RTC anomaly on the 09:59 boot are
  hypotheses only, not established causes.

| | afternoon (boot of 09:59) | after full power-off (boot 17:34) |
|---|---|---|
| X925 ref, 2 min sustained | 3.79 GHz first 10 s, 3.51 after 15 s, min 3.17; cut from ~2 s | 3.891 GHz flat, min 3.889 |
| X925 quad4, 2 min sustained | 3.28 / 3.32, min 2.99 | 3.891 flat |
| X925 bursts (cpu 7 and cpu 16) | drop from ~2 s, to 2.87-3.17 GHz | 3.891 flat, reader loaded or not |
| A725 ref | 2.797 flat | 2.798 flat |
| package power under X925 load | held at ~19.5-21.5 W | 29-32 W (reader loaded) |
| max temperature | 40.6 C | 51.2 C (still cool) |
| warm, per call: A725 ref / X925 ref / X925 quad4 | 6.97 / 4.59 / 2.69 us | 7.04 / 4.56 / 2.68 us |
| quad4 / A725 (the 55% goal) | 38.6% | 38.1% |
| quad4 / X925 ref | 0.586 | 0.587 |

Boot facts: power-down 17:33:53, boot 17:34:39; the RTC came up correct
(the 09:59 boot's RTC read July 28); the signed SPBM reader was not
auto-loaded and loaded fine by hand (insmod ok, srcversion D7345BB5C0CCFCB7B177335);
0 NV_ERR_TIMEOUT since boot. Background AI servers (atlas-max-flux,
atlas-spark-max-judge, max-env) were idle and loaded, as in the afternoon.

Meaning for R15 (Drake's decision, 2026-09-28): TARGET_PCT stays 55 and G1
is unchanged. The earlier goal misses occurred while the machine was in the
power-limited state above. Qualification will record machine physical state
every trial (observability only, not an exemption). If the power-limited
state returns before or during qualification, qualification stops and reports
a machine-state failure. There are no selective power cycles.

The first post-boot attempt (runs/hwchar-postboot-aborted-173714) ended after
12 s because the session driving it exited. The orchestrator reran postboot.sh
detached at 17:39:19. Reproduce the table: python3 research/r15-hwchar/compare.py
