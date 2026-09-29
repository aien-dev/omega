#include "omega_v3.h"
#include "sha256.h"
#include <string.h>

static void hash_u64(sha256_ctx *c, uint64_t v) {
    uint8_t b[8]; for (int i=7;i>=0;--i) { b[i]=(uint8_t)v; v >>= 8; }
    sha256_update(c,b,sizeof(b));
}

int omega_v3_reduce(const OmegaV3Evidence *cases, uint32_t count,
                    uint32_t required, OmegaV3Report *out) {
    if (!out || (count && !cases)) return -1;
    memset(out,0,sizeof(*out)); out->required=required; out->executed=count;
    sha256_ctx c; sha256_init(&c); static const uint8_t domain[]="omega.v3.evidence.v1";
    sha256_update(&c,domain,sizeof(domain));
    for (uint32_t i=0;i<count;i++) {
        sha256_update(&c,cases[i].program_id.bytes,OMEGA_ID_BYTES);
        sha256_update(&c,cases[i].realization_id.bytes,OMEGA_ID_BYTES);
        sha256_update(&c,cases[i].envelope_identity,32);
        hash_u64(&c,cases[i].world_generation); hash_u64(&c,cases[i].attack_class);
        sha256_update(&c,cases[i].attack_input_digest,32);
        hash_u64(&c,cases[i].expected_invariant); hash_u64(&c,cases[i].observed);
        hash_u64(&c,cases[i].authority_attempted); hash_u64(&c,cases[i].effect_attempted);
        hash_u64(&c,cases[i].containment_boundary); hash_u64(&c,cases[i].verdict);
        sha256_update(&c,cases[i].evidence_root,32);
        if (cases[i].verdict == 0 && cases[i].observed == OMEGA_V3_CONTAINED) out->contained++;
        else if (cases[i].verdict == 2 || cases[i].observed == OMEGA_V3_UNAVAILABLE) out->required_skips++;
        else out->failed++;
    }
    if (count < required) out->required_skips += required-count;
    sha256_final(&c,out->evidence_root);
    out->passed=(count >= required && out->contained == count && out->failed == 0 && out->required_skips == 0);
    return 0;
}
