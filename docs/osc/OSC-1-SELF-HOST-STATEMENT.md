# OSC-1 self-hosting statement

**OSC-1 slice; not a general Omega compiler; no self-hosting.**

Question: can the OSC-1 compiler compile any part of itself?

**Answer: no.** No part of the OSC-1 compiler's source is written in the OSC-1
language, and the OSC-1 language cannot express it. Reasons, each a missing
language feature, not a missing test:

1. **No bytes, strings or text.** The lexer and parser consume source bytes and
   produce names and diagnostics. OSC-1 has integer scalars, bool and fixed
   arrays of at most 64 integers. It has no `u8` slice over input, no string
   type and no way to receive a source file.
2. **No structs, unions or tagged data.** The IR (`osc_ir.h`) is a tree of
   structs and enums (`OscUnit`, `OscFunc`, `OscInsn`). OSC-1 has no aggregate
   type other than `own [T; N]` with N <= 64, so it cannot hold an IR.
3. **No growable or large storage.** The compiler needs tables of thousands of
   entries (4096 instructions per function, 480 vregs). OSC-1 allocations are
   fixed-size pool slots of 64 cells, at most 64 live.
4. **No recursion and no forward calls.** The parser is recursive descent. OSC-1
   calls only functions defined earlier in the unit.
5. **No output channel.** The back end writes machine code into a buffer the
   host maps executable, and the receipt writer writes files. OSC-1 programs can
   only return a scalar; they have no I/O, no capabilities and no effects.
6. **No checked contracts.** `requires`/`ensures` are recorded text in OSC-1
   (OSC-0 decision 3). By OSC-0 rule, no production C migrates into Omega
   Systems Core until contract enforcement exists (OSC-2).

What could be written in OSC-1 today is a re-implementation of a few pure
integer helpers that the compiler uses (for example the width mask or the
"is this value representable in type T" check). That would be new code that
happens to compute the same numbers, not the compiler compiling its own
source, and it is not claimed as self-hosting.

What this means for the roadmap. Self-hosting is OSC-14 in the OSC-0 migration
order (audit III.7). The determinism gate II.9 defines the evidence it will
need: `compiler_n` built by `compiler_{n-1}` producing byte-identical output
on a fixed corpus. The M6 `OMEGA_SELF_HOST` result is a fixed-output self-copy
check and is never evidence of a compiler (OSC-0 decision 2, ROADMAP §3 M6
correction note). OSC-1 does not change that and does not touch M6.
