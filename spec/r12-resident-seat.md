# R12 resident seat

Status: qualified on GB10, including seat loss and channel reset under load.
Each receipt under `evidence/R12/` names its exact candidate commit. R8, R10,
R11 and R13 are not claimed.

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
| Seat killed while it holds a claim (B's window already written, no result) | claim fails once as seat-lost, B's window restored, charge released once, work retried on a new seat and published once |
| The dead seat's claim posted again to the new seat | the chip answers with a fault; the processor refuses the old seat generation |
| Channel destroyed and rebuilt 8 times under continuous load | every reset caught a claim in flight; each lost claim retried and published at most once; B and C end on the last change |

## Seat loss

A seat can go away without leaving: killed, or its channel reset. The world
keeps a **seat generation**. Every claim carries it (the descriptor's
`producer_generation`), the seat is launched with it, and the seat refuses any
claim of another generation. The processor refuses any result of another
generation.

`rx_resident_seat_lost` is called once the chip no longer writes the image:

1. Claims the old seat never took and results nobody accepted are discarded.
2. The seat generation moves. Object generations do not.
3. Every claim the seat still held ends `FAILED` with `RX_ERR_SEAT_LOST`. The
   output window is restored from the canonical object, the resource charge
   is released through the normal end of activation, and a crumb records the
   loss with the claim's cause as parent.
4. With retry, that activation is made ready again, caused by the loss crumb.
   Its new claim carries the new generation. Without retry the loss is final
   for that activation; the next change runs normally.

Every claim ends exactly once: `stats.resident_claims` equals
`stats.resident_closed` whenever the world is quiet.

On the chip, `rx_gpu_seat_kill` destroys the running channel group; the image
and launch memory stay mapped. `rx_gpu_seat_relaunch` builds a new channel on
the same device and memory and launches the seat again on the same image.
The heartbeat block carries a pass counter (`RX_SEAT_HB_LIVE`) so a test can
see the chip has stopped, and a hold word (`RX_SEAT_HB_HOLD`) that makes the
seat keep the claim it has just computed, for fault injection.

On GB10 with driver 580.173.02 a channel teardown under a running seat takes
about four seconds: the driver's polite stop times out (`STOP_CHANNEL`,
`NV_ERR_TIMEOUT` in the kernel log), then the channel is removed. No Xid was
raised. The seat keeps running during those seconds, which is why the load
scenario keeps changing A throughout.

Not exercised: recovery from a fault the chip raises itself (an MMU fault or
Xid), as opposed to a teardown the host starts; and a seat lost before its
first heartbeat.

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
