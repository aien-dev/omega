/*
 * visor_machine.c -- Omega Visor V1, lane 5: machine view (read-only).
 * See visor_machine.h. No physics headers, no authority, fixed storage.
 */
#include "visor_machine.h"
#include "visor.h"
#include "aarch64_target.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

/* Weak reference: resolves only when omega_blackwell_realize.o is linked.
 * The Visor never submits GPU work; the id is shown as a declared target. */
extern int omega_blackwell_get_machine_id(SemanticId *out_machine_id) __attribute__((weak));

int visor_machine_blackwell_id(SemanticId *out_id) {
    if (!out_id) return -1;
    if (!omega_blackwell_get_machine_id) return -1;
    return omega_blackwell_get_machine_id(out_id) == 0 ? 0 : -1;
}

const char *visor_machine_unit_name(ComputeUnitType t) {
    switch (t) {
        case UNIT_ALU: return "ALU";
        case UNIT_BRANCH: return "BRANCH";
        case UNIT_MULTIPLIER: return "MULTIPLIER";
        case UNIT_DIVIDER: return "DIVIDER";
        case UNIT_LOAD_STORE: return "LOAD_STORE";
        case UNIT_VECTOR: return "VECTOR";
        case UNIT_ACCELERATOR_PORT: return "ACCELERATOR_PORT";
    }
    return "UNKNOWN";
}

/* ---- host facts ---------------------------------------------------------- */

static void read_first_line(const char *path, char *out, size_t n) {
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)n, f)) {
        size_t l = strlen(out);
        while (l > 0 && (out[l - 1] == '\n' || out[l - 1] == '\r' || out[l - 1] == ' ')) out[--l] = '\0';
    }
    fclose(f);
}

static void collect_cpu_parts(char *out, size_t n) {
    char parts[16][12];
    size_t count = 0;
    out[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "CPU part", 8) != 0) continue;
        const char *c = strchr(line, ':');
        if (!c) continue;
        c++;
        while (*c == ' ' || *c == '\t') c++;
        char val[12];
        size_t k = 0;
        while (*c && *c != '\n' && *c != ' ' && k + 1 < sizeof(val)) val[k++] = *c++;
        val[k] = '\0';
        bool dup = false;
        for (size_t i = 0; i < count; ++i) if (strcmp(parts[i], val) == 0) dup = true;
        if (!dup && count < 16) { memcpy(parts[count], val, sizeof(val)); count++; }
    }
    fclose(f);
    /* sort for determinism */
    for (size_t i = 0; i < count; ++i)
        for (size_t j = i + 1; j < count; ++j)
            if (strcmp(parts[j], parts[i]) < 0) {
                char t[12]; memcpy(t, parts[i], 12); memcpy(parts[i], parts[j], 12); memcpy(parts[j], t, 12);
            }
    size_t pos = 0;
    for (size_t i = 0; i < count; ++i) {
        int w = snprintf(out + pos, n - pos, "%s%s", i ? "," : "", parts[i]);
        if (w < 0 || (size_t)w >= n - pos) break;
        pos += (size_t)w;
    }
}

int visor_machine_host_facts(VisorHostFacts *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    struct utsname u;
    if (uname(&u) == 0) snprintf(out->uname_machine, sizeof(out->uname_machine), "%.31s", u.machine);
    read_first_line("/sys/devices/virtual/dmi/id/product_name", out->dmi_product, sizeof(out->dmi_product));
    collect_cpu_parts(out->cpu_parts, sizeof(out->cpu_parts));
    return 0;
}

/* ---- view ---------------------------------------------------------------- */

static bool seal_is_constant(const uint8_t seal[32]) {
    for (size_t i = 1; i < 32; ++i) if (seal[i] != seal[0]) return false;
    return true;
}

static void fill_targets(const OmegaMachineGraph *mg, VisorMachineView *v) {
    size_t t = 0;
    VisorMachineTarget *a = &v->targets[t++];
    snprintf(a->name, sizeof(a->name), "%s", VISOR_TARGET_AARCH64);
    visor_format_id(&mg->machine_id, a->machine_id);
    a->linked = true;
#if defined(__aarch64__)
    a->runnable = true;
    a->note = "pure AArch64 realizations run natively on this host via omega_exec_native_f3";
#else
    a->runnable = false;
    a->note = "host is not aarch64; run refused";
#endif

    VisorMachineTarget *b = &v->targets[t++];
    snprintf(b->name, sizeof(b->name), "%s", VISOR_TARGET_BLACKWELL);
    SemanticId bw;
    if (visor_machine_blackwell_id(&bw) == 0) {
        visor_format_id(&bw, b->machine_id);
        b->linked = true;
        b->note = "declared target (omega_blackwell_get_machine_id); run refused: requires GPU submit authority; not linked in Visor V1";
    } else {
        snprintf(b->machine_id, sizeof(b->machine_id), "none");
        b->linked = false;
        b->note = "declared target, Blackwell realize objects not linked into this binary; run refused";
    }
    b->runnable = false;
    v->target_count = t;
}

int visor_machine_view(const OmegaMachineGraph *mg, VisorMachineView *out) {
    if (!mg || !out) return -1;
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", mg->name);
    visor_format_id(&mg->machine_id, out->id_text);
    out->target_profile = mg->target_profile;
    out->observed = false;
    snprintf(out->provenance, sizeof(out->provenance),
             "assumed: machine graph as given; no physics descriptor ingested (V1)");
    out->issue_width = mg->pipeline.issue_width;
    out->max_in_flight = mg->pipeline.max_in_flight;
    out->out_of_order = mg->pipeline.out_of_order;
    out->unit_count = mg->pipeline.unit_count > OMEGA_MACHINE_MAX_UNITS ? OMEGA_MACHINE_MAX_UNITS
                                                                        : mg->pipeline.unit_count;
    for (size_t i = 0; i < out->unit_count; ++i) {
        const MachineComputeUnit *u = &mg->pipeline.units[i];
        out->units[i].type_name = visor_machine_unit_name(u->type);
        out->units[i].count = u->count;
        out->units[i].latency = u->latency_cycles;
        out->units[i].throughput = u->throughput_per_cycle;
    }
    out->gpr_count = mg->registers.gpr_count;
    out->gpr_width = mg->registers.gpr_width_bits;
    out->vec_count = mg->registers.vector_count;
    out->vec_width = mg->registers.vector_width_bits;
    out->cache_count = mg->cache_count > OMEGA_MACHINE_MAX_CACHES ? OMEGA_MACHINE_MAX_CACHES : mg->cache_count;
    for (size_t i = 0; i < out->cache_count; ++i) {
        const MachineCacheLevel *c = &mg->caches[i];
        out->caches[i] = (VisorMachineCache){ c->level, c->is_instruction, c->size_bytes,
                                              c->line_size_bytes, c->associativity, c->latency_cycles };
    }
    out->dram_base = mg->dram_base;
    out->dram_size = mg->dram_size;
    out->physics_authorized = mg->is_physics_authorized;
    out->physics_seal_is_placeholder = seal_is_constant(mg->physics_receipt_seal);
    char err[96] = {0};
    out->topology_valid = omega_machine_validate_topology(mg, err, sizeof(err)) == 0;
    if (!out->topology_valid) snprintf(out->topology_error, sizeof(out->topology_error), "%s", err);
    fill_targets(mg, out);
    return 0;
}

int visor_machine_from_facts(const VisorHostFacts *f, OmegaMachineGraph *out_mg, VisorMachineView *out_view) {
    if (!f || !out_mg || !out_view) return -1;
    bool aarch64 = strcmp(f->uname_machine, "aarch64") == 0;
    bool dmi_spark = strstr(f->dmi_product, "DGX_Spark") || strstr(f->dmi_product, "DGX Spark");
    bool nv2 = strstr(f->cpu_parts, "0xd4f") != NULL;
    bool spark = aarch64 && (dmi_spark || nv2);
    int rc = spark ? omega_machine_build_dgx_spark(out_mg) : omega_machine_build_qemu_virt(out_mg);
    if (rc != 0) return -1;
    if (visor_machine_view(out_mg, out_view) != 0) return -1;
    const char *fit;
    if (!spark) fit = "fallback profile";
    else if (nv2) fit = "host cores include Neoverse-V2 (0xd4f)";
    else fit = "profile labels Neoverse-V2 4-wide; host cores do NOT match (no 0xd4f)";
    snprintf(out_view->provenance, sizeof(out_view->provenance),
             "assumed: omega_machine_build_%s(); host uname=%.31s dmi=%.63s cpu_parts=%.63s; %s; "
             "no physics descriptor ingested; seal is builder constant",
             spark ? "dgx_spark" : "qemu_virt",
             f->uname_machine[0] ? f->uname_machine : "?",
             f->dmi_product[0] ? f->dmi_product : "?",
             f->cpu_parts[0] ? f->cpu_parts : "?", fit);
    out_view->observed = false;
    return 0;
}

int visor_machine_current(OmegaMachineGraph *out_mg, VisorMachineView *out_view) {
    VisorHostFacts f;
    if (visor_machine_host_facts(&f) != 0) return -1;
    return visor_machine_from_facts(&f, out_mg, out_view);
}

/* ---- formatting ---------------------------------------------------------- */

typedef struct { char *buf; size_t n, pos; bool trunc; } Out;

static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void outf(Out *o, const char *fmt, ...) {
    if (o->trunc) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->buf + o->pos, o->n - o->pos, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= o->n - o->pos) { o->trunc = true; o->buf[o->n - 1] = '\0'; return; }
    o->pos += (size_t)w;
}

static void json_str(Out *o, const char *s) {
    outf(o, "\"");
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') outf(o, "\\%c", c);
        else if (c < 0x20) outf(o, "\\u%04x", c);
        else outf(o, "%c", c);
    }
    outf(o, "\"");
}

int visor_machine_format_text(const VisorMachineView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "machine %s\n", v->name);
    outf(&o, "  id          %s\n", v->id_text);
    outf(&o, "  profile     0x%02x\n", v->target_profile);
    outf(&o, "  observed    %s\n", v->observed ? "yes" : "no (assumed canonical profile)");
    outf(&o, "  provenance  %s\n", v->provenance);
    outf(&o, "  pipeline    issue=%u in_flight=%u %s\n", v->issue_width, v->max_in_flight,
         v->out_of_order ? "out-of-order" : "in-order");
    for (size_t i = 0; i < v->unit_count; ++i)
        outf(&o, "    unit %-16s count=%u latency=%u throughput=%u\n", v->units[i].type_name,
             v->units[i].count, v->units[i].latency, v->units[i].throughput);
    outf(&o, "  registers   gpr=%ux%u vec=%ux%u\n", v->gpr_count, v->gpr_width, v->vec_count, v->vec_width);
    for (size_t i = 0; i < v->cache_count; ++i)
        outf(&o, "    cache L%u%s size=%" PRIu64 " line=%u assoc=%u latency=%u\n", v->caches[i].level,
             v->caches[i].is_instruction ? "I" : (v->caches[i].level == 1 ? "D" : " "), v->caches[i].size, v->caches[i].line,
             v->caches[i].assoc, v->caches[i].latency);
    outf(&o, "  dram        base=0x%" PRIx64 " size=%" PRIu64 "\n", v->dram_base, v->dram_size);
    outf(&o, "  physics     flag=%s seal=%s\n", v->physics_authorized ? "set" : "unset",
         v->physics_seal_is_placeholder ? "builder-constant placeholder (not a receipt)" : "non-constant");
    outf(&o, "  topology    %s%s\n", v->topology_valid ? "valid" : "INVALID: ",
         v->topology_valid ? "" : v->topology_error);
    for (size_t i = 0; i < v->target_count; ++i)
        outf(&o, "  target %-22s linked=%s runnable=%s id=%s\n    note: %s\n", v->targets[i].name,
             v->targets[i].linked ? "yes" : "no", v->targets[i].runnable ? "yes" : "no",
             v->targets[i].machine_id, v->targets[i].note ? v->targets[i].note : "");
    return o.trunc ? -1 : (int)o.pos;
}

int visor_machine_format_json(const VisorMachineView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    Out o = { out, n, 0, false };
    out[0] = '\0';
    outf(&o, "{\"name\":"); json_str(&o, v->name);
    outf(&o, ",\"id\":"); json_str(&o, v->id_text);
    outf(&o, ",\"target_profile\":%u,\"observed\":%s,\"provenance\":", v->target_profile,
         v->observed ? "true" : "false");
    json_str(&o, v->provenance);
    outf(&o, ",\"pipeline\":{\"issue_width\":%u,\"max_in_flight\":%u,\"out_of_order\":%s,\"units\":[",
         v->issue_width, v->max_in_flight, v->out_of_order ? "true" : "false");
    for (size_t i = 0; i < v->unit_count; ++i)
        outf(&o, "%s{\"type\":\"%s\",\"count\":%u,\"latency\":%u,\"throughput\":%u}", i ? "," : "",
             v->units[i].type_name, v->units[i].count, v->units[i].latency, v->units[i].throughput);
    outf(&o, "]},\"registers\":{\"gpr_count\":%u,\"gpr_width\":%u,\"vec_count\":%u,\"vec_width\":%u},\"caches\":[",
         v->gpr_count, v->gpr_width, v->vec_count, v->vec_width);
    for (size_t i = 0; i < v->cache_count; ++i)
        outf(&o, "%s{\"level\":%u,\"instruction\":%s,\"size\":%" PRIu64 ",\"line\":%u,\"assoc\":%u,\"latency\":%u}",
             i ? "," : "", v->caches[i].level, v->caches[i].is_instruction ? "true" : "false",
             v->caches[i].size, v->caches[i].line, v->caches[i].assoc, v->caches[i].latency);
    outf(&o, "],\"dram_base\":%" PRIu64 ",\"dram_size\":%" PRIu64
             ",\"physics_authorized\":%s,\"physics_seal_is_placeholder\":%s,\"topology_valid\":%s,\"topology_error\":",
         v->dram_base, v->dram_size, v->physics_authorized ? "true" : "false",
         v->physics_seal_is_placeholder ? "true" : "false", v->topology_valid ? "true" : "false");
    json_str(&o, v->topology_error);
    outf(&o, ",\"targets\":[");
    for (size_t i = 0; i < v->target_count; ++i) {
        outf(&o, "%s{\"name\":", i ? "," : "");
        json_str(&o, v->targets[i].name);
        outf(&o, ",\"machine_id\":");
        json_str(&o, v->targets[i].machine_id);
        outf(&o, ",\"linked\":%s,\"runnable\":%s,\"note\":", v->targets[i].linked ? "true" : "false",
             v->targets[i].runnable ? "true" : "false");
        json_str(&o, v->targets[i].note);
        outf(&o, "}");
    }
    outf(&o, "]}");
    return o.trunc ? -1 : (int)o.pos;
}
