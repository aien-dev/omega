# R7 design: host authority onto the AIENOS capability table

Status: design only. R7 is not claimed. Nothing in this note is a native
capability root, and nothing here connects AIEN, Omega synthesis, or AEGIS.

The Linux process that mints capabilities remains the development stand-in.
The reaction world keeps a read-only validation view (`RxCapRoot`). It does
not hold the admin handle (`RxCapAdmin`), the office token, or the socket
that changes the table.

Grounded in aienos `main` at `07bdb86de0ac30016ead43b321e0a39253b4cc8f`:

- `crates/aienos-kernel/src/abi.rs` — `Handle`, `ResourceId`, `Rights`
- `crates/aienos-kernel/src/caps.rs` — `CapTable`

And in the host reaction world:

- `src/runtime/rx_caproot.h` — `RxCapRef`, `RxCapEntry`, rights, lease, epoch
- `src/runtime/rx_world.h` — an object stores a capability reference and still
  asks the root before a use counts

## Mapping

| Host today | AIENOS table today | What has to be true before R7 |
| --- | --- | --- |
| `RxCapRef` is `{cap_id, generation}` | `Handle` is `{index, generation}`. Generation 0 is never issued. | Same pair. A copied pair is still not permission. |
| Subject on the capability, checked at validation | No subject. A slot has a resource id and rights. | Add a principal, or an equivalent the host check already depends on. |
| Resource is 64 bits | `ResourceId` is a kind plus a 32-bit id. The table's `insert` takes a `u32` resource. | Widen the native resource, or define a lossless narrowing. Do not silently drop the top half. |
| Rights: read, write, effect, delegate, and privileged mint / revoke-any / reclaim / epoch / clock | Rights: read, write, map, grant, derive, revoke. Unknown bits are refused. | Read and write line up. Effect has no native bit. Map and grant have no host bit. Privileged rights must stay on the office, not become ordinary derivable bits. |
| Delegation only narrows rights | `derive` requires `DERIVE` and refuses a right the parent does not hold. | Same rule. Derivation is not minting. |
| Revoking a parent revokes the children | `revoke` walks parent links, children first. A derived slot becomes a tombstone so the chain still reaches the children. | Same cascade. |
| Generation: a reused slot must not honor the old reference. A slot at the maximum generation is retired, not wrapped. | `lookup` requires the slot generation to match. `allocate` skips a slot whose generation is already the maximum. | Same stale rule. This generation belongs to the capability. It is not the object's generation. |
| Lease expiry on a logical clock | No lease and no clock. | Add a lease, or stop depending on one. Today the host refuses an expired lease. That check has nowhere to land. |
| Epoch on the capability, compared with the root epoch | No epoch. | Add an epoch, or stop depending on one. Today the host refuses a stale epoch. |
| Admin operations go through `RxCapAdmin` and require the office | `insert`, `derive`, and `revoke` are methods on the table. The table lives in the kernel. | The reaction world receives a validate path only. It must not receive insert, derive, or revoke. |
| Runtime validation is `rx_caproot_validate`: read the slot, check subject, resource, rights, epoch, and lease | `lookup` reads the slot and checks rights. It does not check a subject, an epoch, or a lease. | The native validate path has to grow those checks before it can replace the host root. |

## What must not move across

- The permission word in the 32-byte shared object. It is written as zero and
  is not authority. Copying it into the native table would not create
  authority either.
- An administrative handle on the reaction world. Cognition may hold a
  capability reference. It may not hold the table, the office, or the mint path.
- A second generation counter for the same thing. The capability generation
  and the object generation are different numbers. Each has one owner.

## End state

The trust root mints. The resident world receives a validation view. Policy
and ratification use the trusted admin path. The Linux mint process is only
the stand-in until that native root exists.

## Order

1. Keep the Linux mint process as the oracle. This is the current code.
2. Add subject, epoch, and lease to the native table, or an equivalent the
   host checks already depend on, without giving the reaction world a way to mint.
3. Decide the resource width and the rights that do not line up (effect, map, grant, privileged office rights).
4. Only then switch `rx_caproot_validate` to that table and retire the mint process.
5. That switch is the R7 claim. This document is not that switch.

## Generation barriers, noted only

Object retirement already advances one generation and refuses the old pair
on both the host read and the cross-engine descriptor. A native handle needs
the same "old generation is dead" rule, which `CapTable` already has for
capabilities. A world-wide generation barrier, in the sense of draining every
engine before a generation is reused, is not designed here. That is later work.
