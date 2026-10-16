/*
 * famsig_lib.h -- GEN family-signature contract v1 (aien-spark/omega#476), shared by famsig.c and
 * gencheck.c. The contract is tests/one_layer/generalization/families/FAMSIG_V1.md; this file is its
 * only implementation. Header-only (static functions) so both tools compile from one source.
 *
 * Shape grammar (no spaces, at most FS_MAXLEN bytes, depth at most FS_MAXDEPTH, at most FS_MAXNODES nodes):
 *   node      := leaf | container "(" list ")" | "seq(" node "," list ")"
 *   list      := node ("," node)*
 *   leaf      := "arith" | "array" | "nested"
 *   container := "branch" | "call" | "loop"
 * Canonical form: a "seq" directly inside a container or inside another "seq" is flattened into it;
 * every child list is sorted by byte order of the children's canonical strings (duplicates kept).
 * Signature = "gs1:" + canonical form.
 */
#ifndef GEN_FAMSIG_LIB_H
#define GEN_FAMSIG_LIB_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sha256.h"

#define FS_NPRIM 6
#define FS_SEQ 6
#define FS_MAXLEN 512
#define FS_MAXDEPTH 8
#define FS_MAXNODES 64
#define FS_MAXCOUNT 64
#define FS_MAXID 40
#define FS_SIGMAX (FS_MAXLEN + 8)

/* fixed order: the order of the six count fields of a metadata line */
static const char *const fs_prim[FS_NPRIM + 1] = {"arith", "branch", "loop", "array", "call", "nested", "seq"};

static int fs_is_container(int k) { return k == 1 || k == 2 || k == 4; }
static int fs_is_leaf(int k) { return k == 0 || k == 3 || k == 5; }

typedef struct {
    int kind;
    int nkid;
    int kid[FS_MAXNODES];
} FsNode;

typedef struct {
    FsNode node[FS_MAXNODES];
    int nnode;
    int root;
    int count[FS_NPRIM];          /* primitive occurrences (seq not counted) */
    char canon[FS_SIGMAX];        /* canonical form, no "gs1:" prefix */
    char err[160];
} FsShape;

typedef struct {
    const char *s;
    size_t i;
    FsShape *sh;
} FsParser;

static int fs_word(FsParser *p)
{
    for (int k = 0; k <= FS_NPRIM; k++) {
        size_t n = strlen(fs_prim[k]);
        if (!strncmp(p->s + p->i, fs_prim[k], n)) {
            char c = p->s[p->i + n];
            if (c == '(' || c == ',' || c == ')' || c == 0) { p->i += n; return k; }
        }
    }
    return -1;
}

static int fs_node(FsParser *p, int depth)
{
    FsShape *sh = p->sh;
    if (depth > FS_MAXDEPTH) { snprintf(sh->err, sizeof sh->err, "nesting deeper than %d", FS_MAXDEPTH); return -1; }
    size_t at = p->i;
    int k = fs_word(p);
    if (k < 0) { snprintf(sh->err, sizeof sh->err, "byte %zu: expected one of arith array nested branch call loop seq", at); return -1; }
    if (sh->nnode >= FS_MAXNODES) { snprintf(sh->err, sizeof sh->err, "more than %d nodes", FS_MAXNODES); return -1; }
    int me = sh->nnode++;
    FsNode *n = &sh->node[me];
    n->kind = k;
    n->nkid = 0;
    if (k < FS_NPRIM) sh->count[k]++;
    if (fs_is_leaf(k)) {
        if (p->s[p->i] == '(') { snprintf(sh->err, sizeof sh->err, "byte %zu: %s is a leaf and takes no children", p->i, fs_prim[k]); return -1; }
        return me;
    }
    if (p->s[p->i] != '(') { snprintf(sh->err, sizeof sh->err, "byte %zu: %s needs '(' and at least %d child", p->i, fs_prim[k], k == FS_SEQ ? 2 : 1); return -1; }
    p->i++;
    for (;;) {
        int c = fs_node(p, depth + 1);
        if (c < 0) return -1;
        sh->node[me].kid[sh->node[me].nkid++] = c;
        if (p->s[p->i] == ',') { p->i++; continue; }
        if (p->s[p->i] == ')') { p->i++; break; }
        snprintf(sh->err, sizeof sh->err, "byte %zu: expected ',' or ')'", p->i);
        return -1;
    }
    if (k == FS_SEQ && sh->node[me].nkid < 2) { snprintf(sh->err, sizeof sh->err, "byte %zu: seq needs at least 2 children", at); return -1; }
    return me;
}

/* flattened children of node v (a seq child of a container or of a seq is replaced by its own flattened children) */
static void fs_flat(const FsShape *sh, int v, int *out, int *nout)
{
    const FsNode *n = &sh->node[v];
    for (int j = 0; j < n->nkid; j++) {
        int c = n->kid[j];
        if (sh->node[c].kind == FS_SEQ) fs_flat(sh, c, out, nout);
        else out[(*nout)++] = c;
    }
}

static int fs_cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* canonical string of node v, malloc'd */
static char *fs_canon_node(const FsShape *sh, int v)
{
    const FsNode *n = &sh->node[v];
    if (fs_is_leaf(n->kind)) return strdup(fs_prim[n->kind]);
    int kids[FS_MAXNODES], nk = 0;
    fs_flat(sh, v, kids, &nk);
    char *ks[FS_MAXNODES];
    size_t len = strlen(fs_prim[n->kind]) + 2;
    for (int j = 0; j < nk; j++) { ks[j] = fs_canon_node(sh, kids[j]); len += strlen(ks[j]) + 1; }
    qsort(ks, (size_t)nk, sizeof ks[0], fs_cmpstr);
    char *r = malloc(len + 1);
    if (!r) { perror("famsig"); exit(2); }
    strcpy(r, fs_prim[n->kind]);
    strcat(r, "(");
    for (int j = 0; j < nk; j++) { if (j) strcat(r, ","); strcat(r, ks[j]); free(ks[j]); }
    strcat(r, ")");
    return r;
}

/* parse + canonicalise; 0 ok, -1 with sh->err set */
static int fs_shape(const char *s, FsShape *sh)
{
    memset(sh, 0, sizeof *sh);
    size_t len = strlen(s);
    if (len == 0 || len > FS_MAXLEN) { snprintf(sh->err, sizeof sh->err, "shape length %zu, must be 1..%d", len, FS_MAXLEN); return -1; }
    FsParser p = {s, 0, sh};
    sh->root = fs_node(&p, 1);
    if (sh->root < 0) return -1;
    if (s[p.i]) { snprintf(sh->err, sizeof sh->err, "byte %zu: extra text after the shape", p.i); return -1; }
    char *c = fs_canon_node(sh, sh->root);
    if (strlen(c) >= sizeof sh->canon) { free(c); snprintf(sh->err, sizeof sh->err, "canonical form too long"); return -1; }
    strcpy(sh->canon, c);
    free(c);
    return 0;
}

/* composition edges "parent>child" (container -> head primitive of each flattened child); unique, sorted */
static int fs_edges(const FsShape *sh, char out[][16], int max)
{
    int ne = 0;
    for (int v = 0; v < sh->nnode; v++) {
        if (!fs_is_container(sh->node[v].kind)) continue;
        int kids[FS_MAXNODES], nk = 0;
        fs_flat(sh, v, kids, &nk);
        for (int j = 0; j < nk; j++) {
            char e[16];
            snprintf(e, sizeof e, "%s>%s", fs_prim[sh->node[v].kind], fs_prim[sh->node[kids[j]].kind]);
            int dup = 0;
            for (int q = 0; q < ne; q++) if (!strcmp(out[q], e)) dup = 1;
            if (!dup && ne < max) strcpy(out[ne++], e);
        }
    }
    for (int a = 1; a < ne; a++)
        for (int b = a; b > 0 && strcmp(out[b - 1], out[b]) > 0; b--) { char t[16]; strcpy(t, out[b]); strcpy(out[b], out[b - 1]); strcpy(out[b - 1], t); }
    return ne;
}

/* ---- metadata lines ---------------------------------------------------------------------------- */

enum { FS_CORPUS, FS_DEV, FS_HELDOUT, FS_NSPLIT };
static const char *const fs_split[FS_NSPLIT] = {"corpus", "dev", "heldout"};

typedef struct {
    char id[FS_MAXID + 1];
    int split;
    int count[FS_NPRIM];
    char shape[FS_MAXLEN + 1];
    char sig[FS_SIGMAX + 4];      /* "gs1:" + canonical */
    int distinct;                 /* distinct primitives in the shape */
    char src[256];                /* file:line, for messages */
} FsMeta;

static int fs_task_id_ok(const char *s)
{
    size_t n = strlen(s);
    if (n < 1 || n > FS_MAXID) return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '-')) return 0;
    return 1;
}

static int fs_count_ok(const char *s, int *v)
{
    size_t n = strlen(s);
    if (n < 1 || n > 2 || (n == 2 && s[0] == '0')) return 0;
    for (size_t i = 0; i < n; i++) if (s[i] < '0' || s[i] > '9') return 0;
    *v = atoi(s);
    return *v <= FS_MAXCOUNT;
}

/* one metadata line: "<task_id> <split> <arith> <branch> <loop> <array> <call> <nested> <shape>",
 * fields separated by exactly one space. 0 ok, -1 with err. */
static int fs_meta_line(const char *line, FsMeta *m, char *err, size_t errn)
{
    char buf[1024];
    if (strlen(line) >= sizeof buf) { snprintf(err, errn, "line too long"); return -1; }
    strcpy(buf, line);
    char *f[10];
    int nf = 0;
    char *s = buf;
    for (;;) {
        if (nf == 10) { snprintf(err, errn, "more than 9 fields"); return -1; }
        f[nf++] = s;
        char *sp = strchr(s, ' ');
        if (!sp) break;
        *sp = 0;
        s = sp + 1;
    }
    if (nf != 9) { snprintf(err, errn, "%d fields, need 9: task split arith branch loop array call nested shape", nf); return -1; }
    for (int i = 0; i < nf; i++) if (!*f[i]) { snprintf(err, errn, "empty field %d (fields are separated by exactly one space)", i + 1); return -1; }
    memset(m, 0, sizeof *m);
    if (!fs_task_id_ok(f[0])) { snprintf(err, errn, "task id '%s' is not [a-z0-9_-]{1,40}", f[0]); return -1; }
    strcpy(m->id, f[0]);
    m->split = -1;
    for (int k = 0; k < FS_NSPLIT; k++) if (!strcmp(f[1], fs_split[k])) m->split = k;
    if (m->split < 0) { snprintf(err, errn, "split '%s' is not corpus, dev or heldout", f[1]); return -1; }
    for (int k = 0; k < FS_NPRIM; k++)
        if (!fs_count_ok(f[2 + k], &m->count[k])) { snprintf(err, errn, "%s count '%s' is not a decimal 0..%d without leading zeros", fs_prim[k], f[2 + k], FS_MAXCOUNT); return -1; }
    FsShape sh;
    if (fs_shape(f[8], &sh)) { snprintf(err, errn, "shape: %s", sh.err); return -1; }
    for (int k = 0; k < FS_NPRIM; k++)
        if (sh.count[k] != m->count[k]) { snprintf(err, errn, "declared %s=%d but the shape has %d", fs_prim[k], m->count[k], sh.count[k]); return -1; }
    strcpy(m->shape, f[8]);
    snprintf(m->sig, sizeof m->sig, "gs1:%s", sh.canon);
    for (int k = 0; k < FS_NPRIM; k++) if (m->count[k]) m->distinct++;
    return 0;
}

/* ---- files ------------------------------------------------------------------------------------- */

static char *fs_slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 4096, n = 0;
    char *b = malloc(cap + 1);
    if (!b) { fclose(fp); return NULL; }
    size_t r;
    while ((r = fread(b + n, 1, cap - n, fp)) > 0) {
        n += r;
        if (n == cap) { cap *= 2; char *nb = realloc(b, cap + 1); if (!nb) { free(b); fclose(fp); return NULL; } b = nb; }
    }
    fclose(fp);
    b[n] = 0;
    *len = n;
    return b;
}

static void fs_hex(const unsigned char *d, char out[65])
{
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", d[i]);
    out[64] = 0;
}

/* family token: "gs1-" + first 16 hex of sha256(signature), signature = "gs1:" + canonical (no newline);
 * matches [a-z0-9_-]{1,64}, the record "family" field of tools/gen/retrieval/CONTRACT.md */
static void fs_token(const char *sig, char out[24])
{
    unsigned char d[32];
    char hex[65];
    sha256_hash((const unsigned char *)sig, strlen(sig), d);
    fs_hex(d, hex);
    snprintf(out, 24, "gs1-%.16s", hex);
}

static int fs_hex64_ok(const char *s)
{
    if (strlen(s) != 64) return 0;
    for (int i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

typedef struct {
    FsMeta *m;
    int n, cap;
    int is_split;                 /* last file read was a gen-split file */
    char salt[65];
    char split_sha256[65];
} FsSet;

static void fs_push(FsSet *set, const FsMeta *m)
{
    if (set->n == set->cap) {
        set->cap = set->cap ? 2 * set->cap : 64;
        set->m = realloc(set->m, (size_t)set->cap * sizeof *set->m);
        if (!set->m) { perror("famsig"); exit(2); }
    }
    set->m[set->n++] = *m;
}

/* Reads a "gen-meta 1" file (public: corpus/dev lines; '#' comments and blank lines allowed; heldout
 * lines refused) or a "gen-split 1" file (sealed: "salt <64 hex>", then heldout lines only, ids strictly
 * ascending, no comments or blank lines). Bytes: '\n' and 0x20..0x7E only, last byte '\n'.
 * Appends to set; 0 ok, -1 with err. */
static int fs_read(const char *path, FsSet *set, char *err, size_t errn)
{
    size_t len;
    char *b = fs_slurp(path, &len);
    if (!b) { snprintf(err, errn, "%s: cannot read", path); return -1; }
    if (len == 0 || b[len - 1] != '\n') { snprintf(err, errn, "%s: empty or no final newline", path); free(b); return -1; }
    for (size_t i = 0; i < len; i++)
        if (b[i] != '\n' && (b[i] < 0x20 || b[i] > 0x7e)) { snprintf(err, errn, "%s: byte %zu is not \\n or printable ASCII", path, i); free(b); return -1; }
    int split_file = !strncmp(b, "gen-split 1\n", 12);
    if (!split_file && strncmp(b, "gen-meta 1\n", 11)) { snprintf(err, errn, "%s: first line must be 'gen-meta 1' or 'gen-split 1'", path); free(b); return -1; }
    set->is_split = split_file;
    if (split_file) {
        unsigned char d[32];
        sha256_hash((const unsigned char *)b, len, d);
        fs_hex(d, set->split_sha256);
    }
    char *line = strchr(b, '\n') + 1;
    int ln = 1, first = 1;
    char prev[FS_MAXID + 1] = "";
    int rc = 0;
    while (*line) {
        char *nl = strchr(line, '\n');
        *nl = 0;
        ln++;
        if (split_file && ln == 2) {
            if (strncmp(line, "salt ", 5) || !fs_hex64_ok(line + 5)) { snprintf(err, errn, "%s:2: must be 'salt <64 lowercase hex>'", path); rc = -1; break; }
            strcpy(set->salt, line + 5);
        } else if (!split_file && (line[0] == 0 || line[0] == '#')) {
            /* comment or blank line */
        } else {
            FsMeta m;
            char e[200];
            if (fs_meta_line(line, &m, e, sizeof e)) { snprintf(err, errn, "%s:%d: %s", path, ln, e); rc = -1; break; }
            snprintf(m.src, sizeof m.src, "%s:%d", path, ln);
            if (split_file && m.split != FS_HELDOUT) { snprintf(err, errn, "%s:%d: a gen-split file holds heldout lines only", path, ln); rc = -1; break; }
            if (!split_file && m.split == FS_HELDOUT) { snprintf(err, errn, "%s:%d: heldout metadata belongs only in the sealed gen-split file", path, ln); rc = -1; break; }
            if (split_file && !first && strcmp(prev, m.id) >= 0) { snprintf(err, errn, "%s:%d: task ids must be strictly ascending (byte order)", path, ln); rc = -1; break; }
            strcpy(prev, m.id);
            first = 0;
            fs_push(set, &m);
        }
        line = nl + 1;
    }
    if (!rc && split_file && ln < 2) { snprintf(err, errn, "%s: missing salt line", path); rc = -1; }
    free(b);
    return rc;
}

/* FAMLIST bytes of a split: "gen-famlist 1\n" "salt <hex>\n" then every distinct heldout signature,
 * sorted by byte order, one per line. Returns malloc'd string. */
static char *fs_famlist(const FsSet *set, int *nfam)
{
    char **sig = malloc((size_t)(set->n ? set->n : 1) * sizeof *sig);
    int ns = 0;
    for (int i = 0; i < set->n; i++) if (set->m[i].split == FS_HELDOUT) sig[ns++] = set->m[i].sig;
    qsort(sig, (size_t)ns, sizeof sig[0], fs_cmpstr);
    size_t cap = 64 + (size_t)ns * (FS_SIGMAX + 8);
    char *out = malloc(cap);
    if (!out || !sig) { perror("famsig"); exit(2); }
    int n = snprintf(out, cap, "gen-famlist 1\nsalt %s\n", set->salt);
    int nf = 0;
    for (int i = 0; i < ns; i++) {
        if (i && !strcmp(sig[i], sig[i - 1])) continue;
        n += snprintf(out + n, cap - (size_t)n, "%s\n", sig[i]);
        nf++;
    }
    free(sig);
    *nfam = nf;
    return out;
}

static int fs_cmpmeta_sig(const void *a, const void *b)
{
    const FsMeta *x = a, *y = b;
    int c = strcmp(x->sig, y->sig);
    if (c) return c;
    if (x->split != y->split) return x->split < y->split ? -1 : 1;
    return strcmp(x->id, y->id);
}

/* The disjointness rule (FAMSIG_V1.md section 4) over every line read. Prints a report to out
 * (heldout family signatures only when show_heldout); returns the number of rule failures. */
static int fs_rule(FsSet *set, FILE *out, int show_heldout)
{
    int fails = 0;
    FsMeta *m = set->m;
    int n = set->n;
    /* R2 unique task ids across every input */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (!strcmp(m[i].id, m[j].id)) { fprintf(out, "FAIL R2 duplicate task id %s (%s, %s)\n", m[i].id, m[i].src, m[j].src); fails++; }
    /* R3 a signature belongs to one split */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (m[i].split != m[j].split && !strcmp(m[i].sig, m[j].sig)) {
                int ho = m[i].split == FS_HELDOUT || m[j].split == FS_HELDOUT;
                fprintf(out, "FAIL R3 family shared by %s and %s: %s %s and %s\n", fs_split[m[i].split], fs_split[m[j].split],
                        ho && !show_heldout ? "(heldout signature hidden)" : m[i].sig, m[i].id, m[j].id);
                fails++;
            }
    /* R4 every heldout primitive appears in some corpus task; R5 heldout shapes use >= 2 distinct primitives */
    int corpus_has[FS_NPRIM] = {0}, ncorpus = 0;
    for (int i = 0; i < n; i++) if (m[i].split == FS_CORPUS) { ncorpus++; for (int k = 0; k < FS_NPRIM; k++) if (m[i].count[k]) corpus_has[k] = 1; }
    for (int i = 0; i < n; i++) {
        if (m[i].split != FS_HELDOUT) continue;
        for (int k = 0; k < FS_NPRIM; k++)
            if (m[i].count[k] && !corpus_has[k]) { fprintf(out, "FAIL R4 heldout task %s uses %s, which no corpus task uses%s\n", m[i].id, fs_prim[k], ncorpus ? "" : " (no corpus metadata given)"); fails++; }
        if (m[i].distinct < 2) { fprintf(out, "FAIL R5 heldout task %s uses fewer than 2 distinct primitives\n", m[i].id); fails++; }
    }
    /* corpus composition edges, for the novelty report */
    static char cedge[256][16];
    int nce = 0;
    for (int i = 0; i < n; i++) {
        if (m[i].split != FS_CORPUS) continue;
        FsShape sh;
        if (fs_shape(m[i].shape, &sh)) continue;
        char e[64][16];
        int ne = fs_edges(&sh, e, 64);
        for (int q = 0; q < ne; q++) {
            int dup = 0;
            for (int r = 0; r < nce; r++) if (!strcmp(cedge[r], e[q])) dup = 1;
            if (!dup && nce < 256) strcpy(cedge[nce++], e[q]);
        }
    }
    /* report: per split counts, then families in signature order */
    FsMeta *s = malloc((size_t)(n ? n : 1) * sizeof *s);
    if (!s) { perror("famsig"); exit(2); }
    memcpy(s, m, (size_t)n * sizeof *s);
    qsort(s, (size_t)n, sizeof *s, fs_cmpmeta_sig);
    for (int sp = 0; sp < FS_NSPLIT; sp++) {
        int t = 0, f = 0;
        for (int i = 0; i < n; i++) if (s[i].split == sp) { t++; if (i == 0 || strcmp(s[i].sig, s[i - 1].sig) || s[i - 1].split != sp) f++; }
        fprintf(out, "split %s tasks=%d families=%d\n", fs_split[sp], t, f);
    }
    for (int i = 0; i < n;) {
        int j = i;
        while (j < n && !strcmp(s[j].sig, s[i].sig) && s[j].split == s[i].split) j++;
        if (s[i].split != FS_HELDOUT || show_heldout) {
            fprintf(out, "family %s %s tasks=%d", fs_split[s[i].split], s[i].sig, j - i);
            if (s[i].split == FS_HELDOUT) {
                FsShape sh;
                int novel = 0;
                if (!fs_shape(s[i].shape, &sh)) {
                    char e[64][16];
                    int ne = fs_edges(&sh, e, 64);
                    for (int q = 0; q < ne; q++) { int seen = 0; for (int r = 0; r < nce; r++) if (!strcmp(cedge[r], e[q])) seen = 1; novel += !seen; }
                }
                fprintf(out, " novel_edges=%d", novel);
            }
            fputc('\n', out);
        }
        i = j;
    }
    free(s);
    return fails;
}

#endif
