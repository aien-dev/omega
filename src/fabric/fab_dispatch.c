/* fab_dispatch.c -- Fabric dispatch of remote composition candidates (fab_dispatch.h). */
#include "fab_dispatch.h"

#include <string.h>

int fab_dispatch_init(FabDispatch *d, FabNode *node) {
    if (!d || !node || !node->cfg.catalog) return FAB_E_ARG;
    memset(d, 0, sizeof *d);
    if (pthread_mutex_init(&d->mu, NULL) != 0) return FAB_E_ARG;
    d->node = node;              /* set only once the lock exists (destroy keys on it) */
    return FAB_OK;
}

void fab_dispatch_destroy(FabDispatch *d) {
    if (d && d->node) pthread_mutex_destroy(&d->mu);
    if (d) d->node = NULL;
}

int fab_dispatch_add(FabDispatch *d, const AienMachineId *m, const AgSkillTable *skills) {
    if (!d || !d->node || !m || !skills) return FAB_E_ARG;
    if (aien_mid_equal(m, &d->node->cfg.self)) return FAB_E_ARG;   /* local Skills run locally */
    pthread_mutex_lock(&d->mu);
    if (d->n_exec >= FAB_DISPATCH_MAX) {
        pthread_mutex_unlock(&d->mu);
        return FAB_E_FULL;
    }
    for (uint32_t i = 0; i < d->n_exec; i++)
        if (aien_mid_equal(&d->exec[i].machine, m)) {
            pthread_mutex_unlock(&d->mu);
            return FAB_E_ARG;
        }
    d->exec[d->n_exec++] = (FabExecutor){ *m, skills };
    pthread_mutex_unlock(&d->mu);
    return FAB_OK;
}

/* Caller holds the lock. */
static int pump_locked(FabDispatch *d, uint64_t now_us) {
    int n = 0;
    FabVerdict v;
    for (;;) {
        int rc = fab_poll(d->node, now_us, &v);
        if (rc < 0) return rc;
        if (rc == 0) break;
        n++;
        d->polled++;
        if (v.code != FAB_OK) d->refused_msgs++;
    }
    (void)fab_tick(d->node, now_us);
    return n;
}

int fab_dispatch_pump(FabDispatch *d, uint64_t now_us) {
    if (!d || !d->node) return FAB_E_ARG;
    pthread_mutex_lock(&d->mu);
    int n = pump_locked(d, now_us);
    pthread_mutex_unlock(&d->mu);
    return n;
}

static int refuse(FabDispatch *d, int why) {
    d->refused[why]++;
    d->last_refusal = why;
    return why;
}

int fab_dispatch_run(void *ctx, const SrRouter *router, const SrRoute *route, uint64_t input,
                     uint64_t now_us, uint64_t *result) {
    FabDispatch *d = ctx;
    if (!d || !d->node) return FAB_DX_ARG;
    pthread_mutex_lock(&d->mu);
    int rc;
    if (!router || !route || !result || router->graph != d->node->cfg.catalog) {
        rc = refuse(d, FAB_DX_ARG);
        goto out;
    }
    if (pump_locked(d, now_us) < 0) { rc = refuse(d, FAB_DX_TRANSPORT); goto out; }
    if (sr_route_check(router, route, now_us) != SR_E_REMOTE) { rc = refuse(d, FAB_DX_ROUTE); goto out; }
    if (!route->target_known) { rc = refuse(d, FAB_DX_TARGET); goto out; }
    JsHome home;
    if (fab_home(d->node, &route->target, now_us, &home) != FAB_OK ||
        home.locality != JS_HOME_REMOTE_OWNED) {
        rc = refuse(d, FAB_DX_NOT_MEMBER);
        goto out;
    }
    const FabExecutor *ex = NULL;
    for (uint32_t i = 0; i < d->n_exec && !ex; i++)
        if (aien_mid_equal(&d->exec[i].machine, &route->target)) ex = &d->exec[i];
    const AgSkill *s = NULL;
    for (uint32_t i = 0; ex && i < ex->skills->n && !s; i++)
        if (ex->skills->skill[i].id == route->chosen.skill_id) s = &ex->skills->skill[i];
    if (!s || !s->fn) { rc = refuse(d, FAB_DX_NO_EXECUTOR); goto out; }
    /* The machine runs what it advertised, or nothing: an unpinned (zero)
     * advertisement is refused too, since nothing would bind the result. */
    uint8_t any = 0;
    for (int i = 0; i < 32; i++) any |= route->skill_digest[i];
    if (!any || memcmp(s->identity, route->skill_digest, 32) != 0) { rc = refuse(d, FAB_DX_DIGEST); goto out; }
    uint64_t in = input;
    int failed = 0;
    uint64_t r = s->fn(&in, 1, 0, &failed);
    if (failed) { rc = refuse(d, FAB_DX_FAILED); goto out; }
    *result = r;
    d->dispatched++;
    rc = FAB_DX_OK;
out:
    pthread_mutex_unlock(&d->mu);
    return rc;
}
