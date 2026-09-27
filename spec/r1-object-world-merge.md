# R1 merge: one resident object world

Status: specification only. This document does not change the physical layout.
`R1_SHARED_WORLD_PASS` and `R2_CROSS_ENGINE_ABI_PASS` are not claimed.
No graphics-chip run is claimed.

The two layouts below are the ones in the tree today. They are not two
permanent worlds. The later edit merges them. It does not land on the
`m20-shared-world` branch and it does not touch that branch's unfinished
worker.

## What exists

Host reaction world (`src/runtime/rx_world.h`, `RxObject`):

- identity is a slot id plus a generation, never an address
- type, persistence class (ephemeral / resident / durable), and a resource id
- one version for the object and a version per field
- field values, and the causal id of the last writer of each field
- a content digest of type and fields
- wakes come from a side index: object id, generation, field mask, reaction id
- publication is the atomic field update plus that causal record

Coherent world (`OMEGA_SHARED_WORLD_V1`, `OmegaSharedWorldObject` in the
physics `m20-shared-world` tree, not in this branch):

- identity is again object id plus generation
- state is active or revoked
- permissions are read/write bits stored in the object itself
- `region_offset` and `size_bytes` locate the bytes inside the coherent region
- the header stores byte offsets of rings and the table, not pointers
- a ring slot names `object_id`, `object_generation`, `object_offset`,
  `object_length`, plus a sequence and a checksum
- capacity is 64 objects; the host world allows 256

The resident GPU worker on that same branch is not qualified. Building the
worker returns `OMEGA_SW_ERR_WORKER_INCOMPLETE` and clears the sketch.
`omega_sw_run_campaign` returns failure and sets `silicon_observed` to 0.
Address arithmetic and scheduling words are not finished. Do not treat a host
build as a chip result.

## One record

The canonical object is the host record, extended only where the coherent
world has a real requirement:

| Field | Source | Rule |
| --- | --- | --- |
| id, generation | both | This is the only identity. A raw pointer or a GPU address is never an id. |
| type, version, per-field version and last writer | host | Semantic change metadata. |
| persistence class | host | Ephemeral reaction traffic is not durable belief. |
| capability reference | host root | `{cap_id, generation}` only. The shared record does not contain an editable allow/deny bit. The coherent object's permission bits are removed in the merge. |
| publication | host | The visible bytes change together with the version and the causal id, or not at all. |
| dependency | host side index | Kept beside the object, addressed by id, generation, and field mask. Not a scan of every reaction. |
| region offset and length | coherent world | Realization only. Both processors turn that offset into their own mapping. The offset is not the object's name. |

`OmegaSharedWorldHeader` already stores substructure locations as offsets from
the region base. That stays. GPU virtual addresses stay in the launcher's
private arguments, as `OmegaSwWorkerParams` already says, and are not written
into the shared record.

## Rings

Rings stay transport for wake, claim, and publication notices. A slot already
carries a typed message, a monotonic sequence, an epoch, and a bounded window
into an object. That is enough for:

- no pointer as identity
- stale generation refused (`OMEGA_SW_FAULT_STALE_GEN` already exists)
- hostile descriptor rejected (magic, version, message type, checksum)
- window rejected when offset and length leave the object
- replay refused when the sequence is not the next one
- torn payload rejected by the sequence handshake plus the checksum
- authority checked by validating the capability reference, not by trusting a bit in the descriptor

Rings do not become a general request/response call mechanism. The bytes of
the object are the shared state. A transform request that copies a whole
object out and back is the thing this merge removes, not the pattern to grow.

## What the merge must not do

- keep `RxWorld` and `OMEGA_SHARED_WORLD_V1` as two resident universes
- store a CPU pointer or a GPU address as identity
- put a writable permission flag in the shared object
- treat a capability id copied out of the table as authority by itself
- raise the worker's failure return into a success without a real GB10 run
- change the frozen 128-byte descriptor layout in the same step as the host admission work

## Order

1. Agree this record. (This file.)
2. Later, change the coherent object table to carry the semantic fields and drop in-object permission bits.
3. Point the host reaction runtime at that table. Retire the separate host-only object array only when the same id and generation are visible to both processors.
4. Only then finish the resident worker against the merged record, and only count a GB10 run.
