/* Lane 5: machine view tests. Prints "PASS n/n". */
#include "visor_machine.h"
#include "visor.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_total;
#define CHECK(cond, msg) do { g_total++; if (cond) g_pass++; else fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } while (0)

static char buf1[16384], buf2[16384];

int main(void) {
    OmegaMachineGraph mg, mg2;
    VisorMachineView v, v2;

    /* current machine: assumed profile, deterministic output */
    CHECK(visor_machine_current(&mg, &v) == 0, "visor_machine_current");
    CHECK(v.observed == false, "observed is false in V1");
    CHECK(strstr(v.provenance, "assumed:") == v.provenance, "provenance starts with assumed:");
    CHECK(strstr(v.provenance, "no physics descriptor ingested") != NULL, "provenance says no descriptor");
    CHECK(visor_machine_current(&mg2, &v2) == 0, "visor_machine_current again");
    int n1 = visor_machine_format_text(&v, buf1, sizeof(buf1));
    int n2 = visor_machine_format_text(&v2, buf2, sizeof(buf2));
    CHECK(n1 > 0 && n1 == n2 && strcmp(buf1, buf2) == 0, "text twice identical");
    n1 = visor_machine_format_json(&v, buf1, sizeof(buf1));
    n2 = visor_machine_format_json(&v2, buf2, sizeof(buf2));
    CHECK(n1 > 0 && n1 == n2 && strcmp(buf1, buf2) == 0 && buf1[0] == '{' && buf1[n1 - 1] == '}', "json twice identical");
    CHECK(strstr(buf1, "\"observed\":false") != NULL, "json observed false");
    CHECK(visor_machine_format_text(&v, buf1, 32) == -1, "truncation fails closed");

    /* host facts drive the same choice as current() */
    VisorHostFacts hf;
    CHECK(visor_machine_host_facts(&hf) == 0, "host facts");
    CHECK(visor_machine_from_facts(&hf, &mg2, &v2) == 0 && memcmp(&mg2.machine_id, &mg.machine_id, sizeof(SemanticId)) == 0,
          "current == from_facts(host)");

    /* both canonical profiles validate topology, ids stable and distinct */
    OmegaMachineGraph sp, sp2, qv, qv2;
    CHECK(omega_machine_build_dgx_spark(&sp) == 0 && omega_machine_build_dgx_spark(&sp2) == 0, "build spark");
    CHECK(omega_machine_build_qemu_virt(&qv) == 0 && omega_machine_build_qemu_virt(&qv2) == 0, "build qemu");
    CHECK(memcmp(&sp.machine_id, &sp2.machine_id, sizeof(SemanticId)) == 0, "spark id stable");
    CHECK(memcmp(&qv.machine_id, &qv2.machine_id, sizeof(SemanticId)) == 0, "qemu id stable");
    CHECK(memcmp(&sp.machine_id, &qv.machine_id, sizeof(SemanticId)) != 0, "spark != qemu id");
    VisorMachineView vs, vq;
    CHECK(visor_machine_view(&sp, &vs) == 0 && vs.topology_valid, "spark topology valid");
    CHECK(visor_machine_view(&qv, &vq) == 0 && vq.topology_valid, "qemu topology valid");
    CHECK(vs.issue_width == 4 && vq.issue_width == 2, "issue widths from builders");
    CHECK(vs.unit_count == 6 && vq.unit_count == 5 && strcmp(vs.units[0].type_name, "ALU") == 0, "units");
    CHECK(vs.cache_count == 4 && vq.cache_count == 3, "caches");
    CHECK(vs.physics_seal_is_placeholder && vq.physics_seal_is_placeholder, "builder seals flagged placeholder");
    char idtxt[72];
    visor_format_id(&sp.machine_id, idtxt);
    CHECK(strcmp(idtxt, vs.id_text) == 0 && strncmp(idtxt, "sha256:", 7) == 0, "id text");

    /* broken topology is reported, not hidden */
    OmegaMachineGraph bad = sp;
    bad.registers.gpr_count = 7;
    VisorMachineView vb;
    CHECK(visor_machine_view(&bad, &vb) == 0 && !vb.topology_valid && vb.topology_error[0], "invalid topology reported");

    /* profile selection from facts */
    VisorHostFacts f;
    memset(&f, 0, sizeof(f));
    snprintf(f.uname_machine, sizeof(f.uname_machine), "aarch64");
    snprintf(f.dmi_product, sizeof(f.dmi_product), "NVIDIA_DGX_Spark");
    snprintf(f.cpu_parts, sizeof(f.cpu_parts), "0xd85,0xd87");
    CHECK(visor_machine_from_facts(&f, &mg2, &v2) == 0 && memcmp(&mg2.machine_id, &sp.machine_id, sizeof(SemanticId)) == 0,
          "DGX Spark DMI -> dgx_spark profile");
    CHECK(strstr(v2.provenance, "do NOT match") != NULL && strstr(v2.provenance, "0xd85,0xd87") != NULL,
          "provenance records core mismatch verbatim");
    CHECK(!v2.observed, "spark facts still assumed");
    snprintf(f.dmi_product, sizeof(f.dmi_product), "Some Board");
    snprintf(f.cpu_parts, sizeof(f.cpu_parts), "0xd4f");
    CHECK(visor_machine_from_facts(&f, &mg2, &v2) == 0 && memcmp(&mg2.machine_id, &sp.machine_id, sizeof(SemanticId)) == 0,
          "Neoverse-V2 part -> dgx_spark profile");
    snprintf(f.uname_machine, sizeof(f.uname_machine), "x86_64");
    CHECK(visor_machine_from_facts(&f, &mg2, &v2) == 0 && memcmp(&mg2.machine_id, &qv.machine_id, sizeof(SemanticId)) == 0,
          "non-aarch64 -> qemu_virt profile");
    snprintf(f.uname_machine, sizeof(f.uname_machine), "aarch64");
    snprintf(f.cpu_parts, sizeof(f.cpu_parts), "0xd0c");
    CHECK(visor_machine_from_facts(&f, &mg2, &v2) == 0 && memcmp(&mg2.machine_id, &qv.machine_id, sizeof(SemanticId)) == 0,
          "unknown aarch64 -> qemu_virt profile");

    /* declared targets: aarch64 + Blackwell (run refused) */
    CHECK(v.target_count == 2, "two targets");
    CHECK(strcmp(v.targets[0].name, VISOR_TARGET_AARCH64) == 0 && v.targets[0].linked, "aarch64 target");
    CHECK(strcmp(v.targets[1].name, VISOR_TARGET_BLACKWELL) == 0, "blackwell target declared");
    CHECK(v.targets[1].linked, "blackwell realize objects linked in this test");
    CHECK(!v.targets[1].runnable && strstr(v.targets[1].note, "requires GPU submit authority") != NULL, "blackwell run refused");
    SemanticId bw;
    CHECK(visor_machine_blackwell_id(&bw) == 0 && strncmp(v.targets[1].machine_id, "sha256:", 7) == 0, "blackwell id");


    visor_machine_format_text(&v, buf1, sizeof(buf1));
    fputs(buf1, stdout);
    printf("%s %d/%d\n", g_pass == g_total ? "PASS" : "FAIL", g_pass, g_total);
    return g_pass == g_total ? 0 : 1;
}
