/* st_holdout.c - G3 sealed-holdout commitment format, v1.
 * See st_holdout.h and spec/searchtrace/G3_SEALED_HOLDOUT_COMMITMENT_V1.md.
 * Depends only on src/sha256.h and libc (POSIX for the CLI file helpers). */
#include "searchtrace/st_holdout.h"
#include "sha256.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ST_REC_MAX 1024 /* canonical records are < 512 bytes */

static void set_why(char *why, size_t why_len, const char *msg)
{
    if (why && why_len) {
        size_t n = strlen(msg);
        if (n >= why_len)
            n = why_len - 1;
        memcpy(why, msg, n);
        why[n] = '\0';
    }
}

static void hex_lower(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= (uint8_t)(a[i] ^ b[i]);
    return acc == 0;
}

static int label_ok(const char *s, size_t n)
{
    if (n < 1 || n > ST_HOLDOUT_ID_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

uint32_t st_holdout_count_tasks(const uint8_t *t, size_t len)
{
    uint32_t n = 0;
    size_t i = 0;
    if (!t)
        return 0;
    while (i < len) {
        if (len - i >= 5 && memcmp(t + i, ST_HOLDOUT_TASK_PREFIX, 5) == 0 &&
            n < UINT32_MAX)
            n++;
        while (i < len && t[i] != '\n')
            i++;
        i++; /* skip LF (or step past end) */
    }
    return n;
}

static void salt_digest(const uint8_t salt[32], uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)ST_HOLDOUT_SALT_DOMAIN,
                  sizeof(ST_HOLDOUT_SALT_DOMAIN)); /* includes 0x00 */
    sha256_update(&c, salt, 32);
    sha256_final(&c, out);
}

static void commitment(const uint8_t salt[32], const uint8_t *t, size_t len,
                       uint32_t count, uint8_t out[32])
{
    uint8_t be[12];
    uint64_t l = (uint64_t)len;
    for (int i = 0; i < 8; i++)
        be[i] = (uint8_t)(l >> (56 - 8 * i));
    for (int i = 0; i < 4; i++)
        be[8 + i] = (uint8_t)(count >> (24 - 8 * i));
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)ST_HOLDOUT_DOMAIN,
                  sizeof(ST_HOLDOUT_DOMAIN)); /* includes 0x00 */
    sha256_update(&c, salt, 32);
    sha256_update(&c, be, sizeof be);
    if (len)
        sha256_update(&c, t, len);
    sha256_final(&c, out);
}

int st_holdout_commit(const char *holdout_id, const uint8_t *taskset,
                      size_t len, const uint8_t salt[32], char *rec,
                      size_t cap, size_t *rec_len)
{
    if (!holdout_id || !taskset || !salt || !rec || !rec_len)
        return ST_HOLDOUT_EARG;
    if (!label_ok(holdout_id, strnlen(holdout_id, ST_HOLDOUT_ID_MAX + 1)))
        return ST_HOLDOUT_ELABEL;
    uint32_t count = st_holdout_count_tasks(taskset, len);
    if (count == 0)
        return ST_HOLDOUT_ENOTASKS;

    uint8_t sd[32], cm[32], end[32];
    char sdh[65], cmh[65], endh[65];
    salt_digest(salt, sd);
    commitment(salt, taskset, len, count, cm);
    hex_lower(sd, 32, sdh);
    hex_lower(cm, 32, cmh);

    char body[ST_REC_MAX];
    int n = snprintf(body, sizeof body,
                     ST_HOLDOUT_MAGIC "\n"
                     "domain " ST_HOLDOUT_DOMAIN "\n"
                     "holdout_id %s\n"
                     "task_count %lu\n"
                     "taskset_len %llu\n"
                     "salt_digest %s\n"
                     "commitment %s\n",
                     holdout_id, (unsigned long)count,
                     (unsigned long long)len, sdh, cmh);
    if (n < 0 || (size_t)n >= sizeof body)
        return ST_HOLDOUT_EFORMAT;
    sha256_hash((const uint8_t *)body, (size_t)n, end);
    hex_lower(end, 32, endh);
    size_t total = (size_t)n + 4 + 64 + 1;
    if (cap < total)
        return ST_HOLDOUT_ECAP;
    memcpy(rec, body, (size_t)n);
    memcpy(rec + n, "end ", 4);
    memcpy(rec + n + 4, endh, 64);
    rec[total - 1] = '\n';
    if (cap > total)
        rec[total] = '\0';
    *rec_len = total;
    return 0;
}

/* ---- strict parser ---------------------------------------------------- */

typedef struct {
    const char *p;
    size_t len, pos;
} Cur;

/* Reads the next line "key value\n"; returns pointer/len of value. */
static int next_field(Cur *c, const char *key, const char **val, size_t *vlen,
                      char *why, size_t wl)
{
    size_t kl = strlen(key);
    if (c->pos >= c->len) {
        set_why(why, wl, "truncated record (missing field or end line)");
        return ST_HOLDOUT_EFORMAT;
    }
    const char *nl = memchr(c->p + c->pos, '\n', c->len - c->pos);
    if (!nl) {
        set_why(why, wl, "truncated record (line without LF)");
        return ST_HOLDOUT_EFORMAT;
    }
    size_t ll = (size_t)(nl - (c->p + c->pos));
    const char *l = c->p + c->pos;
    if (ll < kl + 1 || memcmp(l, key, kl) != 0 || l[kl] != ' ') {
        char m[96];
        snprintf(m, sizeof m, "expected field '%s' (missing/extra/reordered)",
                 key);
        set_why(why, wl, m);
        return ST_HOLDOUT_EFORMAT;
    }
    *val = l + kl + 1;
    *vlen = ll - kl - 1;
    c->pos += ll + 1;
    return 0;
}

static int parse_hex32(const char *v, size_t n, uint8_t out[32], char *why,
                       size_t wl)
{
    if (n != 64) {
        set_why(why, wl, "hex field is not 64 characters");
        return ST_HOLDOUT_EFORMAT;
    }
    for (size_t i = 0; i < 64; i++) {
        char ch = v[i];
        int x;
        if (ch >= '0' && ch <= '9')
            x = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            x = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') {
            set_why(why, wl, "uppercase hex refused");
            return ST_HOLDOUT_EFORMAT;
        } else {
            set_why(why, wl, "bad hex character");
            return ST_HOLDOUT_EFORMAT;
        }
        if (i & 1)
            out[i / 2] = (uint8_t)(out[i / 2] | x);
        else
            out[i / 2] = (uint8_t)(x << 4);
    }
    return 0;
}

static int parse_dec(const char *v, size_t n, uint64_t max, uint64_t *out,
                     char *why, size_t wl)
{
    uint64_t x = 0;
    if (n == 0 || n > 20 || (n > 1 && v[0] == '0')) {
        set_why(why, wl, "bad decimal (empty, too long or leading zero)");
        return ST_HOLDOUT_EFORMAT;
    }
    for (size_t i = 0; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') {
            set_why(why, wl, "bad decimal digit");
            return ST_HOLDOUT_EFORMAT;
        }
        uint64_t d = (uint64_t)(v[i] - '0');
        if (x > (max - d) / 10) {
            set_why(why, wl, "decimal out of range");
            return ST_HOLDOUT_EFORMAT;
        }
        x = x * 10 + d;
    }
    *out = x;
    return 0;
}

int st_holdout_parse(const char *rec, size_t len, StHoldoutCommitment *out,
                     char *why, size_t wl)
{
    StHoldoutCommitment c;
    const char *v;
    size_t vl;
    uint64_t num;
    int r;

    if (!rec || !out) {
        set_why(why, wl, "null argument");
        return ST_HOLDOUT_EARG;
    }
    if (len > ST_REC_MAX) {
        set_why(why, wl, "record too long");
        return ST_HOLDOUT_EFORMAT;
    }
    if (memchr(rec, '\0', len)) {
        set_why(why, wl, "NUL byte in record");
        return ST_HOLDOUT_EFORMAT;
    }
    if (memchr(rec, '\r', len)) {
        set_why(why, wl, "CR character in record (LF only)");
        return ST_HOLDOUT_EFORMAT;
    }
    memset(&c, 0, sizeof c);
    Cur cur = {rec, len, 0};

    size_t ml = strlen(ST_HOLDOUT_MAGIC);
    if (len < ml + 1 || memcmp(rec, ST_HOLDOUT_MAGIC, ml) != 0 ||
        rec[ml] != '\n') {
        set_why(why, wl, "wrong magic or version");
        return ST_HOLDOUT_EFORMAT;
    }
    cur.pos = ml + 1;

    if ((r = next_field(&cur, "domain", &v, &vl, why, wl)) != 0)
        return r;
    if (vl != strlen(ST_HOLDOUT_DOMAIN) ||
        memcmp(v, ST_HOLDOUT_DOMAIN, vl) != 0) {
        set_why(why, wl, "wrong domain");
        return ST_HOLDOUT_EFORMAT;
    }
    if ((r = next_field(&cur, "holdout_id", &v, &vl, why, wl)) != 0)
        return r;
    if (!label_ok(v, vl)) {
        set_why(why, wl, "bad holdout_id label");
        return ST_HOLDOUT_EFORMAT;
    }
    memcpy(c.holdout_id, v, vl);
    c.holdout_id[vl] = '\0';

    if ((r = next_field(&cur, "task_count", &v, &vl, why, wl)) != 0)
        return r;
    if ((r = parse_dec(v, vl, UINT32_MAX, &num, why, wl)) != 0)
        return r;
    if (num == 0) {
        set_why(why, wl, "zero tasks refused");
        return ST_HOLDOUT_EFORMAT;
    }
    c.task_count = (uint32_t)num;

    if ((r = next_field(&cur, "taskset_len", &v, &vl, why, wl)) != 0)
        return r;
    if ((r = parse_dec(v, vl, UINT64_MAX, &num, why, wl)) != 0)
        return r;
    if (num / 5 < c.task_count) {
        set_why(why, wl, "taskset_len too small for task_count");
        return ST_HOLDOUT_EFORMAT;
    }
    c.taskset_len = num;

    if ((r = next_field(&cur, "salt_digest", &v, &vl, why, wl)) != 0)
        return r;
    if ((r = parse_hex32(v, vl, c.salt_digest, why, wl)) != 0)
        return r;
    if ((r = next_field(&cur, "commitment", &v, &vl, why, wl)) != 0)
        return r;
    if ((r = parse_hex32(v, vl, c.commitment, why, wl)) != 0)
        return r;

    size_t body_len = cur.pos;
    if ((r = next_field(&cur, "end", &v, &vl, why, wl)) != 0)
        return r;
    if ((r = parse_hex32(v, vl, c.end_digest, why, wl)) != 0)
        return r;
    if (cur.pos != len) {
        set_why(why, wl, "trailing bytes after end line");
        return ST_HOLDOUT_EFORMAT;
    }
    uint8_t d[32];
    sha256_hash((const uint8_t *)rec, body_len, d);
    if (!ct_equal(d, c.end_digest, 32)) {
        set_why(why, wl, "end digest mismatch");
        return ST_HOLDOUT_EDIGEST;
    }
    sha256_hash((const uint8_t *)rec, len, c.record_digest);
    *out = c;
    return 0;
}

int st_holdout_reveal(const StHoldoutCommitment *c, const uint8_t *taskset,
                      size_t len, const uint8_t salt[32], char *why,
                      size_t wl)
{
    uint8_t d[32];
    if (!c || !taskset || !salt) {
        set_why(why, wl, "null argument");
        return ST_HOLDOUT_EARG;
    }
    salt_digest(salt, d);
    if (!ct_equal(d, c->salt_digest, 32)) {
        set_why(why, wl, "salt does not match salt_digest");
        return ST_HOLDOUT_ESALT;
    }
    if ((uint64_t)len != c->taskset_len) {
        set_why(why, wl, "taskset_len mismatch");
        return ST_HOLDOUT_ELEN;
    }
    uint32_t n = st_holdout_count_tasks(taskset, len);
    if (n != c->task_count) {
        set_why(why, wl, "task_count mismatch");
        return ST_HOLDOUT_ECOUNT;
    }
    commitment(salt, taskset, len, n, d);
    if (!ct_equal(d, c->commitment, 32)) {
        set_why(why, wl, "commitment mismatch");
        return ST_HOLDOUT_ECOMMIT;
    }
    return 0;
}

int st_holdout_reveal_receipt(const StHoldoutCommitment *c,
                              const char *repo_commit_hex40, char *out,
                              size_t cap, size_t *out_len)
{
    if (!c || !repo_commit_hex40 || !out || !out_len)
        return ST_HOLDOUT_EARG;
    if (strnlen(repo_commit_hex40, 41) != 40)
        return ST_HOLDOUT_ECOMMITID;
    for (int i = 0; i < 40; i++) {
        char ch = repo_commit_hex40[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
            return ST_HOLDOUT_ECOMMITID;
    }
    if (!label_ok(c->holdout_id, strnlen(c->holdout_id, ST_HOLDOUT_ID_MAX + 1)))
        return ST_HOLDOUT_ELABEL;
    char cmh[65], rdh[65], endh[65];
    uint8_t end[32];
    hex_lower(c->commitment, 32, cmh);
    hex_lower(c->record_digest, 32, rdh);
    char body[ST_REC_MAX];
    int n = snprintf(body, sizeof body,
                     ST_HOLDOUT_REVEAL_MAGIC "\n"
                     "domain " ST_HOLDOUT_DOMAIN "\n"
                     "holdout_id %s\n"
                     "commitment %s\n"
                     "record_digest %s\n"
                     "task_count %lu\n"
                     "taskset_len %llu\n"
                     "repo_commit %s\n"
                     "verdict PASS\n",
                     c->holdout_id, cmh, rdh, (unsigned long)c->task_count,
                     (unsigned long long)c->taskset_len, repo_commit_hex40);
    if (n < 0 || (size_t)n >= sizeof body)
        return ST_HOLDOUT_EFORMAT;
    sha256_hash((const uint8_t *)body, (size_t)n, end);
    hex_lower(end, 32, endh);
    size_t total = (size_t)n + 4 + 64 + 1;
    if (cap < total)
        return ST_HOLDOUT_ECAP;
    memcpy(out, body, (size_t)n);
    memcpy(out + n, "end ", 4);
    memcpy(out + n + 4, endh, 64);
    out[total - 1] = '\n';
    if (cap > total)
        out[total] = '\0';
    *out_len = total;
    return 0;
}

/* ---- CLI -------------------------------------------------------------- */

static int read_file(const char *path, uint8_t **buf, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t cap = 4096, n = 0;
    uint8_t *b = malloc(cap);
    if (!b) {
        fclose(f);
        return -1;
    }
    for (;;) {
        if (n == cap) {
            uint8_t *nb = realloc(b, cap * 2);
            if (!nb) {
                free(b);
                fclose(f);
                return -1;
            }
            b = nb;
            cap *= 2;
        }
        size_t got = fread(b + n, 1, cap - n, f);
        n += got;
        if (got == 0)
            break;
    }
    int err = ferror(f);
    fclose(f);
    if (err) {
        free(b);
        return -1;
    }
    *buf = b;
    *len = n;
    return 0;
}

/* Write content-addressed file. 0 written or identical file present,
 * -1 I/O error, -2 existing file differs. */
static int write_addressed(const char *path, const char *data, size_t len)
{
    uint8_t *old;
    size_t ol;
    if (read_file(path, &old, &ol) == 0) {
        int same = (ol == len && memcmp(old, data, len) == 0);
        free(old);
        return same ? 0 : -2;
    }
    if (errno != ENOENT)
        return -1;
    char tmp[4200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld", path, (long)getpid()) >=
        (int)sizeof tmp)
        return -1;
    FILE *f = fopen(tmp, "wbx");
    if (!f)
        return -1;
    int ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    ok = (fclose(f) == 0) && ok;
    if (!ok || link(tmp, path) != 0) {
        int exists = (errno == EEXIST);
        unlink(tmp);
        return exists ? -2 : -1;
    }
    unlink(tmp);
    return 0;
}

static int read_salt(const char *path, uint8_t salt[32])
{
    uint8_t *b;
    size_t n;
    if (read_file(path, &b, &n) != 0) {
        fprintf(stderr, "st_holdout: cannot read salt file %s\n", path);
        return -1;
    }
    if (n != 32) {
        fprintf(stderr, "st_holdout: salt file must be exactly 32 bytes "
                        "(got %zu)\n", n);
        free(b);
        return -1;
    }
    memcpy(salt, b, 32);
    memset(b, 0, 32);
    free(b);
    return 0;
}

static int cli_commit(char **argv)
{
    uint8_t salt[32], *ts;
    size_t tl, rl;
    char rec[ST_REC_MAX], why[128], path[4096], dh[65];
    uint8_t d[32];
    if (read_salt(argv[3], salt) != 0)
        return 2;
    if (read_file(argv[2], &ts, &tl) != 0) {
        fprintf(stderr, "st_holdout: cannot read taskset %s\n", argv[2]);
        return 2;
    }
    int r = st_holdout_commit(argv[1], ts, tl, salt, rec, sizeof rec, &rl);
    free(ts);
    memset(salt, 0, sizeof salt);
    if (r != 0) {
        fprintf(stderr, "st_holdout: commit refused (%d)\n", r);
        return 2;
    }
    StHoldoutCommitment c; /* self-check: emitted record must parse */
    if (st_holdout_parse(rec, rl, &c, why, sizeof why) != 0) {
        fprintf(stderr, "st_holdout: internal self-check failed: %s\n", why);
        return 2;
    }
    sha256_hash((const uint8_t *)rec, rl, d);
    hex_lower(d, 32, dh);
    if (snprintf(path, sizeof path, "%s/g3-commit-%s.txt", argv[4], dh) >=
        (int)sizeof path)
        return 2;
    r = write_addressed(path, rec, rl);
    if (r == -2) {
        fprintf(stderr, "st_holdout: %s exists with different bytes\n", path);
        return 2;
    }
    if (r != 0) {
        fprintf(stderr, "st_holdout: cannot write %s\n", path);
        return 2;
    }
    printf("%s\n", path);
    return 0;
}

static int cli_reveal(char **argv)
{
    const char *rc = getenv("OMEGA_REPO_COMMIT");
    uint8_t salt[32], *rb, *ts, d[32];
    size_t rbl, tl, ol;
    char why[128], out[ST_REC_MAX], path[4096], dh[65];
    StHoldoutCommitment c;
    if (!rc || !*rc) {
        fprintf(stderr, "st_holdout: OMEGA_REPO_COMMIT unset\n");
        return 2;
    }
    if (read_file(argv[1], &rb, &rbl) != 0) {
        fprintf(stderr, "st_holdout: cannot read record %s\n", argv[1]);
        return 2;
    }
    int r = st_holdout_parse((const char *)rb, rbl, &c, why, sizeof why);
    free(rb);
    if (r != 0) {
        fprintf(stderr, "st_holdout: record refused: %s\n", why);
        return 2;
    }
    if (read_salt(argv[3], salt) != 0)
        return 2;
    if (read_file(argv[2], &ts, &tl) != 0) {
        fprintf(stderr, "st_holdout: cannot read taskset %s\n", argv[2]);
        return 2;
    }
    r = st_holdout_reveal(&c, ts, tl, salt, why, sizeof why);
    free(ts);
    memset(salt, 0, sizeof salt);
    if (r != 0) {
        fprintf(stderr, "st_holdout: reveal FAIL: %s\n", why);
        return 2;
    }
    r = st_holdout_reveal_receipt(&c, rc, out, sizeof out, &ol);
    if (r != 0) {
        fprintf(stderr, "st_holdout: receipt refused (%d): OMEGA_REPO_COMMIT "
                        "must be 40 lowercase hex\n", r);
        return 2;
    }
    sha256_hash((const uint8_t *)out, ol, d);
    hex_lower(d, 32, dh);
    if (snprintf(path, sizeof path, "%s/g3-reveal-%s.txt", argv[4], dh) >=
        (int)sizeof path)
        return 2;
    r = write_addressed(path, out, ol);
    if (r != 0) {
        fprintf(stderr, "st_holdout: cannot write %s%s\n", path,
                r == -2 ? " (exists with different bytes)" : "");
        return 2;
    }
    printf("%s\n", path);
    return 0;
}

static int cli_verify(char **argv)
{
    uint8_t *rb;
    size_t rbl;
    char why[128], dh[65];
    StHoldoutCommitment c;
    if (read_file(argv[1], &rb, &rbl) != 0) {
        fprintf(stderr, "st_holdout: cannot read record %s\n", argv[1]);
        return 2;
    }
    int r = st_holdout_parse((const char *)rb, rbl, &c, why, sizeof why);
    free(rb);
    if (r != 0) {
        fprintf(stderr, "st_holdout: record refused: %s\n", why);
        return 2;
    }
    hex_lower(c.record_digest, 32, dh);
    printf("OK holdout_id %s task_count %lu record_digest %s\n", c.holdout_id,
           (unsigned long)c.task_count, dh);
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
            "usage: commit <holdout_id> <taskset_file> <salt_file> <out_dir>\n"
            "       reveal <record_file> <taskset_file> <salt_file> <out_dir>\n"
            "       verify <record_file>\n");
    return 1;
}

int st_holdout_cli(int argc, char **argv)
{
    if (argc < 1 || !argv || !argv[0])
        return usage();
    if (strcmp(argv[0], "commit") == 0 && argc == 5)
        return cli_commit(argv);
    if (strcmp(argv[0], "reveal") == 0 && argc == 5)
        return cli_reveal(argv);
    if (strcmp(argv[0], "verify") == 0 && argc == 2)
        return cli_verify(argv);
    return usage();
}
