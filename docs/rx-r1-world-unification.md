# R1 merge specification: one resident object world

Status: design only. This note does not change the physical layout.
It is grounded in the code that exists now, not in a new design.

Host evidence below is the reaction runtime on omega `main` (`src/runtime/rx_world.h`).
Physical evidence below is the frozen layout in physics branch `m20-shared-world`
(`shared_world/omega_shared_world_abi.h`, `OMEGA_SHARED_WORLD_V1`).
Neither one is the finished shared world. They must become one world.
This change does not edit that frozen layout.

## What each side actually is today

The host reaction world (`RxObject`, `RxWorld`) is the semantic world the
reactions already use:

- logical id plus generation
- type, eight versioned fields, content digest
- persistence class: ephemeral, resident, durable
- a resource id that capabilities are checked against
- publication is atomic under the world lock
- a stale generation is refused
- reactions subscribe to an object and a field mask
- capability references are `{cap_id, generation}` and are checked in the mint,
  not by a permission bit stored beside the object

The physics world (`OmegaSharedWorldObject`) is a coherent memory record the
CPU and the GPU can both read:

- logical id plus generation
- active or revoked
- a read/write permission bit
- `region_offset` and `size_bytes`: where the bytes live inside the coherent
  region

`region_offset` is a byte offset from the region base. It is not a CPU pointer
and it is not a GPU address. That part is already right. It is still a
physical placement fact. It must not become the object's name.

The rings (`OmegaSharedWorldDesc`) carry a logical `{object_id, object_generation,
object_offset, object_length}` plus a sequence number. They are a transport
for a request, a result, a keepalive, or a shutdown. They are not a second
copy of the object. They must stay that way: wake, claim, and publication
transport. They must not turn into a general call mechanism where one side
asks the other to run a subsystem.

## The split that has to disappear

These are two id spaces today:

- `RxObject.id` lives only in the host process.
- `OmegaSharedWorldObject.object_id` lives only in the coherent region, and
  the table holds 64 objects while the host world holds 256.

A reaction cannot name a GPU-visible object, and a GPU worker cannot name a
reaction object. Unification means one id and one generation for one object,
visible to both, with one rule for "this reference is stale."

## Canonical record

One resident object has one logical identity. The record that both sides
agree on has to be able to hold:

| Fact | Already in the host reaction object | Already in the physics object | Where it belongs after the merge |
| --- | --- | --- | --- |
| Logical id | yes | yes (table index) | the identity. Never a pointer, never a GPU address, never `region_offset` |
| Generation | yes | yes | the identity's epoch. Stale generation is refused on both sides |
| Type | yes | no | semantic record |
| Field values and per-field versions | yes | no | semantic record. This is what reactions subscribe to |
| Content digest | yes | no | semantic record |
| Persistence class | yes | no | semantic record. Ephemeral work must not become durable by being published |
| Authority reference | resource id, capability checked in the mint | a permission bit on the object | a `{cap_id, generation}` reference. The bit on the physics object is not authority |
| Publication / liveness | `live` | active / revoked | one publication state |
| Who wakes on a change | subscription index beside the object | no | dependency record, beside the object, not a scan of every reaction |
| Where the bytes are | no | `region_offset`, `size_bytes` | physical realization only. Private to the machine that maps the region |

The permission bits on `OmegaSharedWorldObject` (`OMEGA_SW_PERM_READ` /
`OMEGA_SW_PERM_WRITE`) are editable facts in shared memory. They must not
survive as the authority check. Authority stays in the capability root.
The object may name the capability that must be held. It must not carry a
boolean that a writer can flip.

## What is explicitly not merged by editing the frozen header

`OmegaSharedWorldObject` is 32 bytes and the header is treated as frozen by
the physics side. Do not widen that struct in place to hold fields, digests,
and capability references. That would break the coherent layout and pretend
the GPU path is done.

The later implementation step, not this one, adds a semantic record next to
the existing physical record, keyed by the same `{object_id, generation}`:

- the physical record keeps `region_offset` and `size_bytes` and stays the
  only place that says where bytes sit
- the semantic record holds type, fields, versions, persistence, publication
  state, and the capability reference
- both records are rejected together when the generation does not match
- raw addresses never appear in either record

Until that step lands, the host `RxWorld` remains the reference the reaction
tests run against. The physics world remains the coherent-memory experiment.
No gate here claims they are already one world.

## Invariants the merge has to keep

- A reference is `{id, generation}`. A pointer or a GPU address is never an id.
- A stale generation is refused and produces no write.
- A torn publication is refused. The host side does this with one lock and a
  version check. The physics side does this with the ring sequence and checksum.
  The merged world needs both: one publication of the semantic fields, and the
  existing ring handshake when a CPU or a GPU is the publisher.
- An authority reference is checked in the capability root. Copying the
  reference is not permission.
- Offsets and lengths are bounded by the object's `size_bytes`.
- A replayed ring sequence is refused. That check already exists on the ring
  and stays on the transport, not on the object identity.
- Dependency wake-up names the object and the fields that changed. It does
  not scan every object.
- Ephemeral reaction traffic does not, by itself, change durable state.

## Not in this step

- No edit to `omega_shared_world_abi.h`.
- No claim that the GPU worker is qualified. The resident GPU worker still
  returns failure rather than a silicon result.
- No second runtime and no global event queue.
- R1, R2, and R12 stay unclaimed until the records share one identity and a
  physical GB10 run shows a CPU publish waking a resident GPU worker through
  that identity.
