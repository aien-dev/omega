#include "rx_effect_identity.h"
#include "../sha256.h"
#include <string.h>

int rx_effect_identity_compute(const RxEffectIdentityInput *in, uint8_t out[32]) {
    if (!in || !out || !in->subject || !in->capability_generation ||
        !in->resource_class || !in->operation_code) return -1;
    /* Fixed-width, big-endian encoding under a versioned domain. */
    uint8_t b[4+4+8+2+2+OMEGA_ID_BYTES+32+8+OMEGA_ID_BYTES+32];
    size_t p=0;
    const uint8_t domain[]="omega.effect-id.v1";
    sha256_ctx c; sha256_init(&c); sha256_update(&c,domain,sizeof(domain));
#define PUTN(v,n) do { uint64_t x=(uint64_t)(v); for (int j=(n)-1;j>=0;--j) b[p++]=(uint8_t)(x>>(j*8)); } while(0)
    PUTN(in->subject,4); PUTN(in->capability_slot,4); PUTN(in->capability_generation,8);
    PUTN(in->resource_class,2); PUTN(in->operation_code,2);
    memcpy(b+p,in->target.bytes,OMEGA_ID_BYTES); p+=OMEGA_ID_BYTES;
    memcpy(b+p,in->input_digest,32); p+=32; PUTN(in->world_generation,8);
    memcpy(b+p,in->authorization_context.bytes,OMEGA_ID_BYTES); p+=OMEGA_ID_BYTES;
    memcpy(b+p,in->idempotency_key,32); p+=32;
#undef PUTN
    sha256_update(&c,b,p); sha256_final(&c,out); return 0;
}
