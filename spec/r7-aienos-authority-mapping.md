# R7 design: host authority onto the AIENOS capability table

Status: host parity is proven. The native authority lives in AIENOS
(`aienos-capability`). The same attack list was run against that authority
and the Linux mint, and the outcomes matched. The reaction world can check
references through the native view. The Linux mint is still built and is
still the oracle. It has not been removed.

This does not connect real AIEN cognition, Omega synthesis, or AEGIS. It
does not start the graphics processor. Continuity across a generation
change is specified separately and does not stop the live organism while
the next generation is being prepared.

The reaction world, when pointed at the native authority, receives the
read-only view. It does not receive the admin handle or the office token.

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

1. The Linux mint process stays as the oracle. It is still in the tree.
2. Subject, epoch, lease, a 64-bit resource, and office rights that cannot be
   delegated are on the native authority. The reaction view cannot mint.
3. Resource width is 64 bits on the native authority. A value that does not
   fit in 32 bits is refused if something tries to narrow it. The older task
   table's rights word is unchanged. Effect and the office rights live on the
   native authority. Map and grant stay on the task table.
4. The reaction world checks the native view when it is started that way.
   The existing heartbeat still checks the Linux oracle. The mint process is
   not retired.
5. The host attack list matching both sides is the R7 evidence. It is not a
   graphics-processor claim. Continuity of the organism across time is the
   separate generation barrier.

## Generation barrier

Object retirement already advances one object's generation and refuses the
old pair. That is not a lineage transition for the whole organism. The
generation barrier keeps one active generation while the next is prepared,
and it becomes strict only when that next generation is promoted. It does
not drain the live organism in order to prepare the candidate. The promotion
right added beside the other privileged rights is what authorizes that
transition. The candidate does not authorize itself.
