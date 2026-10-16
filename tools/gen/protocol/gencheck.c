/*
 * gencheck.c -- strict validator of the GEN protocol manifest (aien-spark/omega#476).
 * Manifest: tests/one_layer/generalization/protocol.json (schema omega.gen.protocol.v1), the machine copy of
 * docs/one_layer/GEN_PROTOCOL_V1.md. Family rules: tests/one_layer/generalization/families/FAMSIG_V1.md,
 * through famsig_lib.h (the same code as famsig). Deterministic: no clock, no randomness, no environment.
 *
 *   gencheck manifest <protocol.json> <root>
 *       Parse the manifest with a strict JSON reader (ASCII only; duplicate keys, unknown keys, missing keys
 *       and wrong types are refused), check every frozen v1 constant, re-hash every pinned file under <root>,
 *       check the dev split (task list = META ids = case directories, families = META signatures) and, once
 *       the corpus META is frozen, apply the disjointness rule R1..R7 over dev + corpus.
 *       Record-set manifests (C and D) are re-hashed once their paths and digests are set.
 *   gencheck run-ready <protocol.json> <root>
 *       Everything `manifest` checks with nothing pending: any null field, any interface file missing from
 *       <root>, or no `interfaces` entry named by retrieval.contract (genret-cli-v3) refuses. Required before
 *       the seal is opened and before the run; there is no "pending merge" continue.
 *   gencheck open <protocol.json> <root> <OPEN>
 *       The seal-open check (custodian, in a clean checkout of the OPEN commit; reducer, on protocol.json taken
 *       from that commit): everything `manifest` checks with nothing pending (every interface file present,
 *       corpus META, prefix, both record sets, HELDOUT.pub and tasks digests set), and the OPEN receipt
 *       ("gen-open 1", omega_commit, protocol_sha256, tasks_sha256, heldout_pub_sha256) matches protocol.json.
 *   gencheck split <protocol.json> <root> <SPLIT> <HELDOUT.pub>
 *       Everything `manifest` checks, then (reviewer, after the seal): SPLIT parses (gen-split 1), HELDOUT.pub
 *       parses (gen-heldout-pub 1) and matches SPLIT (split_sha256, famlist_sha256, task ids, counts), the
 *       held-out count and minimum family count of the manifest hold, and R1..R7 pass over dev + corpus +
 *       SPLIT. Fails closed when splits.heldout.pub_sha256 or tasks_sha256 is null in the manifest. Held-out
 *       signatures are never printed.
 * Output: "ok ..." lines on stdout, then "GENCHECK PASS" (exit 0). A refusal prints "gencheck: FAIL <reason>"
 * on stderr and "GENCHECK FAIL" on stdout (exit 1). Usage errors exit 2.
 */
#include "famsig_lib.h"
#include <dirent.h>
#include <stdarg.h>
#include <sys/stat.h>

#define GC_MAXENT 1024
#define GC_PATH 192
#define GC_DEPTH 8

typedef struct {
    char path[GC_PATH];          /* e.g. splits.dev.tasks[3] */
    char norm[GC_PATH];          /* indices replaced: splits.dev.tasks[] */
    char type;                   /* o a s i n b z (object array string integer number bool null) */
    char *raw;                   /* string contents, number text, true/false/null; NULL for o and a */
} Ent;

static Ent ent[GC_MAXENT];
static int nent;

static void gc_fail(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
static void gc_fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("gencheck: FAIL ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    printf("GENCHECK FAIL\n");
    exit(1);
}

/* ---------- strict JSON reader ---------- */

typedef struct { const char *s; size_t i, n; } Jp;

static void j_ws(Jp *p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\n' || p->s[p->i] == '\t')) p->i++; }

static Ent *j_add(const char *path, const char *norm, char type, char *raw)
{
    if (nent >= GC_MAXENT) gc_fail("json: more than %d values", GC_MAXENT);
    Ent *e = &ent[nent++];
    snprintf(e->path, sizeof e->path, "%s", path);
    snprintf(e->norm, sizeof e->norm, "%s", norm);
    e->type = type;
    e->raw = raw;
    return e;
}

static char *j_string(Jp *p)
{
    if (p->i >= p->n || p->s[p->i] != '"') gc_fail("json: expected string at byte %zu", p->i);
    p->i++;
    char *out = malloc(p->n + 1);
    size_t k = 0;
    for (;;) {
        if (p->i >= p->n) gc_fail("json: unterminated string");
        char c = p->s[p->i++];
        if (c == '"') break;
        if (c == '\\') {
            if (p->i >= p->n) gc_fail("json: bad escape");
            char e = p->s[p->i++];
            if (e != '"' && e != '\\' && e != '/') gc_fail("json: escape \\%c not allowed (only \\\" \\\\ \\/)", e);
            out[k++] = e;
            continue;
        }
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) gc_fail("json: non-printable byte in string at %zu", p->i - 1);
        out[k++] = c;
    }
    out[k] = 0;
    return out;
}

static int j_isdig(char c) { return c >= '0' && c <= '9'; }

static void j_value(Jp *p, const char *path, const char *norm, int depth);

static void j_object(Jp *p, const char *path, const char *norm, int depth)
{
    j_add(path, norm, 'o', NULL);
    p->i++;
    char *keys[128];
    int nk = 0;
    j_ws(p);
    if (p->i < p->n && p->s[p->i] == '}') gc_fail("json: empty object at %s", path[0] ? path : "(root)");
    for (;;) {
        j_ws(p);
        char *k = j_string(p);
        size_t kl = strlen(k);
        if (kl < 1 || kl > 40) gc_fail("json: key length at %s", path);
        for (size_t i = 0; i < kl; i++)
            if (!((k[i] >= 'a' && k[i] <= 'z') || j_isdig(k[i]) || k[i] == '_')) gc_fail("json: key \"%s\" is not [a-z0-9_]", k);
        for (int i = 0; i < nk; i++)
            if (!strcmp(keys[i], k)) gc_fail("json: duplicate key \"%s\" in %s", k, path[0] ? path : "(root)");
        if (nk == 128) gc_fail("json: too many keys in %s", path);
        keys[nk++] = k;
        j_ws(p);
        if (p->i >= p->n || p->s[p->i] != ':') gc_fail("json: expected ':' after \"%s\"", k);
        p->i++;
        char cp[GC_PATH], cn[GC_PATH];
        if (snprintf(cp, sizeof cp, "%s%s%s", path, path[0] ? "." : "", k) >= (int)sizeof cp ||
            snprintf(cn, sizeof cn, "%s%s%s", norm, norm[0] ? "." : "", k) >= (int)sizeof cn) gc_fail("json: path too long");
        j_value(p, cp, cn, depth + 1);
        j_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; break; }
        gc_fail("json: expected ',' or '}' in %s", path[0] ? path : "(root)");
    }
    for (int i = 0; i < nk; i++) free(keys[i]);
}

static void j_array(Jp *p, const char *path, const char *norm, int depth)
{
    j_add(path, norm, 'a', NULL);
    p->i++;
    j_ws(p);
    if (p->i < p->n && p->s[p->i] == ']') gc_fail("json: empty array at %s", path);
    char cn[GC_PATH];
    if (snprintf(cn, sizeof cn, "%s[]", norm) >= (int)sizeof cn) gc_fail("json: path too long");
    for (int idx = 0;; idx++) {
        char cp[GC_PATH];
        if (snprintf(cp, sizeof cp, "%s[%d]", path, idx) >= (int)sizeof cp) gc_fail("json: path too long");
        j_ws(p);
        j_value(p, cp, cn, depth + 1);
        j_ws(p);
        if (p->i < p->n && p->s[p->i] == ',') { p->i++; continue; }
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; break; }
        gc_fail("json: expected ',' or ']' in %s", path);
    }
}

static void j_value(Jp *p, const char *path, const char *norm, int depth)
{
    if (depth > GC_DEPTH) gc_fail("json: nesting deeper than %d", GC_DEPTH);
    j_ws(p);
    if (p->i >= p->n) gc_fail("json: unexpected end");
    char c = p->s[p->i];
    if (c == '{') { j_object(p, path, norm, depth); return; }
    if (c == '[') { j_array(p, path, norm, depth); return; }
    if (c == '"') { j_add(path, norm, 's', j_string(p)); return; }
    static const char *const lit[3] = {"true", "false", "null"};
    for (int i = 0; i < 3; i++) {
        size_t l = strlen(lit[i]);
        if (p->n - p->i >= l && !memcmp(p->s + p->i, lit[i], l)) {
            p->i += l;
            j_add(path, norm, i < 2 ? 'b' : 'z', strdup(lit[i]));
            return;
        }
    }
    /* number: -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)? */
    size_t st = p->i;
    int isint = 1;
    if (p->s[p->i] == '-') p->i++;
    if (p->i < p->n && p->s[p->i] == '0') p->i++;
    else if (p->i < p->n && p->s[p->i] >= '1' && p->s[p->i] <= '9') while (p->i < p->n && j_isdig(p->s[p->i])) p->i++;
    else gc_fail("json: bad value at %s", path);
    if (p->i < p->n && p->s[p->i] == '.') {
        isint = 0; p->i++;
        if (p->i >= p->n || !j_isdig(p->s[p->i])) gc_fail("json: bad number at %s", path);
        while (p->i < p->n && j_isdig(p->s[p->i])) p->i++;
    }
    if (p->i < p->n && (p->s[p->i] == 'e' || p->s[p->i] == 'E')) {
        isint = 0; p->i++;
        if (p->i < p->n && (p->s[p->i] == '+' || p->s[p->i] == '-')) p->i++;
        if (p->i >= p->n || !j_isdig(p->s[p->i])) gc_fail("json: bad number at %s", path);
        while (p->i < p->n && j_isdig(p->s[p->i])) p->i++;
    }
    char *raw = strndup(p->s + st, p->i - st);
    j_add(path, norm, isint ? 'i' : 'n', raw);
}

/* ---------- schema omega.gen.protocol.v1 ---------- */

typedef struct { const char *norm, *types; } Key;

static const Key schema[] = {
    {"", "o"},
    {"schema", "s"}, {"revision", "i"}, {"issue", "s"}, {"doc", "s"}, {"question", "s"},
    {"config", "o"}, {"config.name", "s"}, {"config.params", "s"}, {"config.masking", "b"},
    {"config.harness_kind", "s"}, {"config.writer", "s"}, {"config.reader", "s"},
    {"conditions", "a"}, {"conditions[]", "o"}, {"conditions[].name", "s"},
    {"conditions[].prefix_examples", "i"}, {"conditions[].retrieval", "s"},
    {"retrieval", "o"}, {"retrieval.contract", "s"}, {"retrieval.record_schema", "s"}, {"retrieval.k", "i"},
    {"retrieval.cap_bytes", "i"}, {"retrieval.mode", "s"}, {"retrieval.query", "s"},
    {"retrieval.c_pool_excludes_prefix", "b"}, {"retrieval.length_match_abs_bytes", "i"},
    {"retrieval.length_match_frac", "n"}, {"retrieval.length_match_min_matched_frac", "n"},
    {"generation", "o"}, {"generation.primary", "s"}, {"generation.max_tokens", "i"},
    {"generation.max_rounds", "i"}, {"generation.attempts", "i"}, {"generation.attempt1_temperature", "i"},
    {"generation.sampled_temperature", "n"}, {"generation.sampled_top_p", "n"}, {"generation.seed_base", "i"},
    {"generation.seed_attempt_stride", "i"}, {"generation.retries", "i"}, {"generation.retry_pause_s", "i"},
    {"generation.order", "s"}, {"generation.sets", "a"}, {"generation.sets[]", "s"},
    {"gates", "o"}, {"gates.greedy", "o"}, {"gates.greedy.tasks", "s"}, {"gates.greedy.condition", "s"},
    {"gates.greedy.repeats", "i"}, {"gates.g0", "o"}, {"gates.g0.script", "s"}, {"gates.g0.task", "s"},
    {"gates.g0.condition", "s"}, {"gates.g0.seeds", "i"}, {"gates.g0.repeat", "i"}, {"gates.g0.otherwise", "s"},
    {"gates.g0.seed_values", "a"}, {"gates.g0.seed_values[]", "i"}, {"generation.in_flight", "i"},
    {"decision", "o"}, {"decision.schema", "s"}, {"decision.primary", "s"}, {"decision.controls", "a"},
    {"decision.controls[]", "s"}, {"decision.delta", "n"}, {"decision.alpha", "n"}, {"decision.test", "s"},
    {"decision.ci", "s"}, {"decision.z95", "n"}, {"decision.z90", "n"}, {"decision.fail_control", "s"},
    {"decision.sanity_ab_max", "i"}, {"decision.verdicts", "a"}, {"decision.verdicts[]", "s"},
    {"decision.vectors", "s"}, {"decision.floor_max", "i"}, {"decision.ceiling_min", "i"},
    {"splits", "o"},
    {"splits.dev", "o"}, {"splits.dev.count", "i"}, {"splits.dev.meta", "s"}, {"splits.dev.meta_sha256", "s"},
    {"splits.dev.cases", "s"}, {"splits.dev.pack_sha256", "s"}, {"splits.dev.tasks", "a"},
    {"splits.dev.tasks[]", "s"}, {"splits.dev.families", "a"}, {"splits.dev.families[]", "s"},
    {"splits.corpus", "o"}, {"splits.corpus.count_target", "i"}, {"splits.corpus.meta", "s"},
    {"splits.corpus.meta_sha256", "sz"},
    {"splits.heldout", "o"}, {"splits.heldout.count", "i"}, {"splits.heldout.min_families", "i"},
    {"splits.heldout.pub_sha256", "sz"}, {"splits.heldout.tasks_sha256", "sz"},
    {"splits.heldout.near_dup_threshold", "n"},
    {"record_sets", "o"}, {"record_sets.prefix_ids", "az"}, {"record_sets.prefix_ids[]", "s"},
    {"record_sets.prefix_sha256", "sz"}, {"record_sets.raw_manifest_sha256", "sz"},
    {"record_sets.abstraction_manifest_sha256", "sz"}, {"record_sets.raw_manifest", "sz"},
    {"record_sets.abstraction_manifest", "sz"},
    {"pins", "a"}, {"pins[]", "o"}, {"pins[].path", "s"}, {"pins[].sha256", "s"},
    {"interfaces", "a"}, {"interfaces[]", "o"}, {"interfaces[].name", "s"}, {"interfaces[].path", "s"},
    {"interfaces[].commit", "s"}, {"interfaces[].sha256", "s"},
};
#define NSCHEMA ((int)(sizeof schema / sizeof schema[0]))

/* frozen v1 values: any other value is a change that needs a new revision (protocol section 15) */
typedef struct { const char *path, *raw; } Fixed;
static const Fixed fixed[] = {
    {"schema", "omega.gen.protocol.v1"}, {"revision", "1"}, {"issue", "aien-spark/omega#476"},
    {"doc", "docs/one_layer/GEN_PROTOCOL_V1.md"},
    {"config.name", "A"}, {"config.params", "tools/exp445/PARAMS"}, {"config.masking", "false"},
    {"config.harness_kind", "oscir"}, {"config.writer", "omegese"}, {"config.reader", "tools/exp445/reader.sh"},
    {"conditions[0].name", "A"}, {"conditions[0].prefix_examples", "1"}, {"conditions[0].retrieval", "none"},
    {"conditions[1].name", "B"}, {"conditions[1].prefix_examples", "14"}, {"conditions[1].retrieval", "none"},
    {"conditions[2].name", "C"}, {"conditions[2].prefix_examples", "14"}, {"conditions[2].retrieval", "raw"},
    {"conditions[3].name", "D"}, {"conditions[3].prefix_examples", "14"}, {"conditions[3].retrieval", "abstraction"},
    {"retrieval.contract", "genret-cli-v3"}, {"retrieval.record_schema", "gen-record-1"}, {"retrieval.k", "3"},
    {"retrieval.cap_bytes", "4096"}, {"retrieval.mode", "normal"}, {"retrieval.query", "requirement+prompt"},
    {"retrieval.c_pool_excludes_prefix", "true"}, {"retrieval.length_match_abs_bytes", "256"},
    {"retrieval.length_match_frac", "0.15"}, {"retrieval.length_match_min_matched_frac", "0.5"}, {"generation.in_flight", "1"},
    {"generation.primary", "attempt1_round1_functional"}, {"generation.max_tokens", "3072"},
    {"generation.max_rounds", "2"}, {"generation.attempts", "3"}, {"generation.attempt1_temperature", "0"},
    {"generation.sampled_temperature", "0.7"}, {"generation.sampled_top_p", "0.8"},
    {"generation.seed_base", "476000"}, {"generation.seed_attempt_stride", "1000"}, {"generation.retries", "2"},
    {"generation.retry_pause_s", "5"}, {"generation.order", "attempt,task,condition"},
    {"generation.sets[0]", "<c>-<attempt>"}, {"generation.sets[1]", "<c>-<attempt>-final"},
    {"gates.greedy.tasks", "dev"}, {"gates.greedy.condition", "B"}, {"gates.greedy.repeats", "2"},
    {"gates.g0.script", "tools/exp445/sampling-gate.sh"}, {"gates.g0.task", "devgen01"}, {"gates.g0.condition", "B"},
    {"gates.g0.seeds", "8"}, {"gates.g0.repeat", "1"}, {"gates.g0.otherwise", "UNAVAILABLE"},
    {"gates.g0.seed_values[0]", "476901"}, {"gates.g0.seed_values[1]", "476902"}, {"gates.g0.seed_values[2]", "476903"},
    {"gates.g0.seed_values[3]", "476904"}, {"gates.g0.seed_values[4]", "476905"}, {"gates.g0.seed_values[5]", "476906"},
    {"gates.g0.seed_values[6]", "476907"}, {"gates.g0.seed_values[7]", "476908"},
    {"decision.schema", "omega.gen.decision.v1"}, {"decision.primary", "attempt1_round1_functional"},
    {"decision.controls[0]", "A"}, {"decision.controls[1]", "B"}, {"decision.controls[2]", "C"},
    {"decision.delta", "0.10"}, {"decision.alpha", "0.05"}, {"decision.test", "mcnemar_exact_one_sided"},
    {"decision.ci", "newcombe10"}, {"decision.z95", "1.959963984540054"}, {"decision.z90", "1.644853626951472"},
    {"decision.fail_control", "C"}, {"decision.sanity_ab_max", "2"},
    {"decision.verdicts[0]", "INVALID"}, {"decision.verdicts[1]", "PASS"}, {"decision.verdicts[2]", "FAIL"},
    {"decision.verdicts[3]", "INCONCLUSIVE"},
    {"decision.vectors", "tests/one_layer/generalization/stats_vectors.tsv"}, {"decision.floor_max", "3"},
    {"decision.ceiling_min", "57"},
    {"splits.dev.count", "10"}, {"splits.dev.meta", "tests/one_layer/generalization/dev/META"},
    {"splits.dev.cases", "tests/one_layer/generalization/dev/tasks/cases"},
    {"splits.corpus.count_target", "48"}, {"splits.corpus.meta", "tests/one_layer/generalization/corpus/META"},
    {"splits.heldout.count", "60"}, {"splits.heldout.min_families", "6"}, {"splits.heldout.near_dup_threshold", "0.5"},
};
#define NFIXED ((int)(sizeof fixed / sizeof fixed[0]))

/* exact array lengths */
static const struct { const char *path; int len; } fixed_len[] = {
    {"conditions", 4}, {"generation.sets", 2}, {"decision.controls", 3}, {"decision.verdicts", 4},
    {"gates.g0.seed_values", 8},
};

/* files that must be pinned (protocol section 13); more may be pinned */
static const char *const must_pin[] = {
    "docs/one_layer/GEN_PROTOCOL_V1.md",
    "docs/one_layer/OMEGESE.md",
    "tests/one_layer/acceptance/HARNESS_INTERFACE_V2.md",
    "tests/one_layer/generalization/dev/META",
    "tests/one_layer/generalization/dev/gen_expected.c",
    "tests/one_layer/generalization/dev/pack_dev.sh",
    "tests/one_layer/generalization/families/FAMSIG_V1.md",
    "tests/one_layer/generalization/stats_vectors.tsv",
    "tools/exp445/PARAMS",
    "tools/exp445/arms/common.md",
    "tools/exp445/arms/omegese.md",
    "tools/exp445/reader.sh",
    "tools/exp445/sampling-gate.sh",
    "tools/exp445/weights.A.sha256",
    "tools/gen/protocol/famsig.c",
    "tools/gen/protocol/famsig_lib.h",
    "tools/gen/protocol/gencheck.c",
    "tools/gen/protocol/genjac.c",
    "tools/gen/protocol/genstats.c",
};

static Ent *get(const char *path)
{
    for (int i = 0; i < nent; i++)
        if (!strcmp(ent[i].path, path)) return &ent[i];
    return NULL;
}

static const char *str(const char *path)
{
    Ent *e = get(path);
    if (!e || e->type != 's') gc_fail("%s: missing or not a string", path);
    return e->raw;
}

static int count_elems(const char *path)
{
    char pre[GC_PATH + 2];
    snprintf(pre, sizeof pre, "%s[", path);
    size_t pl = strlen(pre);
    int n = 0;
    for (int i = 0; i < nent; i++)
        if (!strncmp(ent[i].path, pre, pl) && strchr(ent[i].path + pl, ']') && !strchr(strchr(ent[i].path + pl, ']') + 1, '.') &&
            !strchr(strchr(ent[i].path + pl, ']') + 1, '['))
            n++;
    return n;
}

static int hex64(const char *s) { return fs_hex64_ok(s); }

static int safe_rel(const char *p)
{
    size_t n = strlen(p);
    if (n < 1 || n > 160 || p[0] == '/') return 0;
    const char *c = p;
    while (*c) {
        const char *e = strchr(c, '/');
        size_t l = e ? (size_t)(e - c) : strlen(c);
        if (l == 0 || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')) return 0;
        for (size_t i = 0; i < l; i++) {
            char ch = c[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || j_isdig(ch) || ch == '.' || ch == '_' || ch == '-')) return 0;
        }
        c += l;
        if (*c == '/') c++;
    }
    return 1;
}

static const char *root;
static int strict;   /* run-ready and open modes: nothing may be pending */

static int file_sha(const char *rel, char out[65])
{
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    struct stat st;
    if (stat(full, &st) || !S_ISREG(st.st_mode)) return -1;
    size_t len;
    char *b = fs_slurp(full, &len);
    if (!b) return -1;
    unsigned char d[32];
    sha256_hash((const unsigned char *)b, len, d);
    fs_hex(d, out);
    free(b);
    return 0;
}

static int cmp_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void read_meta(const char *rel, FsSet *set, int want_split)
{
    char full[4096], err[512];
    snprintf(full, sizeof full, "%s/%s", root, rel);
    if (fs_read(full, set, err, sizeof err)) gc_fail("%s", err);
    if (set->is_split) gc_fail("%s: a gen-split file where gen-meta was expected", rel);
    for (int i = 0; i < set->n; i++)
        if (set->m[i].split != want_split) gc_fail("%s: task %s has split %s, expected %s", rel, set->m[i].id, fs_split[set->m[i].split], fs_split[want_split]);
}

static int split_index(const char *s)
{
    for (int i = 0; i < FS_NSPLIT; i++)
        if (!strcmp(fs_split[i], s)) return i;
    return -1;
}

/* ---------- manifest ---------- */

static FsSet dev, corpus;
static int corpus_frozen;

static void check_manifest(const char *json_path)
{
    size_t len;
    char *b = fs_slurp(json_path, &len);
    if (!b) gc_fail("cannot read %s", json_path);
    if (strlen(b) != len) gc_fail("json: NUL byte");
    for (size_t i = 0; i < len; i++)
        if (b[i] != '\n' && b[i] != '\t' && ((unsigned char)b[i] < 0x20 || (unsigned char)b[i] > 0x7e)) gc_fail("json: byte 0x%02x at %zu (ASCII only)", (unsigned char)b[i], i);
    Jp p = {b, 0, len};
    j_ws(&p);
    if (p.i >= p.n || b[p.i] != '{') gc_fail("json: top level must be an object");
    j_value(&p, "", "", 0);
    j_ws(&p);
    if (p.i != p.n) gc_fail("json: trailing bytes after the top-level object");

    /* every value is a known key of the right type */
    for (int i = 0; i < nent; i++) {
        int k;
        for (k = 0; k < NSCHEMA; k++)
            if (!strcmp(schema[k].norm, ent[i].norm)) break;
        if (k == NSCHEMA) gc_fail("unknown key %s", ent[i].path);
        if (!strchr(schema[k].types, ent[i].type)) gc_fail("%s: type %c, expected %s", ent[i].path, ent[i].type, schema[k].types);
    }
    /* every object has every key its schema lists */
    for (int i = 0; i < nent; i++) {
        if (ent[i].type != 'o') continue;
        size_t nl = strlen(ent[i].norm);
        for (int k = 0; k < NSCHEMA; k++) {
            const char *kn = schema[k].norm;
            const char *rest;
            if (nl == 0) rest = kn;
            else if (!strncmp(kn, ent[i].norm, nl) && kn[nl] == '.') rest = kn + nl + 1;
            else continue;
            if (!*rest || strchr(rest, '.') || strchr(rest, '[')) continue;
            char want[GC_PATH * 2];
            if (snprintf(want, sizeof want, "%s%s%.*s", ent[i].path, nl ? "." : "", GC_PATH, rest) >= (int)sizeof want) gc_fail("json: path too long");
            if (!get(want)) gc_fail("missing key %s", want);
        }
    }
    printf("ok schema omega.gen.protocol.v1: %d values, no unknown, missing or duplicate key\n", nent);

    for (int i = 0; i < NFIXED; i++) {
        Ent *e = get(fixed[i].path);
        if (!e) gc_fail("missing frozen value %s", fixed[i].path);
        if (!e->raw || strcmp(e->raw, fixed[i].raw)) gc_fail("%s changed: \"%s\", frozen v1 value \"%s\" (a change is a new revision)", fixed[i].path, e->raw ? e->raw : "(container)", fixed[i].raw);
    }
    for (size_t i = 0; i < sizeof fixed_len / sizeof fixed_len[0]; i++)
        if (count_elems(fixed_len[i].path) != fixed_len[i].len) gc_fail("%s: %d elements, frozen %d", fixed_len[i].path, count_elems(fixed_len[i].path), fixed_len[i].len);
    printf("ok %d frozen v1 values\n", NFIXED);

    /* pins: ascending unique safe paths, each file present with that sha256 */
    int np = count_elems("pins");
    char prev[GC_PATH] = "";
    for (int i = 0; i < np; i++) {
        char kp[64], ks[64], sha[65];
        snprintf(kp, sizeof kp, "pins[%d].path", i);
        snprintf(ks, sizeof ks, "pins[%d].sha256", i);
        const char *path = str(kp), *want = str(ks);
        if (!safe_rel(path)) gc_fail("%s: unsafe path \"%s\"", kp, path);
        if (i && strcmp(prev, path) >= 0) gc_fail("pins: \"%s\" not strictly ascending (duplicate or out of order)", path);
        snprintf(prev, sizeof prev, "%s", path);
        if (!hex64(want)) gc_fail("%s: not 64 lowercase hex", ks);
        if (file_sha(path, sha)) gc_fail("pinned file missing: %s", path);
        if (strcmp(sha, want)) gc_fail("digest drift: %s is %s, pinned %s", path, sha, want);
    }
    for (size_t k = 0; k < sizeof must_pin / sizeof must_pin[0]; k++) {
        int found = 0;
        for (int i = 0; i < np && !found; i++) {
            char kp[64];
            snprintf(kp, sizeof kp, "pins[%d].path", i);
            found = !strcmp(str(kp), must_pin[k]);
        }
        if (!found) gc_fail("pins: required file %s is not pinned", must_pin[k]);
    }
    printf("ok %d pinned files re-hashed, no drift\n", np);

    /* interfaces of other GEN parts: verified when the file is present in this tree */
    int ni = count_elems("interfaces");
    for (int i = 0; i < ni; i++) {
        char k[64], sha[65];
        snprintf(k, sizeof k, "interfaces[%d].name", i);
        const char *name = str(k);
        for (int j = 0; j < i; j++) {
            char k2[64];
            snprintf(k2, sizeof k2, "interfaces[%d].name", j);
            if (!strcmp(str(k2), name)) gc_fail("interfaces: duplicate name %s", name);
        }
        snprintf(k, sizeof k, "interfaces[%d].path", i);
        const char *path = str(k);
        if (!safe_rel(path)) gc_fail("%s: unsafe path", k);
        snprintf(k, sizeof k, "interfaces[%d].commit", i);
        const char *commit = str(k);
        if (strlen(commit) != 40 || strspn(commit, "0123456789abcdef") != 40) gc_fail("%s: not a 40-hex commit", k);
        snprintf(k, sizeof k, "interfaces[%d].sha256", i);
        const char *want = str(k);
        if (!hex64(want)) gc_fail("%s: not 64 lowercase hex", k);
        if (file_sha(path, sha)) {
            if (strict) gc_fail("interface %s: %s is not in this tree (every interface must be present at seal-open)", name, path);
            printf("ok interface %s: %s not in this tree (pending merge), pinned %.16s...\n", name, path, want);
            continue;
        }
        if (strcmp(sha, want)) gc_fail("digest drift: interface %s file %s is %s, pinned %s", name, path, sha, want);
        printf("ok interface %s: %s matches\n", name, path);
    }

    /* dev split */
    const char *devmeta = str("splits.dev.meta");
    char sha[65];
    if (file_sha(devmeta, sha)) gc_fail("dev META missing: %s", devmeta);
    const char *dms = str("splits.dev.meta_sha256");
    if (!hex64(dms)) gc_fail("splits.dev.meta_sha256: not 64 lowercase hex");
    if (strcmp(sha, dms)) gc_fail("digest drift: %s is %s, splits.dev.meta_sha256 %s", devmeta, sha, dms);
    if (!hex64(str("splits.dev.pack_sha256"))) gc_fail("splits.dev.pack_sha256: not 64 lowercase hex");
    read_meta(devmeta, &dev, split_index("dev"));
    int nt = count_elems("splits.dev.tasks");
    if (nt != 10) gc_fail("splits.dev.tasks: %d tasks, frozen 10", nt);
    for (int i = 0; i < nt; i++) {
        char k[64];
        snprintf(k, sizeof k, "splits.dev.tasks[%d]", i);
        const char *id = str(k);
        if (!fs_task_id_ok(id)) gc_fail("%s: bad task id", k);
        if (i) {
            char k2[64];
            snprintf(k2, sizeof k2, "splits.dev.tasks[%d]", i - 1);
            if (strcmp(str(k2), id) >= 0) gc_fail("splits.dev.tasks: duplicate or unsorted task id %s", id);
        }
    }
    /* META ids, sorted */
    if (dev.n != nt) gc_fail("dev META has %d tasks, splits.dev.tasks %d", dev.n, nt);
    char **mids = malloc(sizeof(char *) * (size_t)dev.n);
    for (int i = 0; i < dev.n; i++) mids[i] = dev.m[i].id;
    qsort(mids, (size_t)dev.n, sizeof *mids, cmp_cstr);
    for (int i = 0; i < nt; i++) {
        char k[64];
        snprintf(k, sizeof k, "splits.dev.tasks[%d]", i);
        if (strcmp(mids[i], str(k))) gc_fail("dev META task ids differ from splits.dev.tasks (%s vs %s)", mids[i], str(k));
        if (i && !strcmp(mids[i], mids[i - 1])) gc_fail("dev META: duplicate task id %s", mids[i]);
    }
    free(mids);
    /* case directories */
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", root, str("splits.dev.cases"));
    DIR *d = opendir(full);
    if (!d) gc_fail("cannot open %s", str("splits.dev.cases"));
    char *cases[256];
    int nc = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (nc == 256) gc_fail("too many case directories");
        cases[nc++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(cases, (size_t)nc, sizeof *cases, cmp_cstr);
    if (nc != nt) gc_fail("dev cases: %d directories, splits.dev.tasks %d", nc, nt);
    for (int i = 0; i < nc; i++) {
        char k[64];
        snprintf(k, sizeof k, "splits.dev.tasks[%d]", i);
        if (strcmp(cases[i], str(k))) gc_fail("dev case directory %s does not match splits.dev.tasks (%s)", cases[i], str(k));
        free(cases[i]);
    }
    /* families = distinct META signatures, ascending */
    char *sigs[64];
    int ns = 0;
    for (int i = 0; i < dev.n; i++) {
        int seen = 0;
        for (int j = 0; j < ns; j++) seen |= !strcmp(sigs[j], dev.m[i].sig);
        if (!seen) { if (ns == 64) gc_fail("too many dev families"); sigs[ns++] = dev.m[i].sig; }
    }
    qsort(sigs, (size_t)ns, sizeof *sigs, cmp_cstr);
    int nf = count_elems("splits.dev.families");
    if (nf != ns) gc_fail("splits.dev.families: %d listed, dev META has %d families", nf, ns);
    for (int i = 0; i < nf; i++) {
        char k[64];
        snprintf(k, sizeof k, "splits.dev.families[%d]", i);
        if (strcmp(str(k), sigs[i])) gc_fail("splits.dev.families[%d] is %s, dev META gives %s", i, str(k), sigs[i]);
    }
    printf("ok dev split: %d tasks = META ids = case directories, %d families = META signatures\n", nt, nf);
    for (int i = 0; i < nf; i++) { char tok[24]; fs_token(sigs[i], tok); printf("ok dev family %s %s\n", tok, sigs[i]); }

    /* corpus split: frozen once meta_sha256 is set */
    Ent *cm = get("splits.corpus.meta_sha256");
    if (cm->type == 'z') {
        corpus_frozen = 0;
        const char *pre[] = {"record_sets.prefix_ids", "record_sets.prefix_sha256", "record_sets.raw_manifest_sha256",
                             "record_sets.abstraction_manifest_sha256", "record_sets.raw_manifest", "record_sets.abstraction_manifest"};
        for (int i = 0; i < 6; i++)
            if (get(pre[i])->type != 'z') gc_fail("%s is set while the corpus META is not frozen", pre[i]);
        FsSet both = {0};
        char err[512];
        snprintf(full, sizeof full, "%s/%s", root, devmeta);
        if (fs_read(full, &both, err, sizeof err)) gc_fail("%s", err);
        if (fs_rule(&both, stdout, 0)) gc_fail("disjointness rule fails on the dev split");
        printf("ok corpus split: pending (splits.corpus.meta_sha256 null until GEN-3 freezes it)\n");
    } else {
        corpus_frozen = 1;
        const char *cmeta = str("splits.corpus.meta");
        if (!hex64(cm->raw)) gc_fail("splits.corpus.meta_sha256: not 64 lowercase hex");
        if (file_sha(cmeta, sha)) gc_fail("corpus META missing: %s", cmeta);
        if (strcmp(sha, cm->raw)) gc_fail("digest drift: %s is %s, splits.corpus.meta_sha256 %s", cmeta, sha, cm->raw);
        read_meta(cmeta, &corpus, split_index("corpus"));
        FsSet both = {0};
        char err[512];
        snprintf(full, sizeof full, "%s/%s", root, devmeta);
        if (fs_read(full, &both, err, sizeof err)) gc_fail("%s", err);
        snprintf(full, sizeof full, "%s/%s", root, cmeta);
        if (fs_read(full, &both, err, sizeof err)) gc_fail("%s", err);
        if (fs_rule(&both, stdout, 0)) gc_fail("disjointness rule (R1..R7) fails on dev + corpus");
        printf("ok corpus split: %d tasks, dev + corpus pass R1..R7\n", corpus.n);
        Ent *pi = get("record_sets.prefix_ids");
        if (pi->type == 'a') {
            int n = count_elems("record_sets.prefix_ids");
            if (n != 14) gc_fail("record_sets.prefix_ids: %d ids, frozen 14", n);
            for (int i = 0; i < n; i++) {
                char k[64];
                snprintf(k, sizeof k, "record_sets.prefix_ids[%d]", i);
                const char *id = str(k);
                int in = 0;
                for (int j = 0; j < corpus.n; j++) in |= !strcmp(corpus.m[j].id, id);
                if (!in) gc_fail("%s: %s is not a corpus task", k, id);
                for (int j = 0; j < i; j++) {
                    char k2[64];
                    snprintf(k2, sizeof k2, "record_sets.prefix_ids[%d]", j);
                    if (!strcmp(str(k2), id)) gc_fail("record_sets.prefix_ids: duplicate %s", id);
                }
            }
        }
    }
    const char *opt[] = {"splits.heldout.pub_sha256", "splits.heldout.tasks_sha256", "record_sets.prefix_sha256",
                         "record_sets.raw_manifest_sha256", "record_sets.abstraction_manifest_sha256"};
    for (int i = 0; i < 5; i++) {
        Ent *e = get(opt[i]);
        if (e->type == 's' && !hex64(e->raw)) gc_fail("%s: not 64 lowercase hex", opt[i]);
    }
    /* C and D record-set manifests: path and digest both null (not frozen) or both set and re-hashed */
    static const char *const rs[2][2] = {{"record_sets.raw_manifest", "record_sets.raw_manifest_sha256"},
                                         {"record_sets.abstraction_manifest", "record_sets.abstraction_manifest_sha256"}};
    for (int i = 0; i < 2; i++) {
        Ent *p = get(rs[i][0]), *h = get(rs[i][1]);
        if ((p->type == 'z') != (h->type == 'z')) gc_fail("%s and %s must both be null or both be set", rs[i][0], rs[i][1]);
        if (p->type == 'z') continue;
        if (!safe_rel(p->raw)) gc_fail("%s: unsafe path", rs[i][0]);
        if (file_sha(p->raw, sha)) gc_fail("record-set manifest missing: %s", p->raw);
        if (strcmp(sha, h->raw)) gc_fail("digest drift: record-set manifest %s is %s, %s %s", p->raw, sha, rs[i][1], h->raw);
        printf("ok record set %s re-hashed\n", p->raw);
    }
}

/* ---------- split (reviewer, after the seal) ---------- */

static void check_split(const char *split_path, const char *pub_path)
{
    if (!corpus_frozen) gc_fail("split check needs the frozen corpus META (splits.corpus.meta_sha256 is null)");
    if (get("splits.heldout.pub_sha256")->type != 's' || get("splits.heldout.tasks_sha256")->type != 's')
        gc_fail("split check needs splits.heldout.pub_sha256 and tasks_sha256 anchored in protocol.json (fail closed on null)");
    FsSet hs = {0};
    char err[512];
    if (fs_read(split_path, &hs, err, sizeof err)) gc_fail("%s", err);
    if (!hs.is_split || hs.n == 0) gc_fail("%s: not a gen-split file with task lines", split_path);
    int nfam;
    char *fl = fs_famlist(&hs, &nfam);
    unsigned char dg[32];
    char famsha[65];
    sha256_hash((const unsigned char *)fl, strlen(fl), dg);
    fs_hex(dg, famsha);
    free(fl);

    size_t len;
    char *b = fs_slurp(pub_path, &len);
    if (!b) gc_fail("cannot read %s", pub_path);
    if (strlen(b) != len || len == 0 || b[len - 1] != '\n') gc_fail("HELDOUT.pub: must be text ending in a newline");
    for (size_t i = 0; i < len; i++)
        if (b[i] != '\n' && ((unsigned char)b[i] < 0x20 || (unsigned char)b[i] > 0x7e)) gc_fail("HELDOUT.pub: byte 0x%02x", (unsigned char)b[i]);
    unsigned char pd[32];
    char pubsha[65];
    sha256_hash((const unsigned char *)b, len, pd);
    fs_hex(pd, pubsha);
    char *lines[4096];
    int nl = 0;
    for (char *s = b; *s;) {
        char *e = strchr(s, '\n');
        *e = 0;
        if (nl == 4096) gc_fail("HELDOUT.pub: too many lines");
        lines[nl++] = s;
        s = e + 1;
    }
    static const char *const head[] = {"tasks_sha256 ", "split_sha256 ", "famlist_sha256 "};
    if (nl < 6 || strcmp(lines[0], "gen-heldout-pub 1")) gc_fail("HELDOUT.pub: line 1 must be \"gen-heldout-pub 1\"");
    for (int i = 0; i < 3; i++)
        if (strncmp(lines[1 + i], head[i], strlen(head[i])) || !hex64(lines[1 + i] + strlen(head[i])))
            gc_fail("HELDOUT.pub: line %d must be \"%s<64 hex>\"", 2 + i, head[i]);
    const char *p_tasks = lines[1] + 13, *p_split = lines[2] + 13, *p_fam = lines[3] + 15;
    int n_tasks, n_fams;
    char tail;
    if (sscanf(lines[4], "tasks %d%c", &n_tasks, &tail) != 1 || n_tasks < 1) gc_fail("HELDOUT.pub: line 5 must be \"tasks <n>\"");
    if (sscanf(lines[5], "families %d%c", &n_fams, &tail) != 1 || n_fams < 1) gc_fail("HELDOUT.pub: line 6 must be \"families <m>\"");
    char chk[64];
    snprintf(chk, sizeof chk, "tasks %d", n_tasks);
    if (strcmp(chk, lines[4])) gc_fail("HELDOUT.pub: line 5 not canonical");
    snprintf(chk, sizeof chk, "families %d", n_fams);
    if (strcmp(chk, lines[5])) gc_fail("HELDOUT.pub: line 6 not canonical");
    if (nl != 6 + n_tasks) gc_fail("HELDOUT.pub: %d task lines, tasks %d", nl - 6, n_tasks);
    for (int i = 0; i < n_tasks; i++) {
        const char *l = lines[6 + i];
        if (strncmp(l, "task ", 5) || !fs_task_id_ok(l + 5)) gc_fail("HELDOUT.pub: line %d must be \"task <task_id>\"", 7 + i);
        if (i && strcmp(lines[5 + i] + 5, l + 5) >= 0) gc_fail("HELDOUT.pub: task ids not strictly ascending at %s", l + 5);
    }

    if (strcmp(p_split, hs.split_sha256)) gc_fail("split_sha256 mismatch: SPLIT is %s, HELDOUT.pub says %s", hs.split_sha256, p_split);
    if (strcmp(p_fam, famsha)) gc_fail("famlist_sha256 mismatch: SPLIT gives %s, HELDOUT.pub says %s", famsha, p_fam);
    if (n_tasks != hs.n) gc_fail("HELDOUT.pub tasks %d, SPLIT has %d", n_tasks, hs.n);
    if (n_fams != nfam) gc_fail("HELDOUT.pub families %d, SPLIT has %d", n_fams, nfam);
    for (int i = 0; i < hs.n; i++)
        if (strcmp(hs.m[i].id, lines[6 + i] + 5)) gc_fail("HELDOUT.pub task %s differs from SPLIT task %s", lines[6 + i] + 5, hs.m[i].id);
    int want_n = atoi(get("splits.heldout.count")->raw), want_f = atoi(get("splits.heldout.min_families")->raw);
    if (hs.n != want_n) gc_fail("held-out split has %d tasks, protocol requires %d", hs.n, want_n);
    if (nfam < want_f) gc_fail("held-out split has %d families, protocol requires at least %d", nfam, want_f);
    Ent *e = get("splits.heldout.pub_sha256");
    if (strcmp(e->raw, pubsha)) gc_fail("digest drift: HELDOUT.pub is %s, splits.heldout.pub_sha256 %s", pubsha, e->raw);
    e = get("splits.heldout.tasks_sha256");
    if (strcmp(e->raw, p_tasks)) gc_fail("tasks_sha256 mismatch: HELDOUT.pub %s, protocol %s", p_tasks, e->raw);
    printf("ok HELDOUT.pub matches SPLIT: %d tasks, %d families, split and famlist digests\n", hs.n, nfam);

    FsSet all = {0};
    char full[4096];
    snprintf(full, sizeof full, "%s/%s", root, str("splits.dev.meta"));
    if (fs_read(full, &all, err, sizeof err)) gc_fail("%s", err);
    snprintf(full, sizeof full, "%s/%s", root, str("splits.corpus.meta"));
    if (fs_read(full, &all, err, sizeof err)) gc_fail("%s", err);
    if (fs_read(split_path, &all, err, sizeof err)) gc_fail("%s", err);
    if (fs_rule(&all, stdout, 0)) gc_fail("disjointness rule (R1..R7) fails on dev + corpus + SPLIT");
    printf("ok dev + corpus + SPLIT pass R1..R7\n");
}


/* ---------- open (custodian, seal-open step; reducer, before reducing) ---------- */

static void check_frozen(void)
{
    for (int i = 0; i < nent; i++)
        if (ent[i].type == 'z') gc_fail("not run-ready: %s is null", ent[i].path);
    int found = 0, ni = count_elems("interfaces");
    for (int i = 0; i < ni; i++) {
        char k[64];
        snprintf(k, sizeof k, "interfaces[%d].name", i);
        found |= !strcmp(str(k), str("retrieval.contract"));
    }
    if (!found) gc_fail("interfaces: the retrieval contract %s is not pinned", str("retrieval.contract"));

    printf("ok run-ready: no null field, every interface present and matching, %s pinned\n", str("retrieval.contract"));
}

static void check_open(const char *json_path, const char *open_path)
{
    size_t jl;
    char *jb = fs_slurp(json_path, &jl);
    if (!jb) gc_fail("cannot read %s", json_path);
    unsigned char d[32];
    char jsha[65];
    sha256_hash((const unsigned char *)jb, jl, d);
    fs_hex(d, jsha);
    free(jb);

    size_t len;
    char *b = fs_slurp(open_path, &len);
    if (!b) gc_fail("cannot read %s", open_path);
    if (strlen(b) != len || len == 0 || b[len - 1] != '\n') gc_fail("OPEN: must be text ending in a newline");
    char *ln[8];
    int nl = 0;
    for (char *s = b; *s;) {
        char *e = strchr(s, '\n');
        *e = 0;
        if (nl == 8) gc_fail("OPEN: more than 5 lines");
        ln[nl++] = s;
        s = e + 1;
    }
    if (nl != 5 || strcmp(ln[0], "gen-open 1")) gc_fail("OPEN: must be exactly 5 lines starting \"gen-open 1\"");
    static const char *const key[4] = {"omega_commit ", "protocol_sha256 ", "tasks_sha256 ", "heldout_pub_sha256 "};
    const char *val[4];
    for (int i = 0; i < 4; i++) {
        size_t kl = strlen(key[i]);
        if (strncmp(ln[1 + i], key[i], kl)) gc_fail("OPEN: line %d must start \"%s\"", 2 + i, key[i]);
        val[i] = ln[1 + i] + kl;
    }
    if (strlen(val[0]) != 40 || strspn(val[0], "0123456789abcdef") != 40) gc_fail("OPEN: omega_commit must be 40 lowercase hex");
    for (int i = 1; i < 4; i++)
        if (!hex64(val[i])) gc_fail("OPEN: %smust be 64 lowercase hex", key[i]);
    if (strcmp(val[1], jsha)) gc_fail("OPEN: protocol_sha256 %s, protocol.json is %s", val[1], jsha);
    if (strcmp(val[2], get("splits.heldout.tasks_sha256")->raw)) gc_fail("OPEN: tasks_sha256 differs from splits.heldout.tasks_sha256");
    if (strcmp(val[3], get("splits.heldout.pub_sha256")->raw)) gc_fail("OPEN: heldout_pub_sha256 differs from splits.heldout.pub_sha256");
    printf("ok OPEN matches protocol.json %.16s...\n", jsha);
    free(b);
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "manifest")) {
        root = argv[3];
        check_manifest(argv[2]);
    } else if (argc == 4 && !strcmp(argv[1], "run-ready")) {
        root = argv[3];
        strict = 1;
        check_manifest(argv[2]);
        check_frozen();
    } else if (argc == 5 && !strcmp(argv[1], "open")) {
        root = argv[3];
        strict = 1;
        check_manifest(argv[2]);
        check_frozen();
        check_open(argv[2], argv[4]);
    } else if (argc == 6 && !strcmp(argv[1], "split")) {
        root = argv[3];
        check_manifest(argv[2]);
        check_split(argv[4], argv[5]);
    } else {
        fprintf(stderr, "usage: gencheck manifest <protocol.json> <root> | run-ready <protocol.json> <root> | open <protocol.json> <root> <OPEN> | split <protocol.json> <root> <SPLIT> <HELDOUT.pub>\n");
        return 2;
    }
    printf("GENCHECK PASS\n");
    return 0;
}
