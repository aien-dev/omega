/*
 * rx_coherent.c -- physical projection of the canonical reaction object.
 *
 * RxObject is the only identity. The bytes in the coherent image are the
 * frozen 32-byte OMEGA_SHARED_WORLD_V1 record plus the object's field window.
 * This file does not allocate a second id or a second generation. The
 * permission word in the frozen record is written as zero and is never read
 * as authority. The graphics-processor worker is not started here.
 *
 * The frozen header's object table holds 64 records. This image holds one
 * 32-byte slot per host object. That slot is a projection, not an identity.
 * A live object may have no window. The 32-byte record and the 128-byte
 * descriptor are unchanged. The physics m20 sources are not edited.
 */
#include "rx_world.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define ALIGN_UP(x, a) (((x) + ((uint64_t)(a) - 1u)) & ~((uint64_t)(a) - 1u))

typedef struct {
    uint32_t count;
    uint32_t world_epoch;
    uint8_t _pad[OMEGA_SW_CACHELINE - 8u];
    OmegaSharedWorldObject objects[RX_MAX_OBJECTS];
} RxProjectedTable;

_Static_assert(sizeof(OmegaSharedWorldObject) == 32, "physical record must stay 32 bytes");
_Static_assert(sizeof(OmegaSharedWorldDesc) == 128, "descriptor must stay 128 bytes");
_Static_assert(offsetof(RxProjectedTable, objects) == OMEGA_SW_CACHELINE,
               "projection table entries follow one cache line");
_Static_assert(RX_OBJECT_WINDOW == RX_MAX_FIELDS * sizeof(uint64_t),
               "window holds the field values and nothing else");

static uint64_t off_header(void) { return 0; }
static uint64_t off_c2g(void) {
    return ALIGN_UP(sizeof(OmegaSharedWorldHeader), OMEGA_SW_CACHELINE);
}
static uint64_t off_g2c(void) {
    return ALIGN_UP(off_c2g() + sizeof(OmegaSharedWorldRing), OMEGA_SW_CACHELINE);
}
static uint64_t off_fault(void) {
    return ALIGN_UP(off_g2c() + sizeof(OmegaSharedWorldRing), OMEGA_SW_CACHELINE);
}
static uint64_t off_objtbl(void) {
    return ALIGN_UP(off_fault() + sizeof(OmegaSharedWorldFaultMailbox), OMEGA_SW_CACHELINE);
}
static uint64_t off_payload(void) {
    return ALIGN_UP(off_objtbl() + sizeof(RxProjectedTable), OMEGA_SW_CACHELINE);
}
static uint64_t off_heartbeat(void) {
    return ALIGN_UP(off_payload() + (uint64_t)RX_PHYS_WINDOWS * RX_OBJECT_WINDOW,
                    OMEGA_SW_CACHELINE);
}
static uint64_t image_bytes(void) {
    return off_heartbeat() + OMEGA_SW_CACHELINE;
}

uint64_t rx_world_off_c2g(void) { return off_c2g(); }
uint64_t rx_world_off_g2c(void) { return off_g2c(); }
uint64_t rx_world_off_fault(void) { return off_fault(); }
uint64_t rx_world_off_object_table(void) { return off_objtbl(); }
uint64_t rx_world_off_heartbeat(void) { return off_heartbeat(); }

static uint64_t window_offset(uint32_t win) {
    return off_payload() + (uint64_t)win * RX_OBJECT_WINDOW;
}

uint64_t rx_world_physical_table_offset(void) {
    return off_objtbl() + offsetof(RxProjectedTable, objects);
}

static uint64_t load_acquire_u64(volatile uint64_t *p) {
    return atomic_load_explicit((_Atomic uint64_t *)p, memory_order_acquire);
}
static uint64_t load_relaxed_u64(volatile uint64_t *p) {
    return atomic_load_explicit((_Atomic uint64_t *)p, memory_order_relaxed);
}
static void store_release_u64(volatile uint64_t *p, uint64_t v) {
    atomic_store_explicit((_Atomic uint64_t *)p, v, memory_order_release);
}
static void store_release_u32(volatile uint32_t *p, uint32_t v) {
    atomic_store_explicit((_Atomic uint32_t *)p, v, memory_order_release);
}

static uint32_t g_crc[256];
static int g_crc_ready;

static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1u) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
        g_crc[i] = c;
    }
    g_crc_ready = 1;
}

static uint32_t crc32c(const uint8_t *data, size_t len) {
    if (!g_crc_ready) crc_init();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++)
        c = g_crc[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static uint32_t desc_checksum(const OmegaSharedWorldDesc *desc) {
    uint8_t buf[0x3C + 64];
    memcpy(buf, desc, 0x3C);
    memcpy(buf + 0x3C, desc->payload, 64);
    return crc32c(buf, sizeof(buf));
}

void rx_world_seal_descriptor(OmegaSharedWorldDesc *desc) {
    desc->magic = OMEGA_SW_MAGIC;
    desc->abi_version = OMEGA_SW_ABI_VERSION;
    desc->flags = (uint16_t)(desc->flags | OMEGA_SW_FLAG_CHECKSUM | OMEGA_SW_FLAG_OBJECT_REF);
    desc->checksum = 0;
    desc->checksum = desc_checksum(desc);
}

static OmegaSharedWorldHeader *hdr_of(RxWorld *w) {
    return (OmegaSharedWorldHeader *)(w->coherent + off_header());
}
static OmegaSharedWorldRing *pub_ring(RxWorld *w) {
    return (OmegaSharedWorldRing *)(w->coherent + off_c2g());
}
static OmegaSharedWorldFaultMailbox *fault_of(RxWorld *w) {
    return (OmegaSharedWorldFaultMailbox *)(w->coherent + off_fault());
}
static RxProjectedTable *table_of(RxWorld *w) {
    return (RxProjectedTable *)(w->coherent + off_objtbl());
}

static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ring_msg(uint16_t t) {
    return t == RX_RING_WAKE || t == RX_RING_CLAIM || t == RX_RING_PUBLISH ||
           t == RX_RING_COMPLETE || t == RX_RING_FAULT || t == RX_RING_KEEPALIVE ||
           t == RX_RING_SHUTDOWN;
}

/* A claim names the object the seat reads; the object it writes is named in
 * the payload and its write right is checked at publication. */
static uint32_t msg_rights(uint16_t t) {
    if (t == RX_RING_WAKE || t == RX_RING_COMPLETE || t == RX_RING_FAULT ||
        t == RX_RING_KEEPALIVE || t == RX_RING_CLAIM)
        return RX_RIGHT_READ;
    return RX_RIGHT_WRITE;
}

static void raise_fault(RxWorld *w, uint32_t code, uint64_t sequence, uint32_t a, uint32_t b) {
    OmegaSharedWorldFaultMailbox *m = fault_of(w);
    m->world_epoch = w->world_epoch;
    m->fault_sequence = sequence;
    m->detail_a = a;
    m->detail_b = b;
    store_release_u32(&m->fault_code, code);
}

static int fault_err(uint32_t fault) {
    switch (fault) {
    case OMEGA_SW_FAULT_STALE_GEN: return RX_ERR_STALE_GEN;
    case OMEGA_SW_FAULT_BAD_SEQUENCE: return RX_ERR_REPLAY;
    case OMEGA_SW_FAULT_BAD_CHECKSUM:
    case RX_FAULT_TORN_PUB: return RX_ERR_TORN;
    case OMEGA_SW_FAULT_OOB_OBJECT:
    case RX_FAULT_BOUNDS:
    case RX_FAULT_OVERLAP: return RX_ERR_BOUNDS;
    case RX_FAULT_CAP: return RX_ERR_AUTHORITY;
    case RX_FAULT_DIVERGED: return RX_ERR_STALE_GEN;
    case RX_FAULT_UNPLACED: return RX_ERR_UNPLACED;
    default: return RX_ERR_BAD_DESC;
    }
}

int rx_coherent_format(RxWorld *w) {
    uint64_t bytes = image_bytes();
    void *mem = NULL;
    if (posix_memalign(&mem, OMEGA_SW_CACHELINE, (size_t)bytes) != 0) return RX_ERR_FULL;
    memset(mem, 0, (size_t)bytes);
    w->coherent = mem;
    w->coherent_bytes = bytes;
    w->world_epoch = 1;

    OmegaSharedWorldRing *c2g = pub_ring(w);
    OmegaSharedWorldRing *g2c = (OmegaSharedWorldRing *)(w->coherent + off_g2c());
    for (int which = 0; which < 2; which++) {
        OmegaSharedWorldRing *r = which ? g2c : c2g;
        r->capacity = OMEGA_SW_RING_CAPACITY;
        r->mask = OMEGA_SW_RING_MASK;
        r->world_epoch = w->world_epoch;
        r->ring_generation = 1;
    }
    fault_of(w)->world_epoch = w->world_epoch;

    RxProjectedTable *t = table_of(w);
    t->count = RX_MAX_OBJECTS;
    t->world_epoch = w->world_epoch;
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        w->objects[i].id = i;
        w->objects[i].generation = 1;
        w->objects[i].placed = false;
        w->objects[i].window = 0;
        w->objects[i].region_offset = 0;
        w->objects[i].size_bytes = 0;
        w->objects[i].placement = 0;
        w->objects[i].locality = 0;
        w->objects[i].coherency = 0;
        rx_coherent_project(w, i);
    }

    OmegaSharedWorldHeader *h = hdr_of(w);
    h->abi_version = OMEGA_SW_ABI_VERSION;
    h->header_bytes = (uint16_t)sizeof(OmegaSharedWorldHeader);
    h->world_epoch = w->world_epoch;
    h->world_generation = 1;
    h->region_bytes = bytes;
    h->ring_capacity = OMEGA_SW_RING_CAPACITY;
    h->desc_bytes = (uint32_t)sizeof(OmegaSharedWorldDesc);
    h->off_cpu_to_gpu_ring = off_c2g();
    h->off_gpu_to_cpu_ring = off_g2c();
    h->off_fault_mailbox = off_fault();
    h->off_object_table = off_objtbl();
    atomic_thread_fence(memory_order_release);
    store_release_u32(&h->magic, OMEGA_SW_MAGIC);
    return RX_OK;
}

void rx_coherent_free(RxWorld *w) {
    if (!w->coherent_borrowed) free(w->coherent);
    w->coherent = NULL;
    w->coherent_borrowed = false;
}

void rx_coherent_project(RxWorld *w, uint32_t id) {
    if (!w->coherent || id >= RX_MAX_OBJECTS) return;
    RxObject *o = &w->objects[id];
    OmegaSharedWorldObject *p = &table_of(w)->objects[id];
    p->object_id = o->id;
    p->generation = o->generation;
    p->permissions = 0;
    if (!o->placed) {
        p->state = OMEGA_SW_OBJ_REVOKED;
        p->region_offset = 0;
        p->size_bytes = 0;
        return;
    }
    p->state = o->live ? OMEGA_SW_OBJ_ACTIVE : OMEGA_SW_OBJ_REVOKED;
    p->region_offset = o->region_offset;
    p->size_bytes = o->size_bytes;
    if (o->region_offset < off_payload() || o->region_offset > w->coherent_bytes ||
        o->size_bytes > w->coherent_bytes - o->region_offset)
        return;
    uint8_t *win = w->coherent + o->region_offset;
    for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) put_u64(win + f * 8u, o->field[f]);
}

static bool overlap(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz) {
    if (asz == 0 || bsz == 0) return false;
    if (a > UINT64_MAX - asz || b > UINT64_MAX - bsz) return true;
    return a < (b + bsz) && b < (a + asz);
}

static int check_locked(RxWorld *w, const OmegaSharedWorldDesc *desc,
                        uint64_t expected_sequence, uint32_t *out_fault) {
    uint32_t fault = OMEGA_SW_FAULT_NONE;
    if (!w->coherent || !desc) fault = OMEGA_SW_FAULT_BAD_MAGIC;
    else if (desc->magic != OMEGA_SW_MAGIC) fault = OMEGA_SW_FAULT_BAD_MAGIC;
    else if (desc->abi_version != OMEGA_SW_ABI_VERSION) fault = OMEGA_SW_FAULT_BAD_VERSION;
    else if (desc->world_epoch != w->world_epoch) fault = OMEGA_SW_FAULT_STALE_EPOCH;
    else if (desc->sequence != expected_sequence) fault = OMEGA_SW_FAULT_BAD_SEQUENCE;
    else if (!ring_msg(desc->msg_type)) fault = OMEGA_SW_FAULT_BAD_MSGTYPE;
    else if ((desc->flags & OMEGA_SW_FLAG_CHECKSUM) == 0) fault = OMEGA_SW_FAULT_BAD_CHECKSUM;
    else {
        OmegaSharedWorldDesc tmp = *desc;
        uint32_t want = tmp.checksum;
        tmp.checksum = 0;
        if (desc_checksum(&tmp) != want) fault = OMEGA_SW_FAULT_BAD_CHECKSUM;
    }

    if (fault == OMEGA_SW_FAULT_NONE) {
        if (desc->object_id >= RX_MAX_OBJECTS) fault = OMEGA_SW_FAULT_OOB_OBJECT;
        else {
            const RxObject *o = &w->objects[desc->object_id];
            const OmegaSharedWorldObject *p = &table_of(w)->objects[desc->object_id];
            if (desc->object_generation == 0 || desc->object_generation != o->generation)
                fault = OMEGA_SW_FAULT_STALE_GEN;
            else if (p->object_id != o->id || p->generation != o->generation)
                fault = RX_FAULT_DIVERGED;
            else if (!o->placed) {
                fault = (p->state == OMEGA_SW_OBJ_ACTIVE || p->size_bytes != 0)
                            ? RX_FAULT_DIVERGED
                            : RX_FAULT_UNPLACED;
            } else if (p->region_offset != o->region_offset || p->size_bytes != o->size_bytes ||
                       (o->live ? p->state != OMEGA_SW_OBJ_ACTIVE : p->state != OMEGA_SW_OBJ_REVOKED))
                fault = RX_FAULT_DIVERGED;
            else if (!o->live && desc->msg_type != RX_RING_FAULT)
                fault = OMEGA_SW_FAULT_OOB_OBJECT;
            else if (desc->object_length == 0 ||
                     (uint64_t)desc->object_offset > o->size_bytes ||
                     (uint64_t)desc->object_length > o->size_bytes - (uint64_t)desc->object_offset)
                fault = RX_FAULT_BOUNDS;
            else if (o->region_offset < off_payload() ||
                     o->region_offset > w->coherent_bytes ||
                     o->size_bytes > w->coherent_bytes - o->region_offset)
                fault = RX_FAULT_BOUNDS;
            else {
                uint64_t abs = o->region_offset + desc->object_offset;
                if (abs > w->coherent_bytes ||
                    (uint64_t)desc->object_length > w->coherent_bytes - abs)
                    fault = RX_FAULT_BOUNDS;
            }
            if (fault == OMEGA_SW_FAULT_NONE) {
                for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
                    if (i == o->id || !w->objects[i].live) continue;
                    if (overlap(o->region_offset, o->size_bytes,
                                w->objects[i].region_offset, w->objects[i].size_bytes)) {
                        fault = RX_FAULT_OVERLAP;
                        break;
                    }
                }
            }
            if (fault == OMEGA_SW_FAULT_NONE && o->live) {
                const uint8_t *win = w->coherent + o->region_offset;
                for (uint32_t f = 0; f < RX_MAX_FIELDS; f++) {
                    if (get_u64(win + f * 8u) != o->field[f]) {
                        fault = RX_FAULT_TORN_PUB;
                        break;
                    }
                }
            }
            if (fault == OMEGA_SW_FAULT_NONE && desc->msg_type == RX_RING_PUBLISH) {
                if (desc->payload_len < 24) fault = RX_FAULT_TORN_PUB;
                else if (get_u64(desc->payload + 8) != o->version) fault = RX_FAULT_TORN_PUB;
            }
            if (fault == OMEGA_SW_FAULT_NONE) {
                if (desc->payload_len < 8) fault = RX_FAULT_CAP;
                else {
                    RxCapRef cap = { get_u32(desc->payload), get_u32(desc->payload + 4) };
                    if (cap.cap_id != o->cap.cap_id || cap.generation != o->cap.generation)
                        fault = RX_FAULT_CAP;
                    else {
                        RxCapEntry ent;
                        int irc = rx_world_inspect_cap(w, cap, &ent);
                        int vrc = irc == RX_CAP_OK
                                      ? rx_world_validate_cap(w, cap, ent.subject, o->resource,
                                                              msg_rights(desc->msg_type), NULL)
                                      : irc;
                        if (vrc != RX_CAP_OK) fault = RX_FAULT_CAP;
                        else if (desc->msg_type == RX_RING_PUBLISH) {
                            uint64_t cid = get_u64(desc->payload + 16);
                            const RxCrumb *k = (cid == 0 || cid > w->n_crumbs) ? NULL : &w->crumbs[cid - 1];
                            bool named = false;
                            if (k) {
                                for (uint32_t n = 0; n < k->n_outputs; n++)
                                    if (k->outputs[n].obj.id == o->id &&
                                        k->outputs[n].obj.generation == o->generation)
                                        named = true;
                                for (uint32_t n = 0; n < k->n_inputs; n++)
                                    if (k->inputs[n].obj.id == o->id &&
                                        k->inputs[n].obj.generation == o->generation)
                                        named = true;
                            }
                            if (!named) fault = RX_FAULT_TORN_PUB;
                        }
                    }
                }
            }
        }
    }
    if (out_fault) *out_fault = fault;
    return fault == OMEGA_SW_FAULT_NONE ? RX_OK : fault_err(fault);
}

int rx_world_check_descriptor(RxWorld *w, const OmegaSharedWorldDesc *desc,
                              uint64_t expected_sequence, uint32_t *out_fault) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = check_locked(w, desc, expected_sequence, out_fault);
    pthread_mutex_unlock(&w->mu);
    return rc;
}

void rx_coherent_publish(RxWorld *w, uint32_t id, uint64_t crumb_id) {
    if (!w->coherent || id >= RX_MAX_OBJECTS) return;
    RxObject *o = &w->objects[id];
    if (!o->placed) return;
    if (o->cap.cap_id == 0 && o->cap.generation == 0) return;
    rx_coherent_project(w, id);
    OmegaSharedWorldRing *ring = pub_ring(w);
    uint64_t t = load_relaxed_u64(&ring->tail);
    uint64_t h = load_acquire_u64(&ring->head);
    if (t - h >= ring->capacity) {
        w->stats.desc_rejected++;
        return;
    }
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = RX_RING_PUBLISH;
    d.sequence = t;
    d.world_epoch = w->world_epoch;
    d.object_id = o->id;
    d.object_generation = o->generation;
    d.object_offset = 0;
    d.object_length = (uint32_t)o->size_bytes;
    d.payload_len = 24;
    put_u32(d.payload, o->cap.cap_id);
    put_u32(d.payload + 4, o->cap.generation);
    put_u64(d.payload + 8, o->version);
    put_u64(d.payload + 16, crumb_id);
    rx_world_seal_descriptor(&d);
    memcpy(&ring->slots[t & ring->mask], &d, sizeof(d));
    store_release_u64(&ring->tail, t + 1);
    w->stats.desc_published++;
}

int rx_world_take_publication(RxWorld *w, OmegaSharedWorldDesc *out, uint32_t *out_fault) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    OmegaSharedWorldRing *ring = pub_ring(w);
    uint64_t h = load_relaxed_u64(&ring->head);
    uint64_t t = load_acquire_u64(&ring->tail);
    if (h >= t) {
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_NOT_FOUND;
    }
    OmegaSharedWorldDesc d;
    memcpy(&d, &ring->slots[h & ring->mask], sizeof(d));
    uint32_t fault = OMEGA_SW_FAULT_NONE;
    int rc = check_locked(w, &d, h, &fault);
    store_release_u64(&ring->head, h + 1);
    if (rc != RX_OK) {
        raise_fault(w, fault, h, d.msg_type, (uint32_t)d.sequence);
        w->stats.desc_rejected++;
    }
    if (out_fault) *out_fault = fault;
    if (out && rc == RX_OK) *out = d;
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_inject_descriptor(RxWorld *w, const OmegaSharedWorldDesc *desc) {
    if (!w || !desc || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    OmegaSharedWorldRing *ring = pub_ring(w);
    uint64_t t = load_relaxed_u64(&ring->tail);
    uint64_t h = load_acquire_u64(&ring->head);
    if (t - h >= ring->capacity) {
        pthread_mutex_unlock(&w->mu);
        return RX_ERR_FULL;
    }
    memcpy(&ring->slots[t & ring->mask], desc, sizeof(*desc));
    store_release_u64(&ring->tail, t + 1);
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

uint64_t rx_world_publication_tail(RxWorld *w) {
    if (!w || !w->coherent) return 0;
    pthread_mutex_lock(&w->mu);
    uint64_t t = load_relaxed_u64(&pub_ring(w)->tail);
    pthread_mutex_unlock(&w->mu);
    return t;
}

int rx_world_bind_capability(RxWorld *w, RxObjRef ref, RxCapRef cap) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    if (ref.id >= RX_MAX_OBJECTS || !w->objects[ref.id].live ||
        w->objects[ref.id].generation != ref.generation) {
        rc = RX_ERR_STALE_GEN;
        goto out;
    }
    RxCapEntry ent;
    if (rx_world_inspect_cap(w, cap, &ent) != RX_CAP_OK ||
        rx_world_validate_cap(w, cap, ent.subject, w->objects[ref.id].resource,
                              RX_RIGHT_READ, NULL) != RX_CAP_OK) {
        rc = RX_ERR_AUTHORITY;
        goto out;
    }
    w->objects[ref.id].cap = cap;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_physical(RxWorld *w, RxObjRef ref, OmegaSharedWorldObject *out) {
    if (!w || !out || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = RX_OK;
    if (ref.id >= RX_MAX_OBJECTS || w->objects[ref.id].generation != ref.generation) {
        rc = RX_ERR_STALE_GEN;
    } else if (!w->objects[ref.id].placed) {
        rc = RX_ERR_UNPLACED;
    } else {
        *out = table_of(w)->objects[ref.id];
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_overwrite_physical(RxWorld *w, uint32_t id, const OmegaSharedWorldObject *src) {
    if (!w || !src || !w->coherent || id >= RX_MAX_OBJECTS) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    table_of(w)->objects[id] = *src;
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

static int window_busy(const RxWorld *w, uint32_t win, uint32_t except) {
    for (uint32_t i = 0; i < RX_MAX_OBJECTS; i++) {
        if (i == except || !w->objects[i].placed) continue;
        if (w->objects[i].window == win) return 1;
    }
    return 0;
}

static int window_for_offset(uint64_t offset, uint64_t length, uint32_t *out_win) {
    if (length != RX_OBJECT_WINDOW) return RX_ERR_BOUNDS;
    if (offset > UINT64_MAX - length) return RX_ERR_BOUNDS;
    uint64_t payload = off_payload();
    uint64_t limit = payload + (uint64_t)RX_PHYS_WINDOWS * RX_OBJECT_WINDOW;
    if (offset < payload || offset + length > limit) return RX_ERR_BOUNDS;
    uint64_t rel = offset - payload;
    if (rel % RX_OBJECT_WINDOW != 0) return RX_ERR_BOUNDS;
    *out_win = (uint32_t)(rel / RX_OBJECT_WINDOW);
    return RX_OK;
}

static void adopt_window(RxWorld *w, RxObject *o, uint32_t win) {
    o->placed = true;
    o->window = win;
    o->region_offset = window_offset(win);
    o->size_bytes = RX_OBJECT_WINDOW;
    o->placement = RX_PLACE_COHERENT;
    o->locality = RX_LOCALITY_MACHINE;
    o->coherency = RX_COHERENCY_HOST;
    rx_coherent_project(w, o->id);
}

static int current_object(RxWorld *w, RxObjRef ref, RxObject **out) {
    if (ref.id >= RX_MAX_OBJECTS || w->objects[ref.id].generation != ref.generation ||
        !w->objects[ref.id].live)
        return RX_ERR_STALE_GEN;
    *out = &w->objects[ref.id];
    return RX_OK;
}

int rx_world_attach_physical(RxWorld *w, RxObjRef ref) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    RxObject *o = NULL;
    int rc = current_object(w, ref, &o);
    if (rc != RX_OK) goto out;
    if (o->placed) {
        rc = RX_ERR_EXISTS;
        goto out;
    }
    uint32_t win = 0;
    int found = 0;
    for (uint32_t i = 0; i < RX_PHYS_WINDOWS; i++) {
        if (!window_busy(w, i, o->id)) {
            win = i;
            found = 1;
            break;
        }
    }
    if (!found) {
        rc = RX_ERR_FULL;
        goto out;
    }
    adopt_window(w, o, win);
    rc = RX_OK;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_detach_physical(RxWorld *w, RxObjRef ref) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    RxObject *o = NULL;
    int rc = current_object(w, ref, &o);
    if (rc != RX_OK) goto out;
    if (!o->placed) {
        rc = RX_ERR_UNPLACED;
        goto out;
    }
    o->placed = false;
    o->window = 0;
    o->region_offset = 0;
    o->size_bytes = 0;
    o->placement = 0;
    o->locality = 0;
    o->coherency = 0;
    rx_coherent_project(w, o->id);
    rc = RX_OK;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_place_physical(RxWorld *w, RxObjRef ref, uint64_t offset, uint64_t length) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    RxObject *o = NULL;
    int rc = current_object(w, ref, &o);
    if (rc != RX_OK) goto out;
    uint32_t win = 0;
    rc = window_for_offset(offset, length, &win);
    if (rc != RX_OK) goto out;
    if (window_busy(w, win, o->id)) {
        rc = RX_ERR_EXISTS;
        goto out;
    }
    if (o->placed && o->window != win) {
        uint8_t saved[RX_OBJECT_WINDOW];
        memcpy(saved, w->coherent + o->region_offset, RX_OBJECT_WINDOW);
        adopt_window(w, o, win);
        memcpy(w->coherent + o->region_offset, saved, RX_OBJECT_WINDOW);
    } else {
        adopt_window(w, o, win);
    }
    rc = RX_OK;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_relocate_physical(RxWorld *w, RxObjRef ref) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    RxObject *o = NULL;
    int rc = current_object(w, ref, &o);
    if (rc != RX_OK) goto out;
    if (!o->placed) {
        rc = RX_ERR_UNPLACED;
        goto out;
    }
    uint32_t win = 0;
    int found = 0;
    for (uint32_t i = 0; i < RX_PHYS_WINDOWS; i++) {
        if (i == o->window || window_busy(w, i, o->id)) continue;
        win = i;
        found = 1;
        break;
    }
    if (!found) {
        rc = RX_ERR_FULL;
        goto out;
    }
    uint8_t saved[RX_OBJECT_WINDOW];
    uint8_t digest[32];
    uint32_t generation = o->generation;
    uint64_t version = o->version;
    memcpy(saved, w->coherent + o->region_offset, RX_OBJECT_WINDOW);
    memcpy(digest, o->digest, 32);
    adopt_window(w, o, win);
    memcpy(w->coherent + o->region_offset, saved, RX_OBJECT_WINDOW);
    if (o->generation != generation || o->version != version ||
        memcmp(o->digest, digest, 32) != 0) {
        rc = RX_ERR_BAD_DESC;
        goto out;
    }
    rc = RX_OK;
out:
    pthread_mutex_unlock(&w->mu);
    return rc;
}

/* ---- resident seat ------------------------------------------------------ */

static OmegaSharedWorldRing *g2c_ring(RxWorld *w) {
    return (OmegaSharedWorldRing *)(w->coherent + off_g2c());
}

static int notice_intact(const OmegaSharedWorldDesc *d, uint32_t epoch) {
    if (!d || d->magic != OMEGA_SW_MAGIC || d->abi_version != OMEGA_SW_ABI_VERSION)
        return RX_ERR_BAD_DESC;
    if (d->world_epoch != epoch) return RX_ERR_STALE_GEN;
    if ((d->flags & OMEGA_SW_FLAG_CHECKSUM) == 0) return RX_ERR_TORN;
    OmegaSharedWorldDesc tmp = *d;
    uint32_t got = tmp.checksum;
    tmp.checksum = 0;
    if (desc_checksum(&tmp) != got) return RX_ERR_TORN;
    return RX_OK;
}

static int post_on(RxWorld *w, OmegaSharedWorldRing *ring, OmegaSharedWorldDesc *d) {
    uint64_t t = load_relaxed_u64(&ring->tail);
    uint64_t h = load_acquire_u64(&ring->head);
    if (t - h >= ring->capacity) return RX_ERR_FULL;
    d->sequence = t;
    d->world_epoch = w->world_epoch;
    rx_world_seal_descriptor(d);
    memcpy(&ring->slots[t & ring->mask], d, sizeof(*d));
    atomic_thread_fence(memory_order_release);
    store_release_u64(&ring->tail, t + 1);
    return RX_OK;
}

int rx_world_enable_resident(RxWorld *w) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    w->resident_enabled = true;
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

/* Claim payload, little-endian:
 *   0  in capability id      4  in capability generation
 *   8  in object version    16  parent crumb
 *  24  out object id        28  out object generation
 *  32  out capability id    36  out capability generation */
#define CLAIM_PAYLOAD 40u

int rx_resident_post_claim(RxWorld *w, uint32_t in, uint32_t out, uint64_t parent,
                           uint64_t *seq_out) {
    if (!w || !w->coherent || in >= RX_MAX_OBJECTS || out >= RX_MAX_OBJECTS || in == out)
        return RX_ERR_ARG;
    RxObject *a = &w->objects[in];
    RxObject *b = &w->objects[out];
    if (!a->live || !a->placed || !b->live || !b->placed) return RX_ERR_UNPLACED;
    OmegaSharedWorldRing *ring = pub_ring(w);
    uint64_t t = load_relaxed_u64(&ring->tail);
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = RX_RING_CLAIM;
    d.object_id = a->id;
    d.object_generation = a->generation;
    d.object_offset = 0;
    d.object_length = (uint32_t)a->size_bytes;
    d.payload_len = CLAIM_PAYLOAD;
    d.arg_b = (uint32_t)t;
    put_u32(d.payload, a->cap.cap_id);
    put_u32(d.payload + 4, a->cap.generation);
    put_u64(d.payload + 8, a->version);
    put_u64(d.payload + 16, parent);
    put_u32(d.payload + 24, b->id);
    put_u32(d.payload + 28, b->generation);
    put_u32(d.payload + 32, b->cap.cap_id);
    put_u32(d.payload + 36, b->cap.generation);
    int rc = post_on(w, ring, &d);
    if (rc == RX_OK && seq_out) *seq_out = t;
    return rc;
}

/* The seat's one operation, as the stand-in performs it: the qualified
 * 32-bit integer add of the input's field 0 and field 1, into the output's
 * field 0. The high word is written as zero, as the chip writes it. */
static void apply_add(RxWorld *w, const RxObject *a, const RxObject *b) {
    const uint8_t *in = w->coherent + a->region_offset;
    uint8_t *outw = w->coherent + b->region_offset;
    uint32_t sum = (uint32_t)get_u64(in) + (uint32_t)get_u64(in + 8);
    put_u64(outw, (uint64_t)sum);
}

/* Stand-in check of the output named in a claim. */
static int claim_out_ok(RxWorld *w, const OmegaSharedWorldDesc *d, uint32_t *out_id) {
    if (d->payload_len < CLAIM_PAYLOAD) return 0;
    uint32_t id = get_u32(d->payload + 24);
    uint32_t gen = get_u32(d->payload + 28);
    if (id >= RX_MAX_OBJECTS || id == d->object_id) return 0;
    const RxObject *b = &w->objects[id];
    const OmegaSharedWorldObject *p = &table_of(w)->objects[id];
    if (!b->live || !b->placed || gen == 0 || gen != b->generation) return 0;
    if (p->generation != gen || p->state != OMEGA_SW_OBJ_ACTIVE ||
        p->region_offset != b->region_offset)
        return 0;
    *out_id = id;
    return 1;
}

static int post_result(RxWorld *w, uint16_t msg, const OmegaSharedWorldDesc *claim) {
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = msg;
    d.object_id = claim->object_id;
    d.object_generation = claim->object_generation;
    d.object_offset = 0;
    d.object_length = claim->object_length ? claim->object_length : RX_OBJECT_WINDOW;
    d.payload_len = claim->payload_len;
    d.arg_b = (uint32_t)claim->sequence;
    memcpy(d.payload, claim->payload, sizeof d.payload);
    return post_on(w, g2c_ring(w), &d);
}

int rx_resident_seat_step(RxWorld *w) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    int rc = 0;
    OmegaSharedWorldRing *ring = pub_ring(w);
    for (;;) {
        uint64_t h = load_relaxed_u64(&ring->head);
        uint64_t t = load_acquire_u64(&ring->tail);
        if (h >= t) break;
        OmegaSharedWorldDesc d;
        memcpy(&d, &ring->slots[h & ring->mask], sizeof(d));
        if (d.msg_type == RX_RING_SHUTDOWN && notice_intact(&d, w->world_epoch) == RX_OK) {
            store_release_u64(&ring->head, h + 1);
            w->resident_stopped = true;
            rc = 2;
            break;
        }
        if (d.msg_type != RX_RING_CLAIM) {
            store_release_u64(&ring->head, h + 1);
            continue;
        }
        uint32_t fault = OMEGA_SW_FAULT_NONE;
        int chk = check_locked(w, &d, h, &fault);
        if (chk != RX_OK) {
            raise_fault(w, fault, h, d.msg_type, d.object_id);
            post_result(w, RX_RING_FAULT, &d);
            store_release_u64(&ring->head, h + 1);
            w->stats.desc_rejected++;
            rc = chk;
            break;
        }
        uint32_t out_id = 0;
        if (!claim_out_ok(w, &d, &out_id)) {
            raise_fault(w, OMEGA_SW_FAULT_STALE_GEN, h, d.msg_type, d.object_id);
            post_result(w, RX_RING_FAULT, &d);
            store_release_u64(&ring->head, h + 1);
            w->stats.desc_rejected++;
            rc = RX_ERR_STALE_GEN;
            break;
        }
        apply_add(w, &w->objects[d.object_id], &w->objects[out_id]);
        if (post_result(w, RX_RING_PUBLISH, &d) != RX_OK) {
            rc = RX_ERR_FULL;
            break;
        }
        store_release_u64(&ring->head, h + 1);
        rc = 1;
        break;
    }
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_resident_take_result(RxWorld *w, OmegaSharedWorldDesc *out) {
    if (!w || !w->coherent || !out) return RX_ERR_ARG;
    OmegaSharedWorldRing *ring = g2c_ring(w);
    uint64_t h = load_relaxed_u64(&ring->head);
    uint64_t t = load_acquire_u64(&ring->tail);
    if (h >= t) return RX_ERR_NOT_FOUND;
    memcpy(out, &ring->slots[h & ring->mask], sizeof(*out));
    store_release_u64(&ring->head, h + 1);
    return notice_intact(out, w->world_epoch);
}

int rx_resident_reset(RxWorld *w) {
    if (!w) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    w->resident_stopped = false;
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}

int rx_resident_shutdown(RxWorld *w) {
    if (!w || !w->coherent) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    OmegaSharedWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.msg_type = RX_RING_SHUTDOWN;
    d.object_id = 0;
    d.object_generation = w->objects[0].generation ? w->objects[0].generation : 1;
    d.object_length = RX_OBJECT_WINDOW;
    d.payload_len = 8;
    int rc = post_on(w, pub_ring(w), &d);
    pthread_mutex_unlock(&w->mu);
    return rc;
}

int rx_world_bind_coherent(RxWorld *w, void *mem, uint64_t bytes, int borrowed) {
    if (!w || !mem || !w->coherent || bytes != w->coherent_bytes) return RX_ERR_ARG;
    pthread_mutex_lock(&w->mu);
    memcpy(mem, w->coherent, (size_t)bytes);
    if (!w->coherent_borrowed) free(w->coherent);
    w->coherent = mem;
    w->coherent_borrowed = borrowed ? true : false;
    pthread_mutex_unlock(&w->mu);
    return RX_OK;
}
