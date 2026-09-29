/* POLYGLOT-0 lane F shared helpers. See omx_bench.h. */
#define _GNU_SOURCE
#include "polyglot/omx_bench.h"

#include "sha256.h"

#include <elf.h>
#include <link.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

uint64_t omx_rng_next(omx_rng *r) {
    uint64_t z = (r->s += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

double omx_rng_01(omx_rng *r) { return (double)(omx_rng_next(r) >> 11) * (1.0 / 9007199254740992.0); }

void omx_hex(const uint8_t *d, size_t n, char *out) {
    static const char k[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = k[d[i] >> 4];
        out[2 * i + 1] = k[d[i] & 15];
    }
    out[2 * n] = 0;
}

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap + 1);
    if (!b) { fclose(f); return NULL; }
    for (;;) {
        if (n == cap) {
            char *nb = realloc(b, cap * 2 + 1);
            if (!nb) { free(b); fclose(f); return NULL; }
            b = nb;
            cap *= 2;
        }
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (r == 0) break;
    }
    fclose(f);
    b[n] = 0;
    *len = n;
    return b;
}

int omx_sha256_file(const char *path, char hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[65536];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) sha256_update(&c, buf, r);
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    uint8_t d[32];
    sha256_final(&c, d);
    omx_hex(d, 32, hex);
    return 0;
}

int omx_contract_digest(const char *spec_path, char hex[65]) {
    size_t len = 0;
    char *b = slurp(spec_path, &len);
    if (!b) return -1;
    size_t start = (size_t)-1, end = len;
    for (size_t i = 0; i < len;) {
        /* i is the first byte of a line */
        if (start == (size_t)-1) {
            if (len - i >= 6 && memcmp(b + i, "## 1. ", 6) == 0) start = i;
        } else if (len - i >= 3 && memcmp(b + i, "## ", 3) == 0) {
            end = i;
            break;
        }
        const char *nl = memchr(b + i, '\n', len - i);
        if (!nl) break;
        i = (size_t)(nl - b) + 1;
    }
    if (start == (size_t)-1) { free(b); return -1; }
    uint8_t d[32];
    sha256_hash((const uint8_t *)b + start, end - start, d);
    omx_hex(d, 32, hex);
    free(b);
    return 0;
}

/* ---- machine ---- */
static long read_long(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long v = -1;
    if (fscanf(f, "%li", &v) != 1) v = -1;
    fclose(f);
    return v;
}

int omx_cpu_part(int cpu) {
    char p[160];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/regs/identification/midr_el1", cpu);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    unsigned long long v = 0;
    int ok = fscanf(f, "%llx", &v) == 1;
    fclose(f);
    return ok ? (int)((v >> 4) & 0xfff) : -1;
}

const char *omx_cluster_name(int part) {
    return part == 0xd85 ? "X925" : part == 0xd87 ? "A725" : "unknown";
}

const char *omx_core_name(int part) {
    return part == 0xd85 ? "Cortex-X925" : part == 0xd87 ? "Cortex-A725" : "unknown";
}

int omx_cpu_capacity(int cpu) {
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
    return (int)read_long(p);
}

long omx_cpu_cur_khz(int cpu) {
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    return read_long(p);
}

void omx_cpu_governor(int cpu, char *buf, size_t cap) {
    char p[128];
    snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", cpu);
    buf[0] = 0;
    FILE *f = fopen(p, "r");
    if (!f) { snprintf(buf, cap, "unknown"); return; }
    if (!fgets(buf, (int)cap, f)) buf[0] = 0;
    fclose(f);
    buf[strcspn(buf, "\n")] = 0;
    if (!buf[0]) snprintf(buf, cap, "unknown");
}

int omx_ncpus(void) {
    int n = 0;
    while (n < OMX_MAX_CPUS && omx_cpu_part(n) >= 0) n++;
    return n;
}

static int read_idle(unsigned long long idle[OMX_MAX_CPUS], unsigned long long tot[OMX_MAX_CPUS]) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return -1;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int c;
        unsigned long long u, ni, s, id, io, irq, sirq, st;
        if (sscanf(line, "cpu%d %llu %llu %llu %llu %llu %llu %llu %llu", &c, &u, &ni, &s, &id, &io, &irq,
                   &sirq, &st) == 9 && c >= 0 && c < OMX_MAX_CPUS) {
            idle[c] = id + io;
            tot[c] = u + ni + s + id + io + irq + sirq + st;
        }
    }
    fclose(f);
    return 0;
}

int omx_pick_idle_cpu(int part, double *idle_frac) {
    static unsigned long long i0[OMX_MAX_CPUS], t0[OMX_MAX_CPUS], i1[OMX_MAX_CPUS], t1[OMX_MAX_CPUS];
    read_idle(i0, t0);
    struct timespec ts = {0, 300000000};
    nanosleep(&ts, NULL);
    read_idle(i1, t1);
    int best = -1, n = omx_ncpus();
    double bi = -1;
    for (int c = 0; c < n; c++) {
        if (omx_cpu_part(c) != part) continue;
        double dt = (double)(t1[c] - t0[c]);
        double id = dt > 0 ? (double)(i1[c] - i0[c]) / dt : 0;
        if (id > bi) { bi = id; best = c; }
    }
    if (idle_frac) *idle_frac = bi;
    return best;
}

int omx_thermal_read(long mc[OMX_MAX_THERMAL]) {
    int n = 0;
    for (int z = 0; z < OMX_MAX_THERMAL; z++) {
        char p[96];
        snprintf(p, sizeof p, "/sys/class/thermal/thermal_zone%d/temp", z);
        long v = read_long(p);
        if (v == -1) break;
        mc[n++] = v;
    }
    return n;
}

void omx_loadavg(char *buf, size_t cap) {
    FILE *f = fopen("/proc/loadavg", "r");
    buf[0] = 0;
    if (!f) return;
    double a, b, c;
    if (fscanf(f, "%lf %lf %lf", &a, &b, &c) == 3) snprintf(buf, cap, "%.2f %.2f %.2f", a, b, c);
    fclose(f);
}

void omx_kernel(char *buf, size_t cap) {
    struct utsname u;
    if (uname(&u) == 0) snprintf(buf, cap, "%s %s %s", u.sysname, u.release, u.machine);
    else snprintf(buf, cap, "unknown");
}

uint64_t omx_run_delay_ns(void) {
    FILE *f = fopen("/proc/thread-self/schedstat", "r");
    unsigned long long a = 0, b = 0;
    if (f) {
        if (fscanf(f, "%llu %llu", &a, &b) != 2) b = 0;
        fclose(f);
    }
    return (uint64_t)b;
}

double omx_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- ELF ---- */
static int elf_ok(const unsigned char *b, size_t len) {
    return len >= sizeof(Elf64_Ehdr) && memcmp(b, ELFMAG, SELFMAG) == 0 && b[EI_CLASS] == ELFCLASS64 &&
           b[EI_DATA] == ELFDATA2LSB;
}

static const Elf64_Shdr *elf_shdrs(const unsigned char *b, size_t len, size_t *count) {
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)(const void *)b;
    if (eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr)) return NULL;
    if (eh->e_shoff > len || (len - eh->e_shoff) / sizeof(Elf64_Shdr) < eh->e_shnum) return NULL;
    *count = eh->e_shnum;
    return (const Elf64_Shdr *)(const void *)(b + eh->e_shoff);
}

long omx_elf_text_bytes(const char *path) {
    size_t len = 0;
    unsigned char *b = (unsigned char *)slurp(path, &len);
    if (!b) return -1;
    long total = -1;
    size_t n = 0;
    const Elf64_Shdr *sh = elf_ok(b, len) ? elf_shdrs(b, len, &n) : NULL;
    if (sh) {
        total = 0;
        for (size_t i = 0; i < n; i++)
            if ((sh[i].sh_flags & SHF_EXECINSTR) && sh[i].sh_type == SHT_PROGBITS) total += (long)sh[i].sh_size;
    }
    free(b);
    return total;
}

static int first_phdr_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    *(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
    return 1; /* the first entry is the main program */
}

size_t omx_self_symbol_size(const void *addr, char *name, size_t name_cap) {
    if (name && name_cap) name[0] = 0;
    uintptr_t bias = 0;
    dl_iterate_phdr(first_phdr_cb, &bias);
    size_t len = 0;
    unsigned char *b = (unsigned char *)slurp("/proc/self/exe", &len);
    if (!b) return 0;
    size_t n = 0, out = 0;
    const Elf64_Shdr *sh = elf_ok(b, len) ? elf_shdrs(b, len, &n) : NULL;
    uintptr_t a = (uintptr_t)addr - bias;
    for (size_t i = 0; sh && i < n && !out; i++) {
        if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= n) continue;
        const Elf64_Shdr *st = &sh[sh[i].sh_link];
        if (sh[i].sh_offset > len || sh[i].sh_size > len - sh[i].sh_offset) continue;
        if (st->sh_offset > len || st->sh_size > len - st->sh_offset) continue;
        const Elf64_Sym *sym = (const Elf64_Sym *)(const void *)(b + sh[i].sh_offset);
        size_t ns = sh[i].sh_size / sizeof(Elf64_Sym);
        for (size_t k = 0; k < ns; k++) {
            if (ELF64_ST_TYPE(sym[k].st_info) != STT_FUNC || sym[k].st_size == 0) continue;
            if (a >= sym[k].st_value && a < sym[k].st_value + sym[k].st_size) {
                out = sym[k].st_size;
                if (name && name_cap && sym[k].st_name < st->sh_size)
                    snprintf(name, name_cap, "%s", (const char *)b + st->sh_offset + sym[k].st_name);
                break;
            }
        }
    }
    free(b);
    return out;
}

/* ---- JSON ---- */
void omx_json_str(FILE *f, const char *s) {
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c == '\t') fputs("\\t", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

/* ---- statistics ---- */
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

void omx_sort_d(double *v, size_t n) { qsort(v, n, sizeof *v, cmp_d); }

double omx_median_sorted(const double *v, size_t n) {
    if (n == 0) return 0;
    return n & 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

double omx_mad(const double *v, size_t n, double med) {
    if (n == 0) return 0;
    double *d = malloc(n * sizeof *d);
    if (!d) return -1;
    for (size_t i = 0; i < n; i++) d[i] = v[i] > med ? v[i] - med : med - v[i];
    omx_sort_d(d, n);
    double r = omx_median_sorted(d, n);
    free(d);
    return r;
}
