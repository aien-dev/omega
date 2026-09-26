#include "omega_accelerator.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>

int omega_accel_port_init(OmegaAccelPort *port,
                          const OmegaAccelCapability *cap) {
    if (!port || !cap) return -1;
    memset(port, 0, sizeof(*port));

    port->capability = *cap;
    port->is_bound_to_physics = true;
    memset(port->expected_seal, 0, sizeof(port->expected_seal));
    return 0;
}

int omega_accel_port_build_dma_intent(const OmegaAccelPort *port,
                                     uint64_t iova_base,
                                     uint64_t phys_base,
                                     uint64_t size_bytes,
                                     uint32_t permissions,
                                     OmegaEffectIntent *out_intent) {
    if (!port || !out_intent || size_bytes == 0) return -1;

    /* Check bounds against capability */
    if (iova_base < port->capability.iova_bound_base ||
        (iova_base + size_bytes) > (port->capability.iova_bound_base + port->capability.iova_bound_size)) {
        return -2;
    }

    memset(out_intent, 0, sizeof(*out_intent));
    out_intent->version = 1;
    out_intent->length = sizeof(OmegaEffectIntent);
    out_intent->request_id = 1000 + port->requests_issued + 1;
    out_intent->principal_id = port->capability.principal_id;
    out_intent->capability_slot = port->capability.slot;
    out_intent->capability_generation = port->capability.generation;
    out_intent->resource_type = RES_ACCELERATOR;
    out_intent->operation = ACCEL_OP_MAP_DMA;
    out_intent->target_base = iova_base;
    out_intent->target_size = size_bytes;
    out_intent->param0 = phys_base;
    (void)permissions;

    return 0;
}

int omega_accel_port_build_submit_intent(const OmegaAccelPort *port,
                                        uint32_t queue_id,
                                        uint64_t cmd_iova,
                                        uint64_t cmd_len,
                                        OmegaEffectIntent *out_intent) {
    if (!port || !out_intent || cmd_len == 0) return -1;

    memset(out_intent, 0, sizeof(*out_intent));
    out_intent->version = 1;
    out_intent->length = sizeof(OmegaEffectIntent);
    out_intent->request_id = 2000 + port->requests_issued + 1;
    out_intent->principal_id = port->capability.principal_id;
    out_intent->capability_slot = port->capability.slot;
    out_intent->capability_generation = port->capability.generation;
    out_intent->resource_type = RES_ACCELERATOR;
    out_intent->operation = ACCEL_OP_SUBMIT;
    out_intent->target_base = queue_id;
    out_intent->target_size = cmd_len;
    out_intent->param0 = cmd_iova;

    return 0;
}

int omega_accel_port_verify_receipt(OmegaAccelPort *port,
                                   const OmegaEffectIntent *intent,
                                   const OmegaEffectReceipt *receipt) {
    if (!port || !receipt) return -1;

    if (receipt->decision != DEC_ADMITTED) {
        return -2; /* Effect refused or rejected by Physics */
    }

    /* Verify intent digest */
    if (intent) {
        uint8_t expected_intent_digest[32];
        sha256_hash((const uint8_t *)intent, sizeof(OmegaEffectIntent), expected_intent_digest);
        if (memcmp(expected_intent_digest, receipt->intent_digest, 32) != 0) {
            return -3; /* Intent digest mismatch */
        }
    }

    /* Verify previous receipt seal binding */
    if (memcmp(port->expected_seal, receipt->previous_receipt_digest, 32) != 0) {
        return -4; /* Seal chain discontinuity */
    }

    /* Verify unkeyed receipt digest over bytes 0..127 + previous_receipt_digest */
    uint8_t hash_input[160];
    memcpy(hash_input, receipt, 128);
    memcpy(hash_input + 128, receipt->previous_receipt_digest, 32);

    uint8_t expected_receipt_digest[32];
    sha256_hash(hash_input, 160, expected_receipt_digest);

    if (memcmp(expected_receipt_digest, receipt->receipt_digest, 32) != 0) {
        return -5; /* Digest mismatch (does not authenticate the producer) */
    }

    /* Update rolling expected seal and advance counter */
    memcpy(port->expected_seal, receipt->receipt_digest, 32);
    port->receipts_validated++;

    /* If this was a successful DMA grant, track the window */
    if (intent && intent->operation == ACCEL_OP_MAP_DMA) {
        if (port->active_window_count < OMEGA_ACCEL_MAX_WINDOWS) {
            OmegaDmaWindow *w = &port->windows[port->active_window_count++];
            w->iova_base = intent->target_base;
            w->phys_base = receipt->actual_effect;
            w->size_bytes = intent->target_size;
            w->permissions = DMA_PERM_READ | DMA_PERM_WRITE | DMA_PERM_COHERENT;
            w->window_id = port->active_window_count;
        }
    }

    return 0;
}

int omega_accel_port_bind_machine_graph(const OmegaAccelPort *port,
                                       OmegaMachineGraph *mg) {
    if (!port || !mg) return -1;

    /* Add UNIT_ACCELERATOR_PORT to pipeline compute units */
    if (mg->pipeline.unit_count < OMEGA_MACHINE_MAX_UNITS) {
        MachineComputeUnit *unit = &mg->pipeline.units[mg->pipeline.unit_count++];
        unit->type = UNIT_ACCELERATOR_PORT;
        unit->count = 1;
        unit->latency_cycles = 10;
        unit->throughput_per_cycle = 4;
    }

    mg->is_physics_authorized = true;
    memcpy(mg->physics_receipt_seal, port->expected_seal, 32);

    /* Recompute canonical machine ID */
    return omega_machine_compute_id(mg);
}
