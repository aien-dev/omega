#include "runtime/rx_execution_envelope.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

typedef struct { uint32_t version, opcode; uint64_t value; } ChildRequest;
typedef struct { uint32_t version, status; uint64_t value; } ChildResponse;
static int transfer(int fd, void *buf, size_t n, int write_mode) {
    size_t done = 0;
    while (done < n) {
        ssize_t k = write_mode ? write(fd, (char *)buf + done, n-done)
                               : read(fd, (char *)buf + done, n-done);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        done += (size_t)k;
    }
    return 0;
}
static int run_child(const char *path, uint32_t opcode, uint64_t value, ChildResponse *response) {
    int to_child[2], from_child[2];
    if (pipe(to_child) || pipe(from_child)) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(to_child[1]); close(from_child[0]);
        char in[16], out[16]; snprintf(in,sizeof(in),"%d",to_child[0]);
        snprintf(out,sizeof(out),"%d",from_child[1]);
        execl(path,path,in,out,(char *)NULL); _exit(127);
    }
    close(to_child[0]); close(from_child[1]);
    if (pid < 0) return -1;
    ChildRequest request = {1, opcode, value};
    int wr = transfer(to_child[1],&request,sizeof(request),1); close(to_child[1]);
    int rd = transfer(from_child[0],response,sizeof(*response),0); close(from_child[0]);
    int status=0; waitpid(pid,&status,0);
    if (wr || rd || !WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return response->version == 1 && response->status == 0 ? 0 : -1;
}

int main(void) {
    RxDependencyManifest manifest = {0};
    manifest.count = 1;
    manifest.artifacts[0].identity.bytes[0] = 1;
    manifest.artifacts[0].digest[0] = 2;
    manifest.artifacts[0].schema_version = 1;
    manifest.artifacts[0].provenance_ref.bytes[0] = 3;
    uint8_t root[32];
    CHECK(rx_dependency_manifest_root(&manifest, root) == 0);
    CHECK(rx_dependency_manifest_matches(&manifest, root));
    RxDependencyManifest reordered = manifest;
    CHECK(rx_dependency_manifest_matches(&reordered, root));
    reordered.artifacts[0].digest[0] ^= 1;
    CHECK(!rx_dependency_manifest_matches(&reordered, root));

    RxCapRoot caproot; RxCapAdmin admin; RxWorld world;
    CHECK(rx_caproot_start(&caproot, &admin) == RX_CAP_OK);
    CHECK(rx_world_init(&world, &caproot, 1, 64) == RX_OK);
    ChildResponse child_response;
    CHECK(run_child("build/omega_foreign_child", 1, 13, &child_response) == 0);
    CHECK(child_response.value == 40);
    CHECK(run_child("build/omega_foreign_child", 99, 0, &child_response) != 0);
    CHECK(run_child("build/omega_foreign_child", 98, 0, &child_response) != 0);
    CHECK(run_child("build/omega_foreign_child", 2, 0xdeadbeef, &child_response) != 0);
    OmegaProgram p;
    CHECK(omega_program_build_unary_op(&p, "add-one", OP_ADD, 1) == 0);
    RxReactionDesc reaction; memset(&reaction, 0, sizeof(reaction));
    reaction.name = "envelope-control"; reaction.subject = 42;
    VerifyReport verification = { .passed = true, .tier = VERIFY_TIER_V2 };
    RxExecutionEnvelope env; memset(&env, 0, sizeof(env));
    env.program = &p; env.reaction = &reaction; env.verification = &verification;
    env.dependencies = &manifest; memcpy(env.dependency_root, root, 32);
    env.program_id = p.program_id; env.realization_id = p.realization.realization_id;
    env.world_generation = 7; env.required_verification_tier = VERIFY_TIER_V1;
    env.safety_class = RX_SAFETY_NATIVE_CONSTRAINED; env.causal_episode = 19;
    RxEnvelopeEvidence evidence;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 32, &evidence) == RX_ENV_ACCEPT);
    CHECK(evidence.subject == 42 && evidence.causal_episode == 19);
    env.world_generation++;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 32, &evidence) == RX_ENV_REFUSE_WORLD_GENERATION);
    CHECK(evidence.invariant != 0 && evidence.result == RX_ENV_REFUSE_WORLD_GENERATION);
    env.world_generation = 7; env.program_id.bytes[0] ^= 1;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 32, &evidence) == RX_ENV_REFUSE_PROGRAM_ID);
    env.program_id = p.program_id; env.realization_id.bytes[0] ^= 1;
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_REALIZATION_ID);
    env.realization_id = p.realization.realization_id;
    env.program_id = p.program_id; env.dependency_root[0] ^= 1;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 32, &evidence) == RX_ENV_REFUSE_DEPENDENCY);
    memcpy(env.dependency_root, root, 32); env.required_verification_tier = VERIFY_TIER_V3;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 32, &evidence) == RX_ENV_REFUSE_VERIFICATION);
    env.required_verification_tier = VERIFY_TIER_V1; env.safety_class = RX_SAFETY_NATIVE_UNSAFE;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 32, &evidence) == RX_ENV_REFUSE_SAFETY);
    env.safety_class = RX_SAFETY_NATIVE_CONSTRAINED;
    CHECK(rx_execution_envelope_validate(&env, &world, 7, 0, 1024, 1, &evidence) == RX_ENV_REFUSE_RESOURCE);
    struct timespec t0,t1; clock_gettime(CLOCK_MONOTONIC,&t0);
    for (int i=0;i<100;i++) CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_ACCEPT);
    clock_gettime(CLOCK_MONOTONIC,&t1);
    uint64_t elapsed = (uint64_t)(t1.tv_sec-t0.tv_sec)*1000000000ull +
                       (uint64_t)(t1.tv_nsec-t0.tv_nsec);
    uint64_t init[RX_MAX_FIELDS] = {0}; RxObjRef object;
    CHECK(rx_world_create(&world, 1, RX_PERSIST_RESIDENT, 0x55, init, &object) == RX_OK);
    reaction.n_reads = reaction.n_writes = reaction.n_caps = 1;
    reaction.reads[0].obj = reaction.writes[0].obj = object;
    reaction.reads[0].mask = reaction.writes[0].mask = 1;
    RxCapMint mint = {0}; mint.issuer = 3; mint.subject = reaction.subject;
    mint.resource = 0x55; mint.rights = RX_RIGHT_READ | RX_RIGHT_WRITE;
    mint.parent = (RxCapRef){UINT32_MAX,0}; mint.authority = rx_capadmin_office(&admin);
    RxCapRef cap; CHECK(rx_capadmin_mint(&admin,&mint,&cap) == RX_CAP_OK);
    reaction.caps[0] = (RxCapNeed){cap,0x55,RX_RIGHT_READ | RX_RIGHT_WRITE};
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_ACCEPT);
    reaction.caps[0].ref = (RxCapRef){UINT32_MAX, 1};
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_CAPABILITY);
    reaction.caps[0].ref = (RxCapRef){cap.cap_id, cap.generation + 1};
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_CAPABILITY);
    reaction.caps[0].ref = cap; reaction.subject++;
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_CAPABILITY);
    reaction.subject--;
    reaction.caps[0].rights |= RX_RIGHT_EFFECT;
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_CAPABILITY);
    reaction.caps[0].rights &= ~RX_RIGHT_EFFECT;
    CHECK(rx_capadmin_revoke(&admin,rx_capadmin_office(&admin),cap) == RX_CAP_OK);
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_CAPABILITY);
    mint.rights = RX_RIGHT_READ | RX_RIGHT_WRITE; mint.lease_ticks = 1;
    CHECK(rx_capadmin_mint(&admin,&mint,&cap) == RX_CAP_OK);
    reaction.caps[0] = (RxCapNeed){cap,0x55,RX_RIGHT_READ | RX_RIGHT_WRITE};
    CHECK(rx_capadmin_advance_clock(&admin,rx_capadmin_office(&admin),1) == RX_CAP_OK);
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_CAPABILITY);
    mint.lease_ticks = 0;
    CHECK(rx_capadmin_mint(&admin,&mint,&cap) == RX_CAP_OK);
    reaction.caps[0] = (RxCapNeed){cap,0x55,RX_RIGHT_READ | RX_RIGHT_WRITE};
    RxObjRef stale = object; stale.generation++;
    reaction.reads[0].obj = stale;
    CHECK(rx_execution_envelope_validate(&env,&world,7,0,1024,32,&evidence) == RX_ENV_REFUSE_READ_SET);
    omega_program_destroy(&p); rx_world_destroy(&world); rx_caproot_stop(&caproot, &admin);
    puts("OMEGA_EXEC_ENVELOPE_IDENTITY_PASS");
    puts("OMEGA_EXEC_ENVELOPE_AUTHORITY_PASS");
    puts("OMEGA_EXEC_ENVELOPE_WORLD_PASS");
    puts("OMEGA_EXEC_ENVELOPE_RESOURCE_PASS");
    puts("OMEGA_EXEC_ENVELOPE_FOREIGN_PARTIAL (process protocol only; ambient OS access not contained)");
    puts("OMEGA_EXEC_ENVELOPE_EVIDENCE_PASS");
    printf("OMEGA_EXEC_ENVELOPE_PERF_RECORDED validation_ns_per_call=%llu metadata_bytes=%zu allocations=not_instrumented resident_overhead=not_measured steady_state_overhead=not_measured\n",
           (unsigned long long)(elapsed/100), sizeof(env));
    return 0;
}
