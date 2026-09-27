# One resident object

Status: host CPU implementation. The qualification receipt is what claims the
gate. This is not a graphics-processor result, and it is not the native
capability root.

There are still two records, and they are two aspects of one object:

- `RxObject` in `src/runtime/rx_world.h` is the canonical object. It holds the
  only id and the only generation.
- The 32-byte `OmegaSharedWorldObject` from `OMEGA_SHARED_WORLD_V1` is the
  physical projection of that same object: id, generation, active or revoked,
  a byte offset, and a length. The permission word in that record is written
  as zero and is never read as authority.

No third world type was added. The physics `m20-shared-world` tree was not
edited. Its object table holds 64 records; the host image holds one 32-byte
slot per host object. That slot starts empty. A live object does not need a
window. Attaching a window does not allocate an id and does not advance the
generation. A second attach is refused. Detaching the window leaves the id
and the generation in place. Moving the bytes to another offset leaves the
content digest in place. The 32-byte record and the 128-byte descriptor are
the frozen layout.
The frozen header's transform request and result are rejected on this path.
A ring slot here is a wake, a claim, a publication, a completion, a fault, or
a shutdown.

## What one object holds

Semantic aspect, on `RxObject`: type, field values, per-field versions, the
writer of each field, persistence class, publication version, content digest,
and a capability reference `{cap_id, generation}`.

Physical aspect, on that same `RxObject`, and copied into the 32-byte record
only while a window is attached: region offset, byte length, placement,
machine locality, and coherency. With no window, the 32-byte slot is revoked,
zero-length, and carries the same generation. It is not a second object.
Locality and coherency do not fit the 32-byte record. The coherent record
carries offset, length, and active or revoked. The CPU image's coherency value
means "host image of the shared layout." It does not mean a graphics processor
has mapped it.

Identity is the id plus the generation. It is not the offset, not a CPU
pointer, not a graphics address, and not a ring slot.

The projection does not allocate. Creating an object uses the generation
already on that slot. Retiring advances that same generation and copies it
into the projection. A write to the projection's generation or offset is
rejected as a lie. A write to its permission word changes nothing.

## Descriptor

Publication of an object that has a capability reference places one 128-byte
descriptor on the publication ring. The descriptor names the same id and
generation, the canonical publication version, and the causal record of the
write. The CPU checker refuses a bad magic or version, a sequence that is not
the next one, a torn checksum, a stale generation, a length outside the
object, a capability reference the root does not accept, and a transform call.

Rings are transport. They are not a second copy of the object.

## Not claimed

- The graphics-processor worker was not run.
- The Linux mint process is still the authority oracle.
- Real AIEN and real Omega synthesis are not connected.
- AEGIS was not rewritten.
