# R12 resident seat

Status: first physical chain qualified on GB10. The receipt under `evidence/R12/`
names the exact candidate commit. R8, R10, R11 and R13 are not claimed.

## What is proved

One persistent graphics seat on the machine's own chip takes part in the same
world as the processor:

```text
processor changes A
  -> the seat reaction becomes ready (field-granular dependency on A)
  -> R5 admission grants the Blackwell budget
  -> R7 native AIENOS authority validates the seat's capabilities
  -> the reaction posts a claim naming {A id, generation} and {B id, generation}
  -> the resident seat (launched once, polling) claims it
  -> the seat checks both identities against the shared object table
  -> the qualified 32-bit integer add (IADD3, the M17 vector-add body):
       B.field0 = A.field0 + A.field1
  -> the seat writes B's window and posts a checksummed result notice
  -> the processor accepts: authority again, generations again, the input
     window still canonical, the output window exactly the sum
  -> B is published with the same rules as processor work (R4 crumb,
     worker = RX_SEAT_BLACKWELL)
  -> the processor reaction waiting on B wakes and writes C
```

A, B and C keep one identity space. The seat has no names, generation counter,
or authority of its own. The chip finishing is not the commit; publication is.

## Failure cases run on the chip

| Case | Outcome |
|---|---|
| Same completion delivered twice | consumed, not committed again |
| Torn result notice | refused, B's window restored, rejection recorded |
| Seat leaves and a new seat starts | objects, generations and history intact; chain runs again |
| B's physical window detached while the chip holds the claim | refused; canonical B unchanged |
| Output capability revoked after the claim, before publication | refused, B's window restored |
| A's generation moves while the chip holds the claim | invalidated, recorded |
| A claim naming a retired generation sent straight to the chip | the chip answers with a fault and does not touch B |

Not exercised yet: the seat killed mid-claim, and a graphics channel reset
under load. Both need a way to stop a running channel without unmapping memory
a waiting thread still reads.

## What made the chip see the world

Four bring-up findings, each observed on GB10:

1. **Scoreboards.** Variable-latency reads (constant bank, global memory,
   special registers) finish later than the next instruction issues. The seat
   rewrites every control word: reads set barrier 0, stores hold barrier 1, and
   everything waits on both. Without this the seat used ring addresses and
   counters before they had loaded.
2. **The chip's L2.** The image was GPU-cacheable, and the chip kept polling its
   own L2 copy of the ring tail. It is now allocated with
   `nvrm_alloc_gpu_uncached` (PHYSICS).
3. **Register allowance.** The chip keeps the top two registers of a thread's
   allocation. A value in R46 of a 48-register launch read back wrong, which
   silently zeroed the result checksum. Launches now request two registers more
   than the program uses.
4. **Release order.** `MEMBAR.SC.SYS` before the result tail store, so the output
   window and the whole notice are in memory before the processor can see them.

The NVIDIA disassembler is no longer called. The seat checks its own opening
instructions by opcode.

## Host rules

`make test-r12` runs the same rules with an in-process stand-in in place of the
chip, against the native authority. GitHub runs it. It does not claim R12.
`make test-r12-silicon` runs on the machine and is the only run that may set
`silicon_observed`.
