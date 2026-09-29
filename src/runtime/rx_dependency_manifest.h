#ifndef RX_DEPENDENCY_MANIFEST_H
#define RX_DEPENDENCY_MANIFEST_H
#include "../omega_types.h"
#include <stdint.h>

#define RX_DEPENDENCY_MAX_ARTIFACTS 32u
typedef struct {
    SemanticId identity;
    uint8_t digest[32];
    uint32_t schema_version;
    SemanticId provenance_ref;
} RxDependencyArtifact;
typedef struct {
    uint32_t count;
    RxDependencyArtifact artifacts[RX_DEPENDENCY_MAX_ARTIFACTS];
} RxDependencyManifest;
int rx_dependency_manifest_root(const RxDependencyManifest *manifest, uint8_t out_root[32]);
int rx_dependency_manifest_matches(const RxDependencyManifest *manifest, const uint8_t root[32]);
#endif
