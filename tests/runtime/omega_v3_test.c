#include "../../src/omega_v3.h"
#include "../../src/runtime/rx_effect_identity.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); return 1; } } while(0)
int main(void) {
    RxEffectIdentityInput a={0}, b; uint8_t x[32],y[32];
    a.subject=7; a.capability_slot=2; a.capability_generation=9;
    a.resource_class=1; a.operation_code=3; a.world_generation=11;
    a.input_digest[0]=4; a.target.bytes[0]=5; a.authorization_context.bytes[0]=6;
    a.idempotency_key[0]=8; b=a;
    CHECK(rx_effect_identity_compute(&a,x)==0 && rx_effect_identity_compute(&b,y)==0 && !memcmp(x,y,32));
#define MUTATE(field) do { b=a; b.field++; CHECK(rx_effect_identity_compute(&b,y)==0 && memcmp(x,y,32)); } while(0)
    MUTATE(subject); MUTATE(capability_slot); MUTATE(capability_generation);
    MUTATE(resource_class); MUTATE(operation_code); MUTATE(world_generation);
#undef MUTATE
    b=a; b.input_digest[0]++; CHECK(rx_effect_identity_compute(&b,y)==0 && memcmp(x,y,32));
    b=a; b.target.bytes[0]++; CHECK(rx_effect_identity_compute(&b,y)==0 && memcmp(x,y,32));
    b=a; b.authorization_context.bytes[0]++; CHECK(rx_effect_identity_compute(&b,y)==0 && memcmp(x,y,32));
    b=a; b.idempotency_key[0]++; CHECK(rx_effect_identity_compute(&b,y)==0 && memcmp(x,y,32));
    OmegaV3Evidence e={0}; e.observed=OMEGA_V3_UNAVAILABLE; e.verdict=2;
    OmegaV3Report r; CHECK(omega_v3_reduce(&e,1,1,&r)==0 && !r.passed && r.required_skips==1);
    e.observed=OMEGA_V3_CONTAINED; e.verdict=0;
    CHECK(omega_v3_reduce(&e,1,1,&r)==0 && r.passed && r.contained==1);
    puts("OMEGA_V3_INTERFACE_PASS effect identity binds request context; required skips fail closed");
    return 0;
}
