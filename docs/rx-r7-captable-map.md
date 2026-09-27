# R7 design: host capability checks onto the AIENOS table

The mapping itself is `spec/r7-aienos-authority-mapping.md`. This note is the
short version. R7 is not claimed.

Status: design only. The native capability root is not claimed. The Linux
mint process remains the oracle the reaction world checks. This note does not
connect AIEN, and it does not give the reaction world an administrative handle.

Grounded in:

- host validation: `rx_caproot_validate` in `src/runtime/rx_caproot.c`, after
  the split that keeps `RxCapAdmin` off `RxWorld`
- AIENOS table: `CapTable` in `crates/aienos-kernel/src/caps.rs` on aienos
  `main`, and `Handle` in `crates/aienos-kernel/src/abi.rs`

## What already matches

| Host check | AIENOS table today |
| --- | --- |
| A reference is `{id, generation}` | `Handle` is `{index, generation}`. Generation 0 is never issued. |
| A reused slot must not honor the old reference | `lookup` requires the slot generation to match. A stale handle is `InvalidHandle`. |
| A slot whose generation is already the maximum is retired, not wrapped | `allocate` skips a slot whose generation is `u32::MAX`. The old handle stays invalid. The host helper `rx_cap_generation_advance` copies that rule. |
| Narrowing is allowed. Adding rights is not | `derive` requires `DERIVE` and refuses a right the parent does not hold (`RightsEscalation`). |
| Revoking a parent revokes the children | `revoke` walks parent links across the tables the caller supplies, children first. A capability that has been derived from becomes a tombstone so the chain still reaches the children. |
| Copying the reference is not permission | `lookup` checks rights on the live slot. The host checks the root table. The object stores the reference and still asks the root. |

## What the host oracle has that the table does not

These have to be true in the native root before R7 can be claimed. They are
not true of `CapTable` today.

- Subject. The host refuses a capability presented by the wrong principal.
  The table has a resource id and rights, and no subject.
- Epoch and lease. The host refuses a stale epoch and an expired lease.
  The table has neither a clock nor an epoch.
- Privileged rights stay at the office. Mint, revoke-any, reclaim, epoch, and
  clock cannot be delegated. The table's rights are read, write, map, grant,
  derive, and revoke. That is the right shape for attenuation. It is not yet
  the office split.
- Resource width. The host resource is 64 bits. The table resource is 32.
- The mint socket and the office token. They exist so a process that can read
  the table still cannot mint. The native table is in the kernel. The reaction
  world must receive a validation view, the way it receives `RxCapRoot` today,
  and must not receive insert, derive, or revoke.

## What must not move across

- The permission word on the shared object. Authority stays on the capability,
  which is what `CapTable` already does. Do not copy that word into the table
  and do not start honoring it.
- An administrative handle on the reaction world. Cognition may hold a
  capability reference. It may not hold the table, the office, or the mint path.
- A second generation counter. The handle generation and the object generation
  are different numbers for different things. Each still has only one counter
  of its own.

## Order

1. Keep the Linux mint process as the oracle. (This is the current code.)
2. Add subject, epoch, and lease to the native table, or an equivalent the
   host checks already depend on, without giving the reaction world a way to mint.
3. Only then switch `rx_caproot_validate` to that table and retire the mint process.
4. That switch is the R7 claim. This document is not that switch.
