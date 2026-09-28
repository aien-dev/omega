# R15 hardware characterization: reboot-and-repeat result (2026-09-28)

**Answer: the machine changed, and a full power-off changed it back.**
The X925 sustained clock cut was boot state from the 09:59 boot, not normal
GB10 behavior and not damage.

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

Meaning for R15: with the cut gone, quad4 holds ~38% of the A725 cost under
sustained load, well inside the preregistered TARGET_PCT 55. The earlier goal
misses match the ~20 W boot-state cap. Not yet known: what set that cap at
09:59 (MOK-enrollment firmware path and the RTC reset are candidates), so
qualification should record clocks and package power every trial, and a
future cap shows up as a flat line that falls.

The first post-boot attempt (runs/hwchar-postboot-aborted-173714) ended after
12 s because the session driving it exited. The orchestrator reran postboot.sh
detached at 17:39:19. Reproduce the table: python3 research/r15-hwchar/compare.py
