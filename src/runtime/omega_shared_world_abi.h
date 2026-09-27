/*
 * omega_shared_world_abi.h -- Canonical OMEGA_SHARED_WORLD_V1 ABI (frozen).
 *
 * This is the ONE canonical, pointer-free, fixed-width binary contract for the
 * PHYSICS-owned coherent shared-memory world that is physically visible from
 * both the CPU (where AEGIS executes) and the GPU (where AIEN executes).
 *
 *   AIEN PROPOSES. OMEGA DEFINES. PHYSICS REALIZES. AEGIS VERIFIES.
 *   HARDWARE ACTS. EVIDENCE TEACHES.
 *
 * The *semantics* of this layout are defined by OMEGA doctrine
 * (aien-architecture doctrine/OMEGA.md 9.2 and docs/adr/0014). The *physical
 * realization* of the coherent memory that this layout is interpreted over is
 * owned by PHYSICS (coherent/physics_coherent.*). This header is placed in the
 * PHYSICS repository so that both PHYSICS and OMEGA compile against a single
 * definition with zero duplication (OMEGA already includes PHYSICS headers).
 *
 * ------------------------------------------------------------------------
 * DESIGN LAWS (M20 Stage 1)
 * ------------------------------------------------------------------------
 *  1. POINTER-FREE. No field ever holds a host virtual address, GPU virtual
 *     address, RM object handle, kernel pointer, or physical address. Every
 *     cross-object reference is a *logical* {object_id, object_generation,
 *     offset, length} tuple that PHYSICS resolves internally against a trusted
 *     region, exactly as the M19 capability handle (accelerator-world.md 2.3)
 *     carries no GPU VA or size. This is the memory-level foundation of the
 *     invariant "AIEN can communicate with CPU-side authority without ever
 *     possessing CPU execution authority".
 *
 *  2. FIXED-WIDTH & LITTLE-ENDIAN. Every field is a fixed-width integer stored
 *     host-native little-endian (GB10 is little-endian on both the Arm CPU and
 *     the Blackwell GPU). This is the "Zero Serialization" contract: data is
 *     structured in memory exactly as the consuming execution unit expects, so
 *     both CPU and GPU interpret it directly with no marshalling. This is a
 *     DIFFERENT concern from OMEGA canonical *semantic* serialization
 *     (SPEC-OMEGA-CANON-M4), which is big-endian and exists only to derive
 *     content-addressed SEMANTIC_IDs by hashing. 32-byte id fields below are
 *     raw digests (byte arrays) and are therefore endian-neutral.
 *
 *  3. GENERATION & EPOCH PROTECTED. A world epoch guards the whole world; a
 *     per-object generation guards each logical reference; a per-slot sequence
 *     number guards each ring entry against stale reuse (ABA) and replay.
 *
 *  4. BOUNDED. Ring capacities are compile-time power-of-two constants. Every
 *     offset/length reference is bounds-checked at resolution. No unbounded or
 *     variable-length field appears anywhere in the shared world.
 *
 *  5. DETERMINISTIC & HOSTILE-INPUT CHECKED. Every reader treats the producer
 *     as potentially hostile: it validates magic, version, epoch, generation,
 *     sequence, message type, bounds and checksum before acting, and fails
 *     closed on any mismatch.
 *
 * This header is header-only and has no dependency on NVRM/UVM/M16.
 */
#ifndef OMEGA_SHARED_WORLD_ABI_H
#define OMEGA_SHARED_WORLD_ABI_H

#include <stdint.h>
#include <stddef.h>

/* ---- Versioning & identity ------------------------------------------- */

/* Little-endian 32-bit magic "OSW1" (0x4F 0x53 0x57 0x31). Stored as a u32 so
 * a reader on a little-endian machine sees this exact constant. */
#define OMEGA_SW_MAGIC            0x31575330u      /* 'O''S''W''1' LE */
#define OMEGA_SW_ABI_VERSION      1u

/* 32-byte NUL-padded domain tag for the world state digest (matches the
 * M19 rolling-state style "OMEGA_WORLD_ROLLING_STATE_V1\0..."). */
#define OMEGA_SW_DOMAIN_TAG       "OMEGA_SHARED_WORLD_V1"

/* Cache line / alignment. NVLink-C2C coherence granule on GB10 is 64 bytes. */
#define OMEGA_SW_CACHELINE        64u

/* ---- Ring geometry (bounded, power-of-two) --------------------------- */

/* SPSC ring capacity (entries). MUST be a power of two so index wrap is a mask.
 * 1024 mirrors the discovered GPFIFO capacity used by M19. */
#define OMEGA_SW_RING_CAPACITY    1024u
#define OMEGA_SW_RING_MASK        (OMEGA_SW_RING_CAPACITY - 1u)

/* ---- Message descriptor (128 bytes = 2 cache lines) ------------------ */

/* Message types carried on the rings. */
enum {
    OMEGA_SW_MSG_NONE      = 0x0000u,
    OMEGA_SW_MSG_XFORM_REQ = 0x0001u, /* CPU -> GPU: transform request */
    OMEGA_SW_MSG_XFORM_RES = 0x0002u, /* GPU -> CPU: transform result  */
    OMEGA_SW_MSG_NOP       = 0x0003u, /* keepalive / ordering probe     */
    OMEGA_SW_MSG_SHUTDOWN  = 0x0004u  /* CPU -> GPU: ask worker to exit  */
};

/* Descriptor flags (bitfield). */
enum {
    OMEGA_SW_FLAG_INLINE_PAYLOAD = 0x0001u, /* payload[] carries the operand   */
    OMEGA_SW_FLAG_OBJECT_REF     = 0x0002u, /* object_* names a shared object  */
    OMEGA_SW_FLAG_CHECKSUM       = 0x0004u  /* checksum field is valid CRC32C  */
};

/* Deterministic transform selector (Stage 1 qualification worker only). The
 * worker is NOT AIEN; it proves the communication substrate with a trivial,
 * unambiguous, reproducible operation. */
enum {
    OMEGA_SW_XFORM_XOR_CONST = 0x01u, /* out = in ^ arg_a                 */
    OMEGA_SW_XFORM_ADD_SEQ   = 0x02u  /* out = in + (uint32)sequence      */
};

/*
 * OmegaSharedWorldDesc -- one ring slot, exactly 128 bytes.
 *
 *   Line 0 (control, offset 0x00..0x3F):
 *     the fully self-describing, hostile-input-checkable control record.
 *   Line 1 (payload, offset 0x40..0x7F):
 *     64 bytes of inline payload OR the two 32-byte OMEGA identity digests
 *     (semantic_id, realization_id) that the doctrinal 64-byte intent entry
 *     defines. For the Stage 1 qualification worker the first 8 bytes carry a
 *     u32 input and receive a u32 output; the remainder is reserved.
 *
 * checksum is CRC32C over control bytes [0x00..0x3C) followed by all 64 payload
 * bytes. It is defense-in-depth against torn / partially-published writes; the
 * primary torn-write guard is the acquire/release sequence handshake.
 */
typedef struct OmegaSharedWorldDesc {
    /* ---- line 0: control ---- */
    uint32_t magic;              /* 0x00  OMEGA_SW_MAGIC                    */
    uint16_t abi_version;        /* 0x04  OMEGA_SW_ABI_VERSION              */
    uint16_t msg_type;           /* 0x06  OMEGA_SW_MSG_*                    */
    uint64_t sequence;           /* 0x08  monotonic per-ring sequence       */
    uint32_t world_epoch;        /* 0x10  owning world epoch                */
    uint32_t producer_generation;/* 0x14  producer channel/world generation */
    uint32_t object_id;          /* 0x18  logical object reference          */
    uint32_t object_generation;  /* 0x1C  object generation (ABA guard)     */
    uint32_t object_offset;      /* 0x20  byte offset within the object     */
    uint32_t object_length;      /* 0x24  byte length within the object     */
    uint32_t payload_len;        /* 0x28  bytes of payload[] in use         */
    uint16_t flags;              /* 0x2C  OMEGA_SW_FLAG_*                    */
    uint16_t xform;              /* 0x2E  OMEGA_SW_XFORM_* (Stage 1 worker)  */
    uint32_t arg_a;              /* 0x30  transform operand / constant       */
    uint32_t arg_b;              /* 0x34  transform operand / constant       */
    uint32_t reserved0;          /* 0x38  MBZ                                */
    uint32_t checksum;           /* 0x3C  CRC32C(control[0..0x3C) || payload)*/
    /* ---- line 1: payload ---- */
    uint8_t  payload[64];        /* 0x40  inline payload / identity digests  */
} OmegaSharedWorldDesc;

/* ---- Ring control block (its own cache line, header-separated) ------- */

/*
 * OmegaSharedWorldRing -- one SPSC ring.
 *
 * head/tail are 64-bit monotonically increasing counters (NOT masked); the
 * slot index is (counter & OMEGA_SW_RING_MASK). Using free-running 64-bit
 * counters makes sequence-wrap a non-event for the lifetime of the machine
 * (2^64 exchanges) and lets the consumer detect exactly how many entries are
 * outstanding. The producer owns tail; the consumer owns head (SPSC).
 *
 * Each of head and tail sits alone on its own 64-byte line to prevent false
 * sharing between the producer's and consumer's hot indices. The descriptor
 * array follows, 64-byte aligned.
 */
typedef struct OmegaSharedWorldRing {
    /* producer's published write cursor (release-published) */
    volatile uint64_t tail;
    uint8_t  _pad_tail[OMEGA_SW_CACHELINE - sizeof(uint64_t)];
    /* consumer's consumed read cursor (release-published) */
    volatile uint64_t head;
    uint8_t  _pad_head[OMEGA_SW_CACHELINE - sizeof(uint64_t)];
    /* immutable geometry (written once by PHYSICS at world init) */
    uint32_t capacity;           /* == OMEGA_SW_RING_CAPACITY */
    uint32_t mask;               /* == OMEGA_SW_RING_MASK      */
    uint32_t world_epoch;        /* owning world epoch         */
    uint32_t ring_generation;    /* bumped on world rebuild     */
    uint8_t  _pad_geom[OMEGA_SW_CACHELINE - 16];
    /* bounded slot array */
    OmegaSharedWorldDesc slots[OMEGA_SW_RING_CAPACITY];
} OmegaSharedWorldRing;

/* ---- Fault mailbox --------------------------------------------------- */

enum {
    OMEGA_SW_FAULT_NONE            = 0x0000u,
    OMEGA_SW_FAULT_BAD_MAGIC       = 0x0001u,
    OMEGA_SW_FAULT_BAD_VERSION     = 0x0002u,
    OMEGA_SW_FAULT_STALE_EPOCH     = 0x0003u,
    OMEGA_SW_FAULT_STALE_GEN       = 0x0004u,
    OMEGA_SW_FAULT_BAD_SEQUENCE    = 0x0005u,
    OMEGA_SW_FAULT_BAD_MSGTYPE     = 0x0006u,
    OMEGA_SW_FAULT_OOB_OBJECT      = 0x0007u,
    OMEGA_SW_FAULT_BAD_CHECKSUM    = 0x0008u,
    OMEGA_SW_FAULT_WORKER_TIMEOUT  = 0x0009u
};

typedef struct OmegaSharedWorldFaultMailbox {
    volatile uint32_t fault_code;    /* OMEGA_SW_FAULT_* (0 = clear)          */
    uint32_t          world_epoch;   /* epoch in which the fault was raised   */
    uint64_t          fault_sequence;/* sequence the fault is attributed to   */
    uint32_t          detail_a;      /* fault-specific detail                 */
    uint32_t          detail_b;
    uint8_t           _pad[OMEGA_SW_CACHELINE - 24];
} OmegaSharedWorldFaultMailbox;

/* ---- Shared world header --------------------------------------------- */

/*
 * OmegaSharedWorldHeader -- the fixed prologue at offset 0 of the coherent
 * region. It names the world, its epoch, and the byte offsets (relative to the
 * region base) of each substructure. Offsets are stored (not pointers) so the
 * region is fully relocatable and identically interpretable from any mapping
 * on either processor.
 *
 * Reserved offset slots are declared now for the Stage 2/3/4 rings so their
 * ABI space is frozen without implementing them:
 *   effect_ring, effect_result_ring, proof_ring, evidence_ring,
 *   semantic_store, agent_state.
 * A zero offset means "not present in this ABI revision".
 */
typedef struct OmegaSharedWorldHeader {
    uint32_t magic;                  /* 0x00  OMEGA_SW_MAGIC                  */
    uint16_t abi_version;            /* 0x04  OMEGA_SW_ABI_VERSION            */
    uint16_t header_bytes;           /* 0x06  sizeof(OmegaSharedWorldHeader)  */
    uint32_t world_epoch;            /* 0x08  monotonic world epoch           */
    uint32_t world_generation;       /* 0x0C  bumped on rebuild               */
    uint64_t region_bytes;           /* 0x10  total coherent region size      */
    uint32_t ring_capacity;          /* 0x18  OMEGA_SW_RING_CAPACITY          */
    uint32_t desc_bytes;             /* 0x1C  sizeof(OmegaSharedWorldDesc)==128*/

    /* Byte offsets from region base (Stage 1 populated fields). */
    uint64_t off_cpu_to_gpu_ring;    /* 0x20                                  */
    uint64_t off_gpu_to_cpu_ring;    /* 0x28                                  */
    uint64_t off_fault_mailbox;      /* 0x30                                  */
    uint64_t off_object_table;       /* 0x38  logical object descriptor table */

    /* Reserved for future stages (zero == absent). */
    uint64_t off_effect_ring;        /* 0x40  Stage 3                         */
    uint64_t off_effect_result_ring; /* 0x48  Stage 3                         */
    uint64_t off_proof_ring;         /* 0x50  future                          */
    uint64_t off_evidence_ring;      /* 0x58  future                          */
    uint64_t off_semantic_store;     /* 0x60  future                          */
    uint64_t off_agent_state;        /* 0x68  Stage 4 (tensor/KV handles)     */
    uint64_t reserved_off[10];       /* 0x70..0xBF                            */

    uint8_t  _pad[OMEGA_SW_CACHELINE - (0xC0 % OMEGA_SW_CACHELINE)];
} OmegaSharedWorldHeader;

/* ---- Logical object table -------------------------------------------- */

/*
 * A shared object is a bounded, generation-tagged sub-range of the coherent
 * region. Ring descriptors reference objects by {object_id, object_generation}
 * plus an {offset, length} window; PHYSICS resolves these against this table
 * and rejects any reference whose generation is stale or whose window escapes
 * [0, size_bytes). The table stores a region-relative byte offset, never a VA.
 */
#define OMEGA_SW_MAX_OBJECTS       64u

enum {
    OMEGA_SW_OBJ_REVOKED = 0u,
    OMEGA_SW_OBJ_ACTIVE  = 1u
};

/* Permission bits (mirror OMEGA_PERM_* / PHYSICS allowed_ops). */
enum {
    OMEGA_SW_PERM_READ  = 0x1u,
    OMEGA_SW_PERM_WRITE = 0x2u
};

typedef struct OmegaSharedWorldObject {
    uint32_t object_id;          /* == table index                         */
    uint32_t generation;         /* bumped on revoke/reuse (ABA guard)      */
    uint32_t state;              /* OMEGA_SW_OBJ_ACTIVE / _REVOKED          */
    uint32_t permissions;        /* OMEGA_SW_PERM_*                         */
    uint64_t region_offset;      /* byte offset from region base (NOT a VA) */
    uint64_t size_bytes;         /* bounded window size                     */
} OmegaSharedWorldObject;

typedef struct OmegaSharedWorldObjectTable {
    uint32_t count;
    uint32_t world_epoch;
    uint8_t  _pad[OMEGA_SW_CACHELINE - 8];
    OmegaSharedWorldObject objects[OMEGA_SW_MAX_OBJECTS];
} OmegaSharedWorldObjectTable;

/* ---- Compile-time invariants ----------------------------------------- */

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(OmegaSharedWorldDesc) == 128,
               "shared-world descriptor must be exactly 128 bytes (2 cache lines)");
_Static_assert(offsetof(OmegaSharedWorldDesc, checksum) == 0x3C,
               "checksum must be the last control word of line 0");
_Static_assert(offsetof(OmegaSharedWorldDesc, payload) == 0x40,
               "payload must start on the second cache line");
_Static_assert((OMEGA_SW_RING_CAPACITY & OMEGA_SW_RING_MASK) == 0,
               "ring capacity must be a power of two");
_Static_assert(sizeof(OmegaSharedWorldObject) == 32, "object record must be 32 bytes");
_Static_assert(sizeof(OmegaSharedWorldFaultMailbox) == OMEGA_SW_CACHELINE,
               "fault mailbox must occupy exactly one cache line");
#endif

#endif /* OMEGA_SHARED_WORLD_ABI_H */
