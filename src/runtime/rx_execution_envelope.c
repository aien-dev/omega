#include "rx_execution_envelope.h"

#include <string.h>

static bool id_equal(SemanticId a, SemanticId b) {
    return memcmp(a.bytes, b.bytes, sizeof(a.bytes)) == 0;
}

static bool dep_has(const RxDep *deps, uint32_t n, RxObjRef ref) {
    for (uint32_t i = 0; i < n; ++i)
        if (deps[i].obj.id == ref.id && deps[i].obj.generation == ref.generation) return true;
    return false;
}

static RxEnvelopeResult refuse(RxEnvelopeEvidence *e, RxEnvelopeResult r, uint32_t inv) {
    if (e) { e->result = r; e->invariant = inv; }
    return r;
}

RxEnvelopeResult rx_execution_envelope_validate(
    const RxExecutionEnvelope *env, const RxWorld *world,
    uint32_t current_world_generation, uint64_t now,
    uint64_t memory_budget, uint64_t compute_budget,
    RxEnvelopeEvidence *e) {
    if (e) memset(e, 0, sizeof(*e));
    if (!env || !world || !env->program || !env->reaction || !env->verification)
        return refuse(e, RX_ENV_REFUSE_ARGUMENT, 1);
    if (!env->dependencies || !rx_dependency_manifest_matches(env->dependencies, env->dependency_root))
        return refuse(e, RX_ENV_REFUSE_DEPENDENCY, 16);
    const OmegaProgram *p = env->program;
    const RxReactionDesc *d = env->reaction;
    if (e) {
        e->subject = d->subject; e->world_generation = current_world_generation;
        e->program_id = env->program_id; e->realization_id = env->realization_id;
        e->causal_episode = env->causal_episode;
    }
    OmegaProgram copy = *p;
    if (omega_program_compute_id(&copy) != 0 || !id_equal(copy.program_id, env->program_id) ||
        !id_equal(p->program_id, env->program_id)) return refuse(e, RX_ENV_REFUSE_PROGRAM_ID, 2);
    RealizationObject real = p->realization;
    if (!real.has_id || omega_compute_realization_id(&real) != 0 ||
        !id_equal(real.realization_id, env->realization_id) ||
        !id_equal(p->realization.realization_id, env->realization_id)) return refuse(e, RX_ENV_REFUSE_REALIZATION_ID, 3);
    if (env->world_generation != current_world_generation)
        return refuse(e, RX_ENV_REFUSE_WORLD_GENERATION, 4);
    if (env->n_effects > RX_ENV_MAX_EFFECTS || d->n_caps > RX_MAX_CAPS ||
        d->n_reads > RX_MAX_DEPS || d->n_writes > RX_MAX_WRITES)
        return refuse(e, RX_ENV_REFUSE_ARGUMENT, 5);
    if (env->lifetime_deadline && now > env->lifetime_deadline)
        return refuse(e, RX_ENV_REFUSE_RESOURCE, 6);
    uint64_t cost = p->cost.insn_count;
    if (d->need.memory_bytes > memory_budget || cost > compute_budget)
        return refuse(e, RX_ENV_REFUSE_RESOURCE, 7);
    uint32_t min_tier = env->required_verification_tier;
    if (env->safety_class == RX_SAFETY_NATIVE_UNSAFE ||
        env->safety_class == RX_SAFETY_PHYSICAL_UNSAFE ||
        env->safety_class == RX_SAFETY_OPAQUE_FIRMWARE)
        return refuse(e, RX_ENV_REFUSE_SAFETY, 8);
    if (env->safety_class >= RX_SAFETY_MANAGED_RUNTIME && min_tier < VERIFY_TIER_V2)
        min_tier = VERIFY_TIER_V2;
    if (!env->verification->passed || env->verification->tier < min_tier ||
        env->verification->tier > VERIFY_TIER_V2)
        return refuse(e, RX_ENV_REFUSE_VERIFICATION, 9);
    for (uint32_t i = 0; i < d->n_caps; ++i) {
        RxCapRef cap = d->caps[i].ref;
        RxCapEntry entry;
        int rc = rx_world_validate_cap(world, cap, d->subject, d->caps[i].resource,
                                       d->caps[i].rights, &entry);
        if (e) { e->capability_id = cap.cap_id; e->capability_generation = cap.generation; }
        if (rc != RX_CAP_OK) return refuse(e, RX_ENV_REFUSE_CAPABILITY, (uint32_t)(16 - rc));
    }
    for (uint32_t i = 0; i < d->n_reads; ++i) {
        RxObject object;
        if (rx_world_read((RxWorld *)world, d->reads[i].obj, &object) != RX_OK)
            return refuse(e, RX_ENV_REFUSE_READ_SET, 10);
        bool allowed = false;
        for (uint32_t c = 0; c < d->n_caps; ++c)
            if ((d->caps[c].rights & RX_RIGHT_READ) && d->caps[c].resource == object.resource &&
                rx_world_validate_cap(world, d->caps[c].ref, d->subject, object.resource,
                                      RX_RIGHT_READ, NULL) == RX_CAP_OK) allowed = true;
        if (!allowed) return refuse(e, RX_ENV_REFUSE_READ_SET, 14);
    }
    for (uint32_t i = 0; i < d->n_writes; ++i) {
        RxObject object;
        if (rx_world_read((RxWorld *)world, d->writes[i].obj, &object) != RX_OK)
            return refuse(e, RX_ENV_REFUSE_WRITE_SET, 11);
        bool allowed = false;
        for (uint32_t c = 0; c < d->n_caps; ++c)
            if ((d->caps[c].rights & RX_RIGHT_WRITE) && d->caps[c].resource == object.resource &&
                rx_world_validate_cap(world, d->caps[c].ref, d->subject, object.resource,
                                      RX_RIGHT_WRITE, NULL) == RX_CAP_OK) allowed = true;
        if (!allowed) return refuse(e, RX_ENV_REFUSE_WRITE_SET, 15);
    }
    for (uint32_t i = 0; i < env->n_effects; ++i) {
        RxObject object;
        if (!dep_has(d->reads, d->n_reads, env->effects[i]) ||
            rx_world_read((RxWorld *)world, env->effects[i], &object) != RX_OK)
            { if (e) e->effect_object = env->effects[i].id; return refuse(e, RX_ENV_REFUSE_EFFECT, 12); }
        bool authorized = false;
        for (uint32_t c = 0; c < d->n_caps; ++c)
            if (d->caps[c].resource == object.resource && (d->caps[c].rights & RX_RIGHT_EFFECT)) authorized = true;
        if (!authorized) { if (e) e->effect_object = env->effects[i].id; return refuse(e, RX_ENV_REFUSE_EFFECT, 13); }
    }
    if (e) e->result = RX_ENV_ACCEPT;
    return RX_ENV_ACCEPT;
}
