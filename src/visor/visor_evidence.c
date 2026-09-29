/* Omega Visor V1 - lane 4: evidence (receipt) inspection. See visor_evidence.h. */
#include "visor_evidence.h"

#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "omega_canonical.h"
#include "sha256.h"

/* ------------------------------------------------------------------ */
/* Strict JSON scanner over one fixed buffer and one fixed token pool. */
/* ------------------------------------------------------------------ */
enum { JT_OBJ = 1, JT_ARR, JT_STR, JT_PRIM };
#define J_MAX_TOKENS 32768
#define J_MAX_DEPTH 64

typedef struct {
    uint8_t type;
    uint8_t is_key;
    int32_t start, end;   /* STR: content [start,end); others: [start,end) raw */
    int32_t next;         /* index of the first token after this subtree */
} JTok;

static char g_buf[VISOR_EVIDENCE_MAX_FILE_BYTES + 1];
static size_t g_len;
static JTok g_tok[J_MAX_TOKENS];
static int g_ntok;

static void j_ws(size_t *p) {
    while (*p < g_len && (g_buf[*p] == ' ' || g_buf[*p] == '\t' || g_buf[*p] == '\n' || g_buf[*p] == '\r')) (*p)++;
}

static int j_new(uint8_t type, size_t start) {
    if (g_ntok >= J_MAX_TOKENS) return -1;
    JTok *t = &g_tok[g_ntok];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->start = (int32_t)start;
    return g_ntok++;
}

static int j_string(size_t *p, bool is_key) {
    if (*p >= g_len || g_buf[*p] != '"') return -1;
    int t = j_new(JT_STR, *p + 1);
    if (t < 0) return -1;
    g_tok[t].is_key = is_key;
    size_t i = *p + 1;
    while (i < g_len) {
        unsigned char c = (unsigned char)g_buf[i];
        if (c == '"') break;
        if (c < 0x20) return -1;
        if (c == '\\') {
            if (i + 1 >= g_len) return -1;
            char e = g_buf[i + 1];
            if (e == 'u') {
                if (i + 5 >= g_len) return -1;
                for (int k = 2; k < 6; ++k) if (!isxdigit((unsigned char)g_buf[i + k])) return -1;
                i += 6;
                continue;
            }
            if (!strchr("\"\\/bfnrt", e)) return -1;
            i += 2;
            continue;
        }
        i++;
    }
    if (i >= g_len) return -1;
    g_tok[t].end = (int32_t)i;
    g_tok[t].next = g_ntok;
    *p = i + 1;
    return t;
}

static int j_prim(size_t *p) {
    size_t s = *p;
    int t = j_new(JT_PRIM, s);
    if (t < 0) return -1;
    static const char *const lits[] = { "true", "false", "null" };
    for (int k = 0; k < 3; ++k) {
        size_t L = strlen(lits[k]);
        if (s + L <= g_len && memcmp(g_buf + s, lits[k], L) == 0) { *p = s + L; goto done; }
    }
    {
        size_t i = s;
        if (i < g_len && g_buf[i] == '-') i++;
        if (i >= g_len || !isdigit((unsigned char)g_buf[i])) return -1;
        if (g_buf[i] == '0') i++; else while (i < g_len && isdigit((unsigned char)g_buf[i])) i++;
        if (i < g_len && g_buf[i] == '.') {
            i++;
            if (i >= g_len || !isdigit((unsigned char)g_buf[i])) return -1;
            while (i < g_len && isdigit((unsigned char)g_buf[i])) i++;
        }
        if (i < g_len && (g_buf[i] == 'e' || g_buf[i] == 'E')) {
            i++;
            if (i < g_len && (g_buf[i] == '+' || g_buf[i] == '-')) i++;
            if (i >= g_len || !isdigit((unsigned char)g_buf[i])) return -1;
            while (i < g_len && isdigit((unsigned char)g_buf[i])) i++;
        }
        *p = i;
    }
done:
    g_tok[t].end = (int32_t)*p;
    g_tok[t].next = g_ntok;
    return t;
}

static int j_value(size_t *p, int depth) {
    if (depth > J_MAX_DEPTH) return -1;
    j_ws(p);
    if (*p >= g_len) return -1;
    char c = g_buf[*p];
    if (c == '"') return j_string(p, false);
    if (c != '{' && c != '[') return j_prim(p);
    int t = j_new(c == '{' ? JT_OBJ : JT_ARR, *p);
    if (t < 0) return -1;
    char close = c == '{' ? '}' : ']';
    (*p)++;
    j_ws(p);
    if (*p < g_len && g_buf[*p] == close) {
        (*p)++;
    } else {
        for (;;) {
            if (c == '{') {
                j_ws(p);
                if (j_string(p, true) < 0) return -1;
                j_ws(p);
                if (*p >= g_len || g_buf[*p] != ':') return -1;
                (*p)++;
            }
            if (j_value(p, depth + 1) < 0) return -1;
            j_ws(p);
            if (*p >= g_len) return -1;
            if (g_buf[*p] == ',') { (*p)++; continue; }
            if (g_buf[*p] == close) { (*p)++; break; }
            return -1;
        }
    }
    g_tok[t].end = (int32_t)*p;
    g_tok[t].next = g_ntok;
    return t;
}

/* Parse g_buf[0..g_len). Top level must be an object. */
static int j_parse(void) {
    g_ntok = 0;
    size_t p = 0;
    int t = j_value(&p, 0);
    if (t != 0 || g_tok[0].type != JT_OBJ) return -1;
    j_ws(&p);
    return p == g_len ? 0 : -1;
}

static bool j_key_eq(int i, const char *key) {
    if (i < 0 || g_tok[i].type != JT_STR) return false;
    size_t L = (size_t)(g_tok[i].end - g_tok[i].start);
    return strlen(key) == L && memcmp(g_buf + g_tok[i].start, key, L) == 0;
}

/* Value token of `key` directly inside object `obj`, or -1. */
static int j_get(int obj, const char *key) {
    if (obj < 0 || g_tok[obj].type != JT_OBJ) return -1;
    for (int k = obj + 1; k < g_tok[obj].next; k = g_tok[k + 1].next)
        if (j_key_eq(k, key)) return k + 1;
    return -1;
}

/* First value of `key` anywhere, in document order, or -1. */
static int j_find_any(const char *key) {
    for (int i = 0; i < g_ntok; ++i)
        if (g_tok[i].is_key && j_key_eq(i, key)) return i + 1;
    return -1;
}

static size_t j_copy(int i, char *out, size_t n) {
    if (!out || n == 0) return 0;
    out[0] = 0;
    if (i < 0 || (g_tok[i].type != JT_STR && g_tok[i].type != JT_PRIM)) return 0;
    size_t L = (size_t)(g_tok[i].end - g_tok[i].start);
    if (L >= n) L = n - 1;
    memcpy(out, g_buf + g_tok[i].start, L);
    out[L] = 0;
    return L;
}

static bool j_is_lit(int i, const char *lit) {
    return i >= 0 && g_tok[i].type == JT_PRIM &&
           (size_t)(g_tok[i].end - g_tok[i].start) == strlen(lit) &&
           memcmp(g_buf + g_tok[i].start, lit, strlen(lit)) == 0;
}

static bool j_u32(int i, uint32_t *out) {
    char tmp[24];
    if (i < 0 || g_tok[i].type != JT_PRIM) return false;
    j_copy(i, tmp, sizeof(tmp));
    if (!isdigit((unsigned char)tmp[0])) return false;
    char *end = NULL;
    unsigned long v = strtoul(tmp, &end, 10);
    if (!end || *end || v > UINT32_MAX) return false;
    *out = (uint32_t)v;
    return true;
}

static bool is_hex_n(const char *s, size_t n) {
    if (strlen(s) != n) return false;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)s[i])) return false;
    return true;
}

/* ------------------------------------------------------------------ */
/* Record extraction                                                   */
/* ------------------------------------------------------------------ */
const char *visor_qual_scope_name(VisorQualScope s) {
    switch (s) {
        case VISOR_QUAL_UNKNOWN: return "UNKNOWN";
        case VISOR_QUAL_SIMULATED: return "SIMULATED";
        case VISOR_QUAL_HOST: return "HOST";
        case VISOR_QUAL_QEMU: return "QEMU";
        case VISOR_QUAL_SILICON: return "SILICON";
    }
    return "UNKNOWN";
}

static void first_hex_str(const char *const *keys, size_t nkeys, size_t hexlen, bool anywhere,
                          char *out, size_t n, bool prefix_sha) {
    char tmp[80];
    for (size_t k = 0; k < nkeys; ++k) {
        int v = anywhere ? j_find_any(keys[k]) : j_get(0, keys[k]);
        if (v < 0 || g_tok[v].type != JT_STR) continue;
        j_copy(v, tmp, sizeof(tmp));
        if (!is_hex_n(tmp, hexlen)) continue;
        snprintf(out, n, "%s%s", prefix_sha ? "sha256:" : "", tmp);
        return;
    }
}

static void add_part(char *out, size_t n, const char *label, int v) {
    char tmp[64];
    if (v < 0 || g_tok[v].type != JT_STR || j_copy(v, tmp, sizeof(tmp)) == 0) return;
    size_t len = strlen(out);
    if (len >= n - 1) return;
    snprintf(out + len, n - len, "%s%s=%s", len ? " " : "", label, tmp);
}

static void fill_machine(VisorEvidenceRecord *r) {
    int mi = j_get(0, "machine_identity");
    if (mi >= 0 && g_tok[mi].type == JT_OBJ) {
        add_part(r->machine, sizeof(r->machine), "node", j_get(mi, "node"));
        add_part(r->machine, sizeof(r->machine), "arch", j_get(mi, "architecture"));
        add_part(r->machine, sizeof(r->machine), "kernel", j_get(mi, "release"));
        return;
    }
    int h = j_get(0, "host");
    if (h >= 0 && g_tok[h].type == JT_OBJ) {
        add_part(r->machine, sizeof(r->machine), "os", j_get(h, "sysname"));
        add_part(r->machine, sizeof(r->machine), "os", j_get(h, "os"));
        add_part(r->machine, sizeof(r->machine), "arch", j_get(h, "machine"));
        add_part(r->machine, sizeof(r->machine), "arch", j_get(h, "arch"));
        add_part(r->machine, sizeof(r->machine), "kernel", j_get(h, "release"));
        add_part(r->machine, sizeof(r->machine), "core", j_get(h, "core_class_midr_part"));
    }
}

/* Count PASS/FAIL string verdicts in a gates object; remember a headline gate. */
static void count_gates(int obj, VisorEvidenceRecord *r, char *headline, size_t hn) {
    if (obj < 0 || g_tok[obj].type != JT_OBJ) return;
    char key[96], val[16];
    for (int k = obj + 1; k < g_tok[obj].next; k = g_tok[k + 1].next) {
        int v = k + 1;
        if (g_tok[v].type != JT_STR) continue;
        j_copy(v, val, sizeof(val));
        bool pass = strcmp(val, "PASS") == 0, fail = strcmp(val, "FAIL") == 0;
        if (!pass && !fail) continue;
        r->gates_run++;
        if (pass) r->gates_passed++;
        j_copy(k, key, sizeof(key));
        size_t kl = strlen(key);
        /* Prefer the last *_PASS gate (the rollup); else the first verdict seen. */
        if ((kl > 5 && strcmp(key + kl - 5, "_PASS") == 0) || !headline[0])
            snprintf(headline, hn, "%.90s=%s", key, val);
    }
}

static void fill_gates_and_claim(VisorEvidenceRecord *r) {
    char headline[112] = {0}, label[96] = {0}, status[64] = {0};
    int qg = j_get(0, "qualification_gates");
    uint32_t tot = 0, pas = 0;
    if (qg >= 0 && j_u32(j_get(qg, "total"), &tot) && j_u32(j_get(qg, "passed"), &pas)) {
        r->gates_run = tot;
        r->gates_passed = pas;
    } else {
        count_gates(j_get(0, "gates"), r, headline, sizeof(headline));
        count_gates(j_get(0, "gate"), r, headline, sizeof(headline));
        if (qg >= 0) count_gates(j_get(qg, "gates"), r, headline, sizeof(headline));
    }
    static const char *const label_keys[] = { "schema", "milestone", "stage_title", "contract_id" };
    for (size_t i = 0; i < 4 && !label[0]; ++i) j_copy(j_get(0, label_keys[i]), label, sizeof(label));
    j_copy(j_get(0, "status"), status, sizeof(status));
    const char *what = headline[0] ? headline : status;
    if (label[0] && what[0]) snprintf(r->claim, sizeof(r->claim), "%.60s: %.64s", label, what);
    else if (label[0]) snprintf(r->claim, sizeof(r->claim), "%.120s", label);
    else snprintf(r->claim, sizeof(r->claim), "%.120s", what);
}

static void scope_signal(VisorQualScope s, const char *basis, VisorQualScope *cur, bool *have,
                         VisorEvidenceRecord *r) {
    if (!*have || s < *cur) {
        *cur = s;
        snprintf(r->scope_basis, sizeof(r->scope_basis), "%s", basis);
    }
    *have = true;
}

static void fill_scope(VisorEvidenceRecord *r) {
    VisorQualScope cur = VISOR_QUAL_UNKNOWN;
    bool have = false;
    int so = j_get(0, "silicon_observed");
    if (j_is_lit(so, "true")) scope_signal(VISOR_QUAL_SILICON, "silicon_observed=true", &cur, &have, r);
    if (j_is_lit(so, "false")) scope_signal(VISOR_QUAL_HOST, "silicon_observed=false", &cur, &have, r);
    if (j_find_any("gpu_uuid") >= 0) scope_signal(VISOR_QUAL_SILICON, "gpu_uuid", &cur, &have, r);
    if (j_find_any("compute_class") >= 0) scope_signal(VISOR_QUAL_SILICON, "compute_class", &cur, &have, r);
    /* Only an explicit execution target counts; gate names or machine-model
     * names that merely mention QEMU (e.g. *_QEMU_VIRT_PASS) do not. */
    if (j_find_any("qemu_target") >= 0) scope_signal(VISOR_QUAL_QEMU, "qemu_target", &cur, &have, r);
    static const char *const scope_keys[] = { "hardware_scope", "scope" };
    for (size_t k = 0; k < 2; ++k) {
        int v = j_get(0, scope_keys[k]);
        if (v < 0 || g_tok[v].type != JT_STR) continue;
        char s[256];
        j_copy(v, s, sizeof(s));
        for (char *c = s; *c; ++c) *c = (char)tolower((unsigned char)*c);
        char basis[96];
        snprintf(basis, sizeof(basis), "%s", scope_keys[k]);
        if (strstr(s, "simulat")) scope_signal(VISOR_QUAL_SIMULATED, basis, &cur, &have, r);
        if (strncmp(s, "host", 4) == 0 || strstr(s, "no silicon claim"))
            scope_signal(VISOR_QUAL_HOST, basis, &cur, &have, r);
        if (strstr(s, "qemu")) scope_signal(VISOR_QUAL_QEMU, basis, &cur, &have, r);
    }
    r->scope = have ? cur : VISOR_QUAL_UNKNOWN;
    if (!have) r->scope_basis[0] = 0;
    r->scope_name = visor_qual_scope_name(r->scope);
}

static void fill_tree(VisorEvidenceRecord *r) {
    int td = j_get(0, "tree_dirty");
    if (j_is_lit(td, "true") || j_is_lit(td, "false")) {
        r->tree_state_known = true;
        r->tree_dirty = j_is_lit(td, "true");
        return;
    }
    int tc = j_get(0, "candidate_trees_clean");
    if (tc >= 0 && g_tok[tc].type == JT_OBJ) {
        r->tree_state_known = true;
        for (int k = tc + 1; k < g_tok[tc].next; k = g_tok[k + 1].next)
            if (j_is_lit(k + 1, "false")) r->tree_dirty = true;
    }
}

static int name_hash_match(const char *path, const char *receipt_hex) {
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (strlen(base) != 64 + 5 || strcmp(base + 64, ".json") != 0) return -1;
    char name[65];
    memcpy(name, base, 64);
    name[64] = 0;
    if (!is_hex_n(name, 64)) return -1;
    return strcmp(name, receipt_hex) == 0 ? 1 : 0;
}

/* Read file into g_buf. -1 if unreadable, not a regular file, or oversize. */
static int read_file(const char *path) {
    g_len = 0;
    g_buf[0] = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(g_buf, 1, sizeof(g_buf), f);
    int err = ferror(f);
    fclose(f);
    if (err || n > VISOR_EVIDENCE_MAX_FILE_BYTES) return -1;   /* read one extra byte => oversize */
    g_len = n;
    g_buf[n] = 0;
    return 0;
}

/* Build a record from g_buf (already read). */
static int record_from_buf(const char *path, VisorEvidenceRecord *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->scope = VISOR_QUAL_UNKNOWN;
    out->scope_name = visor_qual_scope_name(VISOR_QUAL_UNKNOWN);
    out->name_hash_match = -1;
    if (memchr(g_buf, 0, g_len)) return -1;           /* binary content */
    if (j_parse() != 0) return -1;

    uint8_t d[SHA256_DIGEST_SIZE];
    sha256_hash((const uint8_t *)g_buf, g_len, d);
    SemanticId sid;
    memcpy(sid.bytes, d, OMEGA_ID_BYTES);
    char hex[65];
    omega_hex_semantic_id(&sid, hex);
    snprintf(out->receipt_id, sizeof(out->receipt_id), "sha256:%s", hex);
    out->name_hash_match = name_hash_match(path, hex);

    static const char *const commit_keys[] = {
        "candidate_commit", "run_commit", "git_commit", "candidate_git_commit", "qualified_implementation_commit"
    };
    first_hex_str(commit_keys, 5, 40, false, out->commit, sizeof(out->commit), false);
    static const char *const phys_keys[] = { "physics_commit", "physics_forge_commit" };
    first_hex_str(phys_keys, 2, 40, false, out->physics_commit, sizeof(out->physics_commit), false);
    if (!out->physics_commit[0])
        first_hex_str(phys_keys, 2, 40, true, out->physics_commit, sizeof(out->physics_commit), false);
    int rid = j_get(0, "run_id");
    if (rid >= 0 && g_tok[rid].type == JT_STR) j_copy(rid, out->run_id, sizeof(out->run_id));
    static const char *const real_keys[] = {
        "realization_id", "selected_realization_identity", "promoted_realization_identity", "compiler_realization_id"
    };
    first_hex_str(real_keys, 4, 64, true, out->realization_id, sizeof(out->realization_id), true);
    fill_machine(out);
    fill_tree(out);
    fill_gates_and_claim(out);
    fill_scope(out);
    return 0;
}

int visor_evidence_load(const char *path, VisorEvidenceRecord *out) {
    if (!path || !out) return -1;
    if (read_file(path) != 0) {
        memset(out, 0, sizeof(*out));
        snprintf(out->path, sizeof(out->path), "%s", path);
        out->scope_name = visor_qual_scope_name(VISOR_QUAL_UNKNOWN);
        out->name_hash_match = -1;
        return -1;
    }
    return record_from_buf(path, out);
}

/* ------------------------------------------------------------------ */
/* Directory walk (sorted, deterministic)                              */
/* ------------------------------------------------------------------ */
#define VISOR_EVIDENCE_MAX_FILES 1024
static char g_paths[VISOR_EVIDENCE_MAX_FILES][256];
static size_t g_npaths;
static bool g_paths_overflow;

static void walk(const char *dir, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char p[256];
        int w = snprintf(p, sizeof(p), "%s%s%s", dir, dir[strlen(dir) - 1] == '/' ? "" : "/", e->d_name);
        if (w < 0 || (size_t)w >= sizeof(p)) { g_paths_overflow = true; continue; }
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { walk(p, depth + 1); continue; }
        if (!S_ISREG(st.st_mode)) continue;               /* symlinks and devices are not followed */
        if (g_npaths >= VISOR_EVIDENCE_MAX_FILES) { g_paths_overflow = true; continue; }
        memcpy(g_paths[g_npaths++], p, sizeof(p));
    }
    closedir(d);
}

static int cmp_path(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int collect(const char *root) {
    struct stat st;
    if (!root || !root[0] || stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return -1;
    g_npaths = 0;
    g_paths_overflow = false;
    walk(root, 0);
    qsort(g_paths, g_npaths, sizeof(g_paths[0]), cmp_path);
    return 0;
}

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static void view_reset(VisorEvidenceView *v) { memset(v, 0, sizeof(*v)); }

static void note_unparsed(VisorEvidenceView *v, const char *path) {
    v->unparsed++;
    if (v->unparsed_listed < VISOR_EVIDENCE_MAX_UNPARSED_PATHS)
        snprintf(v->unparsed_paths[v->unparsed_listed++], 256, "%s", path);
}

static void view_add(VisorEvidenceView *v, const VisorEvidenceRecord *r) {
    v->matched++;
    if (v->count < VISOR_EVIDENCE_MAX_ITEMS) v->items[v->count++] = *r;
    else v->truncated = true;
}

int visor_evidence_scan(const char *evidence_root, VisorEvidenceView *out) {
    if (!out) return -1;
    view_reset(out);
    if (collect(evidence_root) != 0) return -1;
    static VisorEvidenceRecord rec;
    for (size_t i = 0; i < g_npaths; ++i) {
        out->scanned++;
        if (!ends_with(g_paths[i], ".json") || visor_evidence_load(g_paths[i], &rec) != 0) {
            note_unparsed(out, g_paths[i]);
            continue;
        }
        view_add(out, &rec);
    }
    if (g_paths_overflow) out->truncated = true;
    return 0;
}

int visor_evidence_for_id(const char *evidence_root, const SemanticId *id, VisorEvidenceView *out) {
    if (!out) return -1;
    view_reset(out);
    if (!id || collect(evidence_root) != 0) return -1;
    char hex[65];
    omega_hex_semantic_id(id, hex);
    static VisorEvidenceRecord rec;
    for (size_t i = 0; i < g_npaths; ++i) {
        out->scanned++;
        if (read_file(g_paths[i]) != 0) { note_unparsed(out, g_paths[i]); continue; }   /* could hide a mention */
        if (!memmem(g_buf, g_len, hex, 64)) continue;
        /* Mentions the id: parse it, or report it UNPARSED with its path. */
        if (!ends_with(g_paths[i], ".json") || record_from_buf(g_paths[i], &rec) != 0) {
            note_unparsed(out, g_paths[i]);
            continue;
        }
        view_add(out, &rec);
    }
    if (g_paths_overflow) out->truncated = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */
typedef struct { char *p; size_t n, len; bool overflow; } EBuf;

static void eb_printf(EBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void eb_printf(EBuf *b, const char *fmt, ...) {
    if (b->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(b->p + b->len, b->n - b->len, fmt, ap);
    va_end(ap);
    if (w < 0 || (size_t)w >= b->n - b->len) { b->overflow = true; b->p[b->len] = 0; return; }
    b->len += (size_t)w;
}

static void eb_json_str(EBuf *b, const char *s) {
    eb_printf(b, "\"");
    for (const unsigned char *c = (const unsigned char *)(s ? s : ""); *c && !b->overflow; ++c) {
        if (*c == '"' || *c == '\\') eb_printf(b, "\\%c", *c);
        else if (*c < 0x20) eb_printf(b, "\\u%04x", *c);
        else eb_printf(b, "%c", *c);
    }
    eb_printf(b, "\"");
}

static const char *or_none(const char *s) { return (s && s[0]) ? s : "(none)"; }

int visor_evidence_format_text(const VisorEvidenceView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    EBuf b = { out, n, 0, false };
    out[0] = 0;
    eb_printf(&b, "evidence: %zu shown, %zu matched, %zu scanned, %zu unparsed%s\n",
              v->count, v->matched, v->scanned, v->unparsed, v->truncated ? " (truncated)" : "");
    if (v->count == 0) eb_printf(&b, "no evidence\n");
    for (size_t i = 0; i < v->count; ++i) {
        const VisorEvidenceRecord *r = &v->items[i];
        eb_printf(&b, "receipt      %s%s\n", r->receipt_id,
                  r->name_hash_match == 1 ? " (name = content hash)" :
                  r->name_hash_match == 0 ? " (NAME != CONTENT HASH)" : "");
        eb_printf(&b, "  path       %s\n", r->path);
        eb_printf(&b, "  claim      %s\n", or_none(r->claim));
        eb_printf(&b, "  scope      %s%s%s%s\n", r->scope_name ? r->scope_name : "UNKNOWN",
                  r->scope_basis[0] ? " (from " : "", r->scope_basis, r->scope_basis[0] ? ")" : "");
        eb_printf(&b, "  gates      %u/%u passed\n", r->gates_passed, r->gates_run);
        eb_printf(&b, "  commit     %s\n", or_none(r->commit));
        eb_printf(&b, "  physics    %s\n", or_none(r->physics_commit));
        eb_printf(&b, "  run        %s\n", or_none(r->run_id));
        eb_printf(&b, "  machine    %s\n", or_none(r->machine));
        eb_printf(&b, "  realization %s\n", or_none(r->realization_id));
        eb_printf(&b, "  tree       %s\n", !r->tree_state_known ? "unknown" : r->tree_dirty ? "DIRTY" : "clean");
    }
    for (size_t i = 0; i < v->unparsed_listed; ++i) eb_printf(&b, "UNPARSED     %s\n", v->unparsed_paths[i]);
    if (v->unparsed > v->unparsed_listed)
        eb_printf(&b, "UNPARSED     (+%zu more)\n", v->unparsed - v->unparsed_listed);
    return b.overflow ? -1 : (int)b.len;
}

int visor_evidence_format_json(const VisorEvidenceView *v, char *out, size_t n) {
    if (!v || !out || n == 0) return -1;
    EBuf b = { out, n, 0, false };
    out[0] = 0;
    eb_printf(&b, "{\"count\":%zu,\"matched\":%zu,\"scanned\":%zu,\"unparsed\":%zu,\"truncated\":%s,\"receipts\":[",
              v->count, v->matched, v->scanned, v->unparsed, v->truncated ? "true" : "false");
    for (size_t i = 0; i < v->count; ++i) {
        const VisorEvidenceRecord *r = &v->items[i];
        eb_printf(&b, "%s{\"receipt_id\":", i ? "," : "");
        eb_json_str(&b, r->receipt_id);
        eb_printf(&b, ",\"path\":"); eb_json_str(&b, r->path);
        eb_printf(&b, ",\"claim\":"); eb_json_str(&b, r->claim);
        eb_printf(&b, ",\"scope\":\"%s\",\"scope_basis\":", r->scope_name ? r->scope_name : "UNKNOWN");
        eb_json_str(&b, r->scope_basis);
        eb_printf(&b, ",\"gates_run\":%u,\"gates_passed\":%u,\"commit\":", r->gates_run, r->gates_passed);
        eb_json_str(&b, r->commit);
        eb_printf(&b, ",\"physics_commit\":"); eb_json_str(&b, r->physics_commit);
        eb_printf(&b, ",\"run_id\":"); eb_json_str(&b, r->run_id);
        eb_printf(&b, ",\"machine\":"); eb_json_str(&b, r->machine);
        eb_printf(&b, ",\"realization_id\":"); eb_json_str(&b, r->realization_id);
        eb_printf(&b, ",\"tree\":\"%s\",\"name_hash_match\":%d}",
                  !r->tree_state_known ? "unknown" : r->tree_dirty ? "dirty" : "clean", r->name_hash_match);
    }
    eb_printf(&b, "],\"unparsed_paths\":[");
    for (size_t i = 0; i < v->unparsed_listed; ++i) {
        if (i) eb_printf(&b, ",");
        eb_json_str(&b, v->unparsed_paths[i]);
    }
    eb_printf(&b, "]}");
    return b.overflow ? -1 : (int)b.len;
}
