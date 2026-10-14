/* checker2: the independent checker's command line (omega#446, spec/one-layer-receipts.md).
 *
 *   checker2 check <unit.ir>                   judge canonical OSC IR bytes
 *   checker2 version                           print the checker identity
 *   checker2 prove-cnf <unit.ir> <func> <out.cnf>    NO-TRAP formula of func (c2_blast.h)
 *   checker2 prove-check <unit.ir> <func> <proof>    rebuild that formula, check an LRAT proof
 *   checker2 receipt issue <unit.ir> <exe> --target T --epoch E --expiry X --out-dir D
 *                    [--firmware F.bin] [--requirements R] [--input I] [--producer P]
 *                    [--dep ID]... [--lrat FUNC=PROOF]...  (proofs checked, evidence class 2)
 *                                              check, sign with the TEST key, store by id
 *   checker2 receipt tbs <receipt> <out65>     write the 65-byte message an owner key signs
 *   checker2 receipt attach <receipt> <sig64> --out-dir D
 *                                              verify an OWNER signature, store by id
 *   checker2 receipt verify <receipt> <unit.ir> <exe> --target T --epoch E
 *                    [--production] [--floor N] [--firmware F.bin] [--revoked-id HEX]...
 *                    [--revoked-key HEX]... [--lrat FUNC=PROOF]...  (required for evidence class 2)
 *
 * Exit: 0 accept / done, 1 reject / refused, 2 usage or I/O error. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "c2_blast.h"
#include "c2_ir.h"
#include "c2_lrat.h"
#include "c2_receipt.h"
#include "sha256.h"

#define C2_CHECKER_VERSION "checker2 1 (omega#446)"
#ifndef C2_SOURCES_SHA256
#define C2_SOURCES_SHA256 "unset"
#endif

static void hex(const uint8_t *b, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
    out[2 * n] = 0;
}

static int unhex32(const char *s, uint8_t out[32])
{
    if (strlen(s) != 64) return -1;
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[2 * i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "checker2: cannot open %s: %s\n", path, strerror(errno)); return NULL; }
    size_t cap = 1 << 16, n = 0;
    uint8_t *b = malloc(cap);
    while (b) {
        size_t r = fread(b + n, 1, cap - n, f);
        n += r;
        if (n < cap) break;
        if (cap >= (1u << 28)) { free(b); b = NULL; break; }
        uint8_t *nb = realloc(b, cap * 2);
        if (!nb) { free(b); b = NULL; break; }
        b = nb;
        cap *= 2;
    }
    int err = ferror(f);
    fclose(f);
    if (!b || err) { free(b); fprintf(stderr, "checker2: cannot read %s\n", path); return NULL; }
    *len = n;
    return b;
}

static int file_sha(const char *path, uint8_t out[32])
{
    size_t n;
    uint8_t *b = slurp(path, &n);
    if (!b) return -1;
    sha256_hash(b, n, out);
    free(b);
    return 0;
}

/* identity = SHA-256(version string ‖ 0 ‖ sources digest hex) (spec section 5) */
static void checker_id(uint8_t out[32])
{
    sha256_ctx c;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)C2_CHECKER_VERSION, sizeof C2_CHECKER_VERSION);
    sha256_update(&c, (const uint8_t *)C2_SOURCES_SHA256, strlen(C2_SOURCES_SHA256));
    sha256_final(&c, out);
}

static int judge(const char *path, uint8_t ir_sha[32], int quiet)
{
    size_t n;
    uint8_t *b = slurp(path, &n);
    if (!b) return 2;
    sha256_hash(b, n, ir_sha);
    C2Verdict v;
    int rc = c2_check_bytes(b, n, &v);
    free(b);
    char h[65];
    hex(ir_sha, 32, h);
    if (rc == 0) {
        if (!quiet) printf("checker2=ACCEPT ir_sha256=%s\n", h);
        return 0;
    }
    printf("checker2=REJECT rule=%s func=%d insn=%d ir_sha256=%s detail=%s\n", v.rule, v.func, v.insn, h, v.detail);
    return 1;
}

/* create a new file with exactly these bytes; never overwrite */
static int write_new(const char *path, const uint8_t *b, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0444);
    if (fd < 0) { fprintf(stderr, "checker2: cannot create %s: %s\n", path, strerror(errno)); return 2; }
    ssize_t w = write(fd, b, n);
    if (w < 0 || (size_t)w != n || fsync(fd) != 0 || close(fd) != 0) { fprintf(stderr, "checker2: write %s failed\n", path); return 2; }
    return 0;
}

/* write bytes to D/<id>.rcpt: exclusive create; identical existing bytes is a no-op */
static int store(const char *dir, const uint8_t r[C2R_SIZE])
{
    uint8_t id[32];
    char h[65], path[4096];
    c2r_id(r, id);
    hex(id, 32, h);
    if (snprintf(path, sizeof path, "%s/%s.rcpt", dir, h) >= (int)sizeof path) return 2;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0444);
    if (fd < 0 && errno == EEXIST) {
        size_t n;
        uint8_t *old = slurp(path, &n);
        int same = old && n == C2R_SIZE && memcmp(old, r, C2R_SIZE) == 0;
        free(old);
        if (same) { printf("receipt=%s unchanged\n", path); return 0; }
        fprintf(stderr, "checker2: %s exists with different bytes; receipts are never rewritten\n", path);
        return 1;
    }
    if (fd < 0) { fprintf(stderr, "checker2: cannot create %s: %s\n", path, strerror(errno)); return 2; }
    ssize_t w = write(fd, r, C2R_SIZE);
    if (w != C2R_SIZE || fsync(fd) != 0 || close(fd) != 0) { fprintf(stderr, "checker2: write %s failed\n", path); return 2; }
    printf("receipt=%s\n", path);
    return 0;
}

static const char *opt(int argc, char **argv, const char *name)
{
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

static int flag(int argc, char **argv, const char *name)
{
    for (int i = 0; i < argc; i++)
        if (strcmp(argv[i], name) == 0) return 1;
    return 0;
}

static int opt_u64(int argc, char **argv, const char *name, uint64_t *out, int required)
{
    const char *s = opt(argc, argv, name);
    if (!s) {
        if (required) fprintf(stderr, "checker2: %s is required\n", name);
        return required ? -1 : 0;
    }
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end || !*s || s[0] == '-') { fprintf(stderr, "checker2: bad number for %s\n", name); return -1; }
    *out = v;
    return 0;
}

static int opt_sha_file(int argc, char **argv, const char *name, uint8_t out[32])
{
    const char *s = opt(argc, argv, name);
    memset(out, 0, 32);
    return s ? file_sha(s, out) : 0;
}

static int property_cnf(const char *ir, const char *func, C2Cnf *cnf, C2BlastInfo *bi, uint8_t ir_sha[32]);

/* Every "--lrat FUNC=PROOF" pair: rebuild FUNC's NO-TRAP formula from the IR bytes and check the
 * proof. The requirements object (spec section 10) is
 *   "OMEGA-REQUIREMENTS-V1\n" then per pair, FUNC strictly ascending:
 *   "no-trap-except-requires <FUNC> proof_sha256=<hex>\n"
 * Returns 0 with *count pairs and req = SHA-256 of the object (object text in obj, may be NULL),
 * 1 if a proof is refused or the order is wrong, 2 on usage or I/O error. */
static int lrat_requirements(int argc, char **argv, const char *ir, uint8_t req[32], int *count, char *obj, size_t objcap)
{
    char text[8192], prev[256] = "";
    size_t tn = (size_t)snprintf(text, sizeof text, "OMEGA-REQUIREMENTS-V1\n");
    *count = 0;
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--lrat") != 0) continue;
        const char *arg = argv[i + 1], *eq = strchr(arg, '=');
        if (!eq || eq == arg || (size_t)(eq - arg) >= sizeof prev) { fprintf(stderr, "checker2: --lrat wants FUNC=PROOF\n"); return 2; }
        char func[256];
        memcpy(func, arg, (size_t)(eq - arg));
        func[eq - arg] = 0;
        if (*count && strcmp(prev, func) >= 0) { fprintf(stderr, "checker2: --lrat functions must be strictly ascending\n"); return 1; }
        snprintf(prev, sizeof prev, "%s", func);
        C2Cnf cnf;
        C2BlastInfo bi;
        uint8_t s[32], ps[32];
        int rc = property_cnf(ir, func, &cnf, &bi, s);
        if (rc) return rc;
        size_t n;
        uint8_t *p = slurp(eq + 1, &n);
        if (!p) { c2_cnf_free(&cnf); return 2; }
        sha256_hash(p, n, ps);
        C2LratResult r;
        int e = c2_lrat_check(&cnf, (const char *)p, n, &r);
        free(p);
        c2_cnf_free(&cnf);
        if (e) { printf("lrat=REFUSED func=%s rule=%s line=%ld\n", func, r.rule, r.line); return 1; }
        char h[65];
        hex(ps, 32, h);
        int k = snprintf(text + tn, sizeof text - tn, "%s %s proof_sha256=%s\n", C2_PROPERTY_NOTRAP, func, h);
        if (k < 0 || (size_t)k >= sizeof text - tn) { fprintf(stderr, "checker2: too many --lrat\n"); return 2; }
        tn += (size_t)k;
        (*count)++;
    }
    sha256_hash((const uint8_t *)text, tn, req);
    if (obj) snprintf(obj, objcap, "%s", text);
    return 0;
}

static int receipt_issue(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *ir = argv[0], *exe = argv[1], *target = opt(argc, argv, "--target"), *dir = opt(argc, argv, "--out-dir");
    const char *draft = opt(argc, argv, "--owner-draft");
    if (!target || (!dir == !draft)) { fprintf(stderr, "checker2: --target and exactly one of --out-dir / --owner-draft are required\n"); return 2; }
    C2Receipt r;
    memset(&r, 0, sizeof r);
    r.version = C2R_FORMAT_VERSION;
    r.signer_class = C2R_CLASS_TEST;
    r.alg = C2R_ALG_ED25519;
    r.evidence_class = C2R_EV_IR_CHECKED;
    int rc = judge(ir, r.program, 1);
    if (rc) { fprintf(stderr, "checker2: refusing to issue a receipt for bytes checker2 does not accept\n"); return rc; }
    if (file_sha(exe, r.executable)) return 2;
    if (opt_u64(argc, argv, "--epoch", &r.policy_epoch, 1) || opt_u64(argc, argv, "--expiry", &r.expiry_epoch, 1)) return 2;
    if (opt_sha_file(argc, argv, "--firmware", r.firmware) || opt_sha_file(argc, argv, "--requirements", r.requirements) ||
        opt_sha_file(argc, argv, "--input", r.input) || opt_sha_file(argc, argv, "--producer", r.producer_checker))
        return 2;
    if (opt(argc, argv, "--lrat")) {
        if (opt(argc, argv, "--requirements")) { fprintf(stderr, "checker2: --lrat and --requirements are exclusive\n"); return 2; }
        int nproofs;
        char obj[8192];
        rc = lrat_requirements(argc, argv, ir, r.requirements, &nproofs, obj, sizeof obj);
        if (rc) { fprintf(stderr, "checker2: refusing to issue: a proof did not check\n"); return rc; }
        r.evidence_class = C2R_EV_IR_CHECKED_LRAT;
        char h[65];
        hex(r.requirements, 32, h);
        printf("requirements_sha256=%s proofs=%d\n%s", h, nproofs, obj);
    }
    uint8_t deps[64][32];
    unsigned nd = 0;
    for (int i = 0; i + 1 < argc; i++)
        if (strcmp(argv[i], "--dep") == 0) {
            if (nd == 64 || unhex32(argv[i + 1], deps[nd])) { fprintf(stderr, "checker2: bad --dep\n"); return 2; }
            nd++;
        }
    if (c2r_deps_digest((const uint8_t (*)[32])deps, nd, r.dependencies)) { fprintf(stderr, "checker2: --dep ids must be strictly ascending\n"); return 2; }
    c2r_target_id(target, r.target);
    checker_id(r.checker);
    uint8_t buf[C2R_SIZE], seed[32];
    if (draft) {
        /* OWNER draft: class OWNER, key fingerprint of the owner root, signature all zero. The
         * owner signs `receipt tbs` output on the YubiKey; `receipt attach` verifies and stores. */
        r.signer_class = C2R_CLASS_OWNER;
        sha256_hash(c2r_owner_root_pk, 32, r.key_fp);
        c2r_encode(&r, buf);
        return write_new(draft, buf, C2R_SIZE);
    }
    c2r_test_seed(seed);
    int e = c2r_sign_test(&r, seed, buf);
    if (e) { fprintf(stderr, "checker2: signing refused: %s\n", c2r_reason(e)); return 1; }
    return store(dir, buf);
}

static int read_receipt(const char *path, uint8_t buf[C2R_SIZE], C2Receipt *r)
{
    size_t n;
    uint8_t *b = slurp(path, &n);
    if (!b) return 2;
    int e = c2r_decode(b, n, r);
    if (!e) memcpy(buf, b, C2R_SIZE);
    free(b);
    if (e) { fprintf(stderr, "checker2: not a receipt: %s\n", c2r_reason(e)); return 1; }
    return 0;
}

static int receipt_tbs(int argc, char **argv)
{
    if (argc < 2) return 2;
    uint8_t buf[C2R_SIZE], msg[C2R_SIGNED_MSG_LEN];
    C2Receipt r;
    int rc = read_receipt(argv[0], buf, &r);
    if (rc) return rc;
    c2r_signed_message(buf, msg);
    return write_new(argv[1], msg, sizeof msg);
}

static int receipt_attach(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *dir = opt(argc, argv, "--out-dir");
    if (!dir) { fprintf(stderr, "checker2: --out-dir is required\n"); return 2; }
    uint8_t buf[C2R_SIZE], out[C2R_SIZE];
    C2Receipt r;
    int rc = read_receipt(argv[0], buf, &r);
    if (rc) return rc;
    if (r.signer_class != C2R_CLASS_OWNER) { fprintf(stderr, "checker2: attach is for OWNER receipts\n"); return 1; }
    size_t ns;
    uint8_t *s = slurp(argv[1], &ns);
    if (!s) return 2;
    if (ns != 64) { free(s); fprintf(stderr, "checker2: signature must be 64 bytes\n"); return 2; }
    int e = c2r_attach(&r, c2r_owner_root_pk, s, out);
    free(s);
    if (e) { fprintf(stderr, "checker2: attach refused: %s\n", c2r_reason(e)); return 1; }
    return store(dir, out);
}

static int receipt_verify(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *target = opt(argc, argv, "--target");
    if (!target) { fprintf(stderr, "checker2: --target is required\n"); return 2; }
    uint8_t tgt[32], fw[32], prog[32], exe[32];
    c2r_target_id(target, tgt);
    int rc = judge(argv[1], prog, 1);
    if (rc == 0 && file_sha(argv[2], exe)) rc = 2;
    if (rc == 0 && opt_sha_file(argc, argv, "--firmware", fw)) rc = 2;
    C2ReceiptExpect ex = {.target = tgt, .firmware = fw, .program = prog, .executable = exe};
    C2ReceiptPolicy p;
    memset(&p, 0, sizeof p);
    p.production = flag(argc, argv, "--production");
    if (!p.production) { p.test_anchors = &c2r_test_pk; p.n_test = 1; }
    p.owner_anchors = &c2r_owner_root_pk;
    p.n_owner = 1;
    static uint8_t ids[16][32], keys[16][32];
    unsigned nid = 0, nkey = 0;
    for (int i = 0; rc == 0 && i + 1 < argc; i++) {
        if (strcmp(argv[i], "--revoked-id") == 0) rc = nid < 16 && !unhex32(argv[i + 1], ids[nid]) ? (nid++, 0) : 2;
        if (strcmp(argv[i], "--revoked-key") == 0) rc = nkey < 16 && !unhex32(argv[i + 1], keys[nkey]) ? (nkey++, 0) : 2;
    }
    p.revoked_ids = (const uint8_t (*)[32])ids;
    p.n_revoked_ids = nid;
    p.revoked_keys = (const uint8_t (*)[32])keys;
    p.n_revoked_keys = nkey;
    if (rc == 0 && (opt_u64(argc, argv, "--epoch", &p.current_epoch, 1) || opt_u64(argc, argv, "--floor", &p.epoch_floor, 0))) rc = 2;
    if (rc) return rc;
    size_t n;
    uint8_t *b = slurp(argv[0], &n);
    if (!b) return 2;
    C2Receipt got;
    int e = c2r_verify(b, n, &p, &ex, &got);
    free(b);
    if (e) { printf("receipt=%s\n", c2r_reason(e)); return 1; }
    /* LRAT evidence is replayed, never taken on trust: the proofs must be given, must check
     * against these IR bytes, and must hash to the receipt's requirements digest. */
    if (got.evidence_class == C2R_EV_IR_CHECKED_LRAT || opt(argc, argv, "--lrat")) {
        uint8_t req[32];
        int nproofs;
        rc = lrat_requirements(argc, argv, argv[1], req, &nproofs, NULL, 0);
        if (rc) { printf("receipt=LRAT-REPLAY-FAILED\n"); return rc; }
        if (got.evidence_class != C2R_EV_IR_CHECKED_LRAT) { printf("receipt=LRAT-NOT-CLAIMED\n"); return 1; }
        if (nproofs == 0) { printf("receipt=LRAT-PROOFS-REQUIRED\n"); return 1; }
        if (memcmp(req, got.requirements, 32)) { printf("receipt=LRAT-REQUIREMENTS-MISMATCH\n"); return 1; }
    }
    printf("receipt=VALID\n");
    return 0;
}

/* decode + accept the unit, locate the function, build its NO-TRAP formula (never read from disk) */
static int property_cnf(const char *ir, const char *func, C2Cnf *cnf, C2BlastInfo *bi, uint8_t ir_sha[32])
{
    size_t n;
    uint8_t *b = slurp(ir, &n);
    if (!b) return 2;
    sha256_hash(b, n, ir_sha);
    C2Unit *u = malloc(sizeof *u);
    C2Verdict v;
    int rc = 1;
    if (!u) { free(b); return 2; }
    if (c2_decode(b, n, u, &v)) printf("checker2=REJECT rule=%s detail=%s\n", v.rule, v.detail);
    else if (c2_blast_notrap(u, c2_func_index(u, func), cnf, bi)) printf("prove=REFUSED rule=%s detail=%s\n", bi->rule, bi->detail);
    else rc = 0;
    c2_unit_free(u);
    free(u);
    free(b);
    return rc;
}

static int prove_cnf(const char *ir, const char *func, const char *out)
{
    C2Cnf cnf;
    C2BlastInfo bi;
    uint8_t s[32];
    int rc = property_cnf(ir, func, &cnf, &bi, s);
    if (rc) return rc;
    size_t n;
    char *t = c2_cnf_print(&cnf, &n);
    rc = t ? write_new(out, (const uint8_t *)t, n) : 2;
    if (!rc) printf("property=%s func=%s inputs=%d paths=%d trap_sites=%d vars=%d clauses=%d\n", C2_PROPERTY_NOTRAP, func,
                    bi.inputs, bi.paths, bi.trap_sites, cnf.nvars, cnf.nclauses);
    free(t);
    c2_cnf_free(&cnf);
    return rc;
}

static int prove_check(const char *ir, const char *func, const char *proof)
{
    C2Cnf cnf;
    C2BlastInfo bi;
    uint8_t s[32], ps[32];
    int rc = property_cnf(ir, func, &cnf, &bi, s);
    if (rc) return rc;
    size_t n;
    uint8_t *p = slurp(proof, &n);
    if (!p) { c2_cnf_free(&cnf); return 2; }
    sha256_hash(p, n, ps);
    C2LratResult r;
    int e = c2_lrat_check(&cnf, (const char *)p, n, &r);
    free(p);
    c2_cnf_free(&cnf);
    char h1[65], h2[65];
    hex(s, 32, h1);
    hex(ps, 32, h2);
    if (e) { printf("lrat=REFUSED rule=%s line=%ld\n", r.rule, r.line); return 1; }
    printf("lrat=VERIFIED property=%s func=%s ir_sha256=%s proof_sha256=%s steps=%ld\n", C2_PROPERTY_NOTRAP, func, h1, h2, r.steps);
    return 0;
}

static int usage(void)
{
    fprintf(stderr, "usage: checker2 check <unit.ir> | version | receipt issue|tbs|attach|verify ... (see source header)\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    if (strcmp(argv[1], "version") == 0) {
        uint8_t id[32];
        char h[65];
        checker_id(id);
        hex(id, 32, h);
        printf("%s\nsources_sha256=%s\nchecker_id=%s\n", C2_CHECKER_VERSION, C2_SOURCES_SHA256, h);
        return 0;
    }
    if (strcmp(argv[1], "prove-cnf") == 0 && argc == 5) return prove_cnf(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "prove-check") == 0 && argc == 5) return prove_check(argv[2], argv[3], argv[4]);
    if (strcmp(argv[1], "check") == 0 && argc == 3) {
        uint8_t s[32];
        return judge(argv[2], s, 0);
    }
    if (strcmp(argv[1], "receipt") == 0 && argc >= 3) {
        int (*fn)(int, char **) = NULL;
        if (strcmp(argv[2], "issue") == 0) fn = receipt_issue;
        if (strcmp(argv[2], "tbs") == 0) fn = receipt_tbs;
        if (strcmp(argv[2], "attach") == 0) fn = receipt_attach;
        if (strcmp(argv[2], "verify") == 0) fn = receipt_verify;
        if (fn) {
            int rc = fn(argc - 3, argv + 3);
            return rc == 2 && argc - 3 < 2 ? usage() : rc;
        }
    }
    return usage();
}
