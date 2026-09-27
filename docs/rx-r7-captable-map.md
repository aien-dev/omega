# R7 design: host capability checks onto the AIENOS table

The mapping itself is `spec/r7-aienos-authority-mapping.md`. This note is the
short version.

Status: the native authority now has the pieces this note said were missing,
and the host attack list matches the Linux mint. The Linux mint remains the
oracle and is still in the tree. The reaction world can be pointed at the
native view; that view cannot mint. This note does not connect real AIEN,
and it does not give the reaction world an administrative handle.

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

## What the native authority now carries

The older task table is unchanged. The native authority beside it now has
the pieces the host oracle was checking:

- Subject. A capability presented by the wrong principal is refused.
- Epoch and lease. A stale epoch and an expired lease are refused.
- Privileged rights stay at the office. Mint, revoke-any, reclaim, epoch, and
  clock cannot be delegated. The task table's rights stay read, write, map,
  grant, derive, and revoke.
- Resource width. The native resource is 64 bits. Narrowing drops nothing:
  a value that does not fit in 32 bits is refused.
- The reaction world can receive the validation view. It does not receive
  the office token or the mint path. The Linux mint process remains beside
  it as the oracle.

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

1. The Linux mint process is still the oracle.
2. Subject, epoch, and lease are on the native authority, and the reaction
   view cannot mint.
3. The reaction world can check that authority. The Linux mint stays.
4. Host parity on the attack list is the R7 evidence. Retiring the Linux
   oracle would be a later decision.
