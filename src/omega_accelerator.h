#ifndef OMEGA_ACCELERATOR_H
#define OMEGA_ACCELERATOR_H

#include "omega_types.h"
#include "omega_machine.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OMEGA_ACCEL_MAX_WINDOWS      8
#define OMEGA_ACCEL_MAX_QUEUES       2

/* Resource Type for Accelerator Capability (M3/M15) */
#define RES_ACCELERATOR              0x00000005

/* Accelerator Operations */
#define ACCEL_OP_PROBE               0x00000001
#define ACCEL_OP_MAP_DMA             0x00000002
#define ACCEL_OP_UNMAP_DMA           0x00000004
#define ACCEL_OP_ALLOC_QUEUE         0x00000008
#define ACCEL_OP_SUBMIT              0x00000010
#define ACCEL_OP_SYNC                0x00000020
#define ACCEL_OP_RESET               0x00000040

/* DMA Permission Flags */
#define DMA_PERM_READ                0x00000001
#define DMA_PERM_WRITE               0x00000002
#define DMA_PERM_COHERENT            0x00000004

/* Decision Codes in EffectReceipt */
#define DEC_ADMITTED                 1
#define DEC_REJECTED                 2

/* DMA Window Descriptor */
typedef struct {
    uint64_t iova_base;
    uint64_t phys_base;
    uint64_t size_bytes;
    uint32_t permissions;
    uint32_t stream_id;
    uint32_t window_id;
    uint32_t reserved;
} OmegaDmaWindow;

/* Accelerator Capability */
typedef struct {
    uint32_t slot;
    uint32_t generation;
    uint64_t principal_id;
    uint32_t resource_type;
    uint32_t allowed_ops;
    uint64_t iova_bound_base;
    uint64_t iova_bound_size;
    uint32_t queue_id_mask;
    uint32_t revocation_state; /* 1 = ACTIVE, 2 = REVOKED */
    uint8_t  provenance_digest[32];
} OmegaAccelCapability;

/* Canonical EffectIntent (64 bytes) */
typedef struct {
    uint16_t version;               /* 1 */
    uint16_t length;                /* 64 */
    uint32_t reserved0;             /* 0 */
    uint64_t request_id;            /* Caller-generated request ID */
    uint64_t principal_id;          /* Principal ID */
    uint32_t capability_slot;       /* Capability slot index */
    uint32_t capability_generation; /* Capability generation */
    uint32_t resource_type;         /* RES_ACCELERATOR */
    uint32_t operation;             /* ACCEL_OP_* */
    uint64_t target_base;           /* Target IOVA or Queue ID */
    uint64_t target_size;           /* Byte span or command length */
    uint64_t param0;                /* Descriptor address or doorbell parameter */
} OmegaEffectIntent;

/* Canonical EffectReceipt (192 bytes) */
typedef struct {
    uint16_t version;               /* 1 */
    uint16_t length;                /* 192 */
    uint32_t decision;              /* 1 = ADMITTED, 2 = REJECTED */
    uint32_t rejection_reason;      /* Specific reason code if rejected */
    uint32_t reserved0;             /* 0 */
    uint64_t request_id;            /* Mirrored from EffectIntent */
    uint8_t  intent_digest[32];     /* SHA-256 of 64-byte EffectIntent */
    uint64_t actual_effect;         /* Physical result or mapped address */
    uint64_t output;                /* Submitted packet count or status */
    uint32_t capability_slot;       /* Presented slot */
    uint32_t capability_generation; /* Presented generation */
    uint64_t machine_generation;    /* Monotonic machine counter */
    uint64_t measurement;           /* Timestamp / cycle measurement */
    uint8_t  previous_receipt_digest[32]; /* Rolling SHA-256 seal chain */
    uint8_t  receipt_digest[32];    /* SHA-256 seal over fields + previous */
    uint8_t  reserved1[32];         /* Padding to 192 bytes */
} OmegaEffectReceipt;

/* Omega Accelerator Port Client Context */
typedef struct {
    OmegaAccelCapability capability;
    uint32_t active_window_count;
    OmegaDmaWindow windows[OMEGA_ACCEL_MAX_WINDOWS];
    uint32_t assigned_queue_id;
    bool has_active_queue;
    uint8_t expected_seal[32];
    uint64_t requests_issued;
    uint64_t receipts_validated;
    bool is_bound_to_physics;
} OmegaAccelPort;

/* Initialize Omega accelerator port client with Physics capability */
int omega_accel_port_init(OmegaAccelPort *port,
                          const OmegaAccelCapability *cap);

/* Formulate an EffectIntent for DMA mapping request */
int omega_accel_port_build_dma_intent(const OmegaAccelPort *port,
                                     uint64_t iova_base,
                                     uint64_t phys_base,
                                     uint64_t size_bytes,
                                     uint32_t permissions,
                                     OmegaEffectIntent *out_intent);

/* Formulate an EffectIntent for queue submission */
int omega_accel_port_build_submit_intent(const OmegaAccelPort *port,
                                        uint32_t queue_id,
                                        uint64_t cmd_iova,
                                        uint64_t cmd_len,
                                        OmegaEffectIntent *out_intent);

/* Validate a unkeyed EffectReceipt digest (no signature/MAC) and update rolling seal state */
int omega_accel_port_verify_receipt(OmegaAccelPort *port,
                                   const OmegaEffectIntent *intent,
                                   const OmegaEffectReceipt *receipt);

/* Ingest Physics accelerator topology into OmegaMachineGraph */
int omega_accel_port_bind_machine_graph(const OmegaAccelPort *port,
                                       OmegaMachineGraph *mg);

#endif /* OMEGA_ACCELERATOR_H */
