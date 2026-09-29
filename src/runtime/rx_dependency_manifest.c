#include "rx_dependency_manifest.h"
#include "../sha256.h"
#include <stdlib.h>
#include <string.h>

static int cmp_artifact(const void *aa, const void *bb) {
    const RxDependencyArtifact *a = aa, *b = bb;
    return memcmp(a->identity.bytes, b->identity.bytes, sizeof(a->identity.bytes));
}
int rx_dependency_manifest_root(const RxDependencyManifest *m, uint8_t out[32]) {
    if (!m || !out || m->count > RX_DEPENDENCY_MAX_ARTIFACTS) return -1;
    RxDependencyArtifact sorted[RX_DEPENDENCY_MAX_ARTIFACTS];
    memcpy(sorted, m->artifacts, m->count * sizeof(sorted[0]));
    qsort(sorted, m->count, sizeof(sorted[0]), cmp_artifact);
    for (uint32_t i = 1; i < m->count; ++i)
        if (cmp_artifact(&sorted[i-1], &sorted[i]) == 0) return -1;
    sha256_ctx c; sha256_init(&c);
    static const uint8_t domain[] = "omega.dependency-manifest.v1";
    uint8_t count[4] = {(uint8_t)(m->count >> 24), (uint8_t)(m->count >> 16),
                        (uint8_t)(m->count >> 8), (uint8_t)m->count};
    sha256_update(&c, domain, sizeof(domain)); sha256_update(&c, count, sizeof(count));
    for (uint32_t i = 0; i < m->count; ++i) {
        uint8_t schema[4] = {(uint8_t)(sorted[i].schema_version >> 24),
                             (uint8_t)(sorted[i].schema_version >> 16),
                             (uint8_t)(sorted[i].schema_version >> 8), (uint8_t)sorted[i].schema_version};
        sha256_update(&c, sorted[i].identity.bytes, sizeof(sorted[i].identity.bytes));
        sha256_update(&c, sorted[i].digest, sizeof(sorted[i].digest));
        sha256_update(&c, schema, sizeof(schema));
        sha256_update(&c, sorted[i].provenance_ref.bytes, sizeof(sorted[i].provenance_ref.bytes));
    }
    sha256_final(&c, out); return 0;
}
int rx_dependency_manifest_matches(const RxDependencyManifest *m, const uint8_t root[32]) {
    uint8_t computed[32];
    return root && rx_dependency_manifest_root(m, computed) == 0 && memcmp(computed, root, 32) == 0;
}
