/* test-turing: TURING Field V0 + control arm on stored receipts.
 * docs/turing/TURING_W0_PROPOSAL.md sections J and K. No timed runs. */
#define _POSIX_C_SOURCE 200809L
#include "turing/select.h"

#include "omega_canonical.h"
#include "sha256.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int pass, fail;
#define CHECK(c, ...)                                     \
    do {                                                  \
        if (c) {                                          \
            ++pass;                                       \
        } else {                                          \
            ++fail;                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static const char *R1 = "evidence/MIXED_ALGEBRA/ma3_bench_run1.json";
static const char *R2 = "evidence/MIXED_ALGEBRA/ma3_bench_run2.json";

/* Golden values: a rebuild (any flags, any day) must reproduce them. */
static const char *GOLD_CONTRACT = "75772afe8a98f73064f2933d76bc58f7bfe5b9353eb40b703e173b4cd3f22b5f";
static const char *GOLD_CRUMB = "d785f5ba7b7cae43f84f71e01fa279d7bff9fbdd0f9604e5b55f6b692b5e00f7";
static const char *GOLD_PLAIN = "272d7bdfd34514286b303857c98beb3c10b0ffada5e1d38dfb9fa94eaef17f99";

static turing_store *load2(const char *a, const char *b) {
    turing_store *st = turing_store_new();
    if (!st || turing_ingest_registry(st) < 0 || turing_ingest_receipt(st, a) <= 0 ||
        (b && turing_ingest_receipt(st, b) <= 0)) {
        turing_store_free(st);
        return NULL;
    }
    return st;
}

static int copy_file(const char *from, const char *to) {
    FILE *i = fopen(from, "rb"), *o = fopen(to, "wb");
    int ok = i && o;
    char buf[8192];
    size_t k;
    while (ok && (k = fread(buf, 1, sizeof buf, i)) > 0) ok = fwrite(buf, 1, k, o) == k;
    if (i) fclose(i);
    if (o) ok = (fclose(o) == 0) && ok;
    return ok ? 0 : -1;
}

static int flip_byte(const char *path, long off) {
    FILE *f = fopen(path, "r+b");
    if (!f) return -1;
    int rc = -1;
    if (fseek(f, off, SEEK_SET) == 0) {
        int c = fgetc(f);
        if (c != EOF && fseek(f, off, SEEK_SET) == 0 && fputc(c == '1' ? '2' : '1', f) != EOF) rc = 0;
    }
    return fclose(f) == 0 ? rc : -1;
}

static void test_digests(void) {
    turing_contract c1, c2;
    turing_contract_omega_x(&c1);
    turing_contract_omega_x(&c2);
    turing_digest d1, d2;
    char h[65];
    CHECK(turing_contract_digest(&c1, &d1) == 0 && turing_contract_digest(&c2, &d2) == 0, "contract digest");
    CHECK(turing_digest_eq(&d1, &d2), "contract digest reproducible");
    turing_hex(&d1, h);
    CHECK(!strcmp(h, GOLD_CONTRACT), "contract digest golden: got %s", h);
    c2.max_n -= 1;
    CHECK(turing_contract_digest(&c2, &d2) == 0 && !turing_digest_eq(&d1, &d2), "contract field change -> new digest");

    turing_store *st = turing_store_new();
    CHECK(st && turing_ingest_registry(st) == 10, "registry adapter: 10 realizations");
    if (!st) return;
    int crumb = turing_find_spec(st, "R2c_crumb"), plain = turing_find_spec(st, "R1_plain");
    CHECK(crumb >= 0 && plain >= 0, "specs present");
    turing_hex(&st->spec_id[crumb], h);
    CHECK(!strcmp(h, GOLD_CRUMB), "R2c_crumb spec_id golden: got %s", h);
    turing_hex(&st->spec_id[plain], h);
    CHECK(!strcmp(h, GOLD_PLAIN), "R1_plain spec_id golden: got %s", h);
    /* One contract across all 10. */
    size_t same = 0;
    for (size_t k = 0; k < st->nspec; ++k) same += turing_digest_eq(&st->spec[k].contract_digest, &st->contract_digest);
    CHECK(same == 10 && st->nspec == 10, "one contract_digest across all %zu specs (%zu match)", st->nspec, same);
    /* Distinct spec ids. */
    int distinct = 1;
    for (size_t a = 0; a < st->nspec; ++a)
        for (size_t b = a + 1; b < st->nspec; ++b) distinct &= !turing_digest_eq(&st->spec_id[a], &st->spec_id[b]);
    CHECK(distinct, "spec ids distinct");
    /* Domain separation: a Field digest is not SHA-256 of the OMG0 bytes (the
     * Omega semantic id construction). */
    uint8_t buf[8192];
    size_t len = 0;
    turing_digest raw;
    CHECK(turing_spec_bytes(&st->spec[crumb], buf, sizeof buf, &len) == 0 && len > 6, "spec canonical bytes");
    CHECK(!memcmp(buf, "OMG0", 4), "spec bytes use the OMG0 layout");
    sha256_hash(buf, len, raw.b);
    CHECK(!turing_digest_eq(&raw, &st->spec_id[crumb]), "spec_id is domain-separated from an Omega semantic id");
    /* Byte-level reproducibility. */
    uint8_t buf2[8192];
    size_t len2 = 0;
    CHECK(turing_spec_bytes(&st->spec[crumb], buf2, sizeof buf2, &len2) == 0 && len == len2 && !memcmp(buf, buf2, len),
          "spec canonical bytes reproducible");
    turing_store_free(st);
}

static void test_rebuild_stable(void) {
    /* spec ids from the registry == spec ids reconstructed from receipt rows. */
    turing_store *reg = turing_store_new(), *rec = turing_store_new();
    CHECK(reg && rec, "alloc");
    if (!reg || !rec) return;
    CHECK(turing_ingest_registry(reg) == 10, "registry");
    CHECK(turing_ingest_receipt(rec, R1) == 360, "receipt-only store: 360 rows");
    CHECK(rec->nspec == 10, "receipt-only store: 10 specs (%zu)", rec->nspec);
    size_t match = 0;
    for (size_t k = 0; k < reg->nspec; ++k) {
        int j = turing_find_spec(rec, reg->spec[k].rz_id);
        match += j >= 0 && turing_digest_eq(&reg->spec_id[k], &rec->spec_id[j]);
    }
    CHECK(match == 10, "registry and receipt adapters give identical spec ids (%zu/10)", match);
    /* Build fields live on evidence, not on the spec. */
    const turing_evidence *e = &rec->ev[0];
    CHECK(strlen(e->build_digest) == 64 && strlen(e->git_commit) == 40 && e->toolchain[0], "evidence carries build");
    CHECK(!strcmp(e->tier, "E2") && strstr(e->tier_source, "PROPOSED"), "tier text + ADR revision");
    CHECK(strstr(e->cpu_freq_state, "not recorded") && strstr(e->quiet_flag, "not recorded"),
          "cpu freq + quiet flag stated as not recorded");
    /* Evidence digests reproducible. */
    size_t ok = 0;
    for (size_t i = 0; i < rec->nev; ++i) {
        turing_digest d;
        ok += turing_evidence_digest(&rec->ev[i], &d) == 0 && turing_digest_eq(&d, &rec->ev_id[i]);
    }
    CHECK(ok == rec->nev, "evidence digests reproducible (%zu/%zu)", ok, rec->nev);
    turing_store_free(reg);
    turing_store_free(rec);
}

static void test_refusals(void) {
    char dir[] = "/tmp/turing_refuse_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char p[300];
    snprintf(p, sizeof p, "%s/bad.json", dir);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("{\n  \"schema\": \"SOMETHING_ELSE\",\n  \"run_id\": \"x\"\n}\n", f);
        fclose(f);
    }
    turing_store *st = turing_store_new();
    CHECK(st && turing_ingest_registry(st) == 10, "registry");
    CHECK(turing_ingest_receipt(st, p) < 0 && st->nev == 0, "unknown schema refused");
    CHECK(turing_ingest_receipt(st, "/nonexistent/receipt.json") < 0, "missing receipt refused");
    /* A receipt whose realization row disagrees with the registry is refused. */
    snprintf(p, sizeof p, "%s/maxn.json", dir);
    CHECK(copy_file(R1, p) == 0, "copy");
    FILE *g = fopen(p, "r+b");
    long off = -1;
    if (g) {
        char line[1024];
        long pos = 0;
        while (fgets(line, sizeof line, g)) {
            char *hit = strstr(line, "\"id\": \"R3_sparse\"");
            char *mx = hit ? strstr(line, "\"max_n\": 65536") : NULL;
            if (mx) {
                off = pos + (mx - line) + 10; /* first digit of 65536 */
                break;
            }
            pos = ftell(g);
        }
        fclose(g);
    }
    CHECK(off > 0 && flip_byte(p, off) == 0, "edit R3_sparse max_n in copy");
    CHECK(turing_ingest_receipt(st, p) < 0, "registry/receipt spec disagreement refused");
    CHECK(st->nev == 0, "no half-read evidence kept (%zu)", st->nev);
    turing_store_free(st);
}

static void test_decisions_verify(void) {
    turing_store *st = load2(R1, R2);
    CHECK(st != NULL, "load both receipts");
    if (!st) return;
    CHECK(st->nev == 720 && st->nreceipt == 2, "720 evidence rows, 2 receipts (%zu)", st->nev);
    turing_query cells[80];
    size_t nc = turing_cells(st, cells, 80);
    CHECK(nc == 36, "36 measured cells (%zu)", nc);
    size_t ok = 0, total = 0, exact = 0, cites_cell = 0;
    turing_decision d;
    for (int pack = 0; pack < 2; ++pack)
        for (size_t i = 0; i < nc; ++i) {
            turing_query q = cells[i];
            q.pack = pack;
            char why[200];
            if (turing_field_select(st, &q, NULL, NULL, &d) != 0) continue;
            ++total;
            ok += turing_decision_verify(st, &d, why, sizeof why) == 0 && d.ncite == 20;
            exact += d.exact_cell;
            int all = 1;
            for (size_t c = 0; c < d.ncite; ++c) {
                int k = turing_find_evidence(st, &d.cite[c]);
                all &= k >= 0 && st->ev[k].n == q.n && st->ev[k].m == q.m && st->ev[k].sparsity_milli == q.sparsity_milli;
            }
            cites_cell += all;
        }
    CHECK(total == 72 && ok == 72, "72 Field decisions, each cites 20 rows whose receipts verify (%zu/%zu)", ok, total);
    CHECK(exact == 72 && cites_cell == 72, "every decision on its exact cell cites only that cell");

    /* Winner flip from stored evidence alone: L2-fit int8, DRAM-bound crumb. */
    turing_query s1 = {4096, 64, 300, TURING_PACK_ONCE, 0}, s4 = {16384, 4096, 300, TURING_PACK_ONCE, 0};
    turing_decision a, b;
    CHECK(turing_field_select(st, &s1, NULL, NULL, &a) == 0 && !strncmp(a.cand_rz[a.chosen], "R1_", 3),
          "S1 (L2-fit) -> binary int8 (%s)", a.cand_rz[a.chosen]);
    CHECK(turing_field_select(st, &s4, NULL, NULL, &b) == 0 && !strcmp(b.cand_rz[b.chosen], "R2c_crumb"),
          "S4 (DRAM-bound) -> R2c_crumb (%s)", b.cand_rz[b.chosen]);

    /* Decision digests: reproducible; supersedes changes the digest. */
    turing_digest i1, i2, i3;
    CHECK(turing_decision_digest(&b, &i1) == 0 && turing_decision_digest(&b, &i2) == 0 && turing_digest_eq(&i1, &i2),
          "decision digest reproducible");
    turing_decision b2;
    CHECK(turing_field_select(st, &s4, NULL, &i1, &b2) == 0 && b2.has_supersedes && turing_decision_digest(&b2, &i3) == 0 &&
              !turing_digest_eq(&i1, &i3),
          "superseding decision has its own digest");

    /* Off-grid query: domain filter with reason codes, nearest cell. */
    turing_query big = {100000, 64, 300, TURING_PACK_ONCE, 0};
    turing_decision o;
    CHECK(turing_field_select(st, &big, NULL, NULL, &o) == 0 && !o.exact_cell, "off-grid decision");
    int r3 = turing_find_spec(st, "R3_sparse"), r4 = turing_find_spec(st, "R4_rns");
    CHECK(o.reason[r3] == TURING_R_MAX_N && o.reason[r4] == TURING_R_MAX_N, "R3/R4 filtered MAX_N");
    CHECK(o.cell_n == 16384 && o.cell_m == 64, "nearest cell 16384 x 64");

    /* Incumbent inside a tied set stands (S1 per call is a TIE). */
    turing_query s1pc = {4096, 64, 300, TURING_PACK_PER_CALL, 0};
    turing_decision t0, t1;
    CHECK(turing_field_select(st, &s1pc, NULL, NULL, &t0) == 0 && !strcmp(t0.verdict, "TIE"), "S1 per call TIE");
    int other = -1;
    for (size_t k = 0; k < t0.ncand; ++k)
        if (t0.reason[k] == TURING_R_TIED) other = (int)k;
    if (other >= 0) {
        CHECK(turing_field_select(st, &s1pc, t0.cand_rz[other], NULL, &t1) == 0 && t1.chosen == other &&
                  !strcmp(t1.tie_resolution, "incumbent"),
              "incumbent inside tied set stands");
    }

    /* In-memory tamper of a cited evidence record -> verify fails. */
    int k = turing_find_evidence(st, &b.cite[0]);
    char why[200];
    if (k >= 0) {
        uint64_t keep = st->ev[k].median_ps;
        st->ev[k].median_ps += 1;
        CHECK(turing_decision_verify(st, &b, why, sizeof why) == -5, "tampered evidence record detected (%s)", why);
        st->ev[k].median_ps = keep;
        CHECK(turing_decision_verify(st, &b, why, sizeof why) == 0, "restored record verifies");
    }
    turing_store_free(st);
}

static void test_receipt_tamper(void) {
    char dir[] = "/tmp/turing_tamper_XXXXXX";
    CHECK(mkdtemp(dir) != NULL, "mkdtemp");
    char a[300], b[300];
    snprintf(a, sizeof a, "%s/run1.json", dir);
    snprintf(b, sizeof b, "%s/run2.json", dir);
    CHECK(copy_file(R1, a) == 0 && copy_file(R2, b) == 0, "copy receipts");
    turing_store *st = load2(a, b);
    CHECK(st != NULL, "load copies");
    if (!st) return;
    turing_query s4 = {16384, 4096, 300, TURING_PACK_ONCE, 0};
    turing_decision d, d2;
    char why[300];
    CHECK(turing_field_select(st, &s4, NULL, NULL, &d) == 0 && turing_decision_verify(st, &d, why, sizeof why) == 0,
          "decision on copies verifies");
    CHECK(flip_byte(a, 5000) == 0, "tamper one byte of run1 copy");
    int v = turing_decision_verify(st, &d, why, sizeof why);
    CHECK(v == -8, "tampered receipt -> decision verify fails (%d: %s)", v, why);
    /* A new decision stops using the tampered receipt. */
    CHECK(turing_field_select(st, &s4, NULL, NULL, &d2) == 0 && d2.ncite == 10 &&
              turing_decision_verify(st, &d2, why, sizeof why) == 0,
          "fresh decision cites only the intact receipt (%zu cites)", d2.ncite);
    CHECK(flip_byte(b, 5000) == 0, "tamper run2 copy too");
    turing_decision d3;
    CHECK(turing_field_select(st, &s4, NULL, NULL, &d3) == 1 && d3.chosen < 0 &&
              d3.reason[turing_find_spec(st, "R2c_crumb")] == TURING_R_RECEIPT_UNVERIFIED,
          "no verified receipt -> no choice, RECEIPT_UNVERIFIED");
    unlink(a);
    unlink(b);
    rmdir(dir);
    turing_store_free(st);
}

static void test_history(void) {
    turing_store *st = load2(R1, R2);
    if (!st) return;
    turing_history *h = malloc(sizeof *h), *h2 = malloc(sizeof *h2);
    turing_decision d;
    turing_query q = {4096, 64, 300, TURING_PACK_ONCE, 0};
    turing_history_init(h, 42);
    turing_history_init(h2, 43);
    int seen[TURING_MAX_SPECS] = {0}, order1[10], order2[10], explore = 1;
    for (int t = 0; t < 10; ++t) {
        int k = turing_history_select(h, st, &q, &d);
        explore &= k >= 0 && !strcmp(d.verdict, "EXPLORE");
        if (k >= 0) {
            seen[k]++;
            order1[t] = k;
            turing_history_observe(h, st, (size_t)k, q.n, q.m, q.pack, (uint64_t)(1000 + 10 * k));
        }
        int k2 = turing_history_select(h2, st, &q, &d);
        order2[t] = k2;
        if (k2 >= 0) turing_history_observe(h2, st, (size_t)k2, q.n, q.m, q.pack, 5);
    }
    int once = 1;
    for (int k = 0; k < 10; ++k) once &= seen[k] == 1;
    CHECK(explore && once, "cold start: seeded round-robin tries each spec exactly once");
    CHECK(memcmp(order1, order2, sizeof order1) != 0, "different seed -> different exploration order");
    int k = turing_history_select(h, st, &q, &d);
    CHECK(k == 0 && !strcmp(d.verdict, "CHOSEN"), "after calibration: minimum running mean (%d)", k);
    CHECK(turing_decision_verify(st, &d, (char[64]){0}, 64) == -3, "control arm cites no receipts");
    free(h);
    free(h2);
    turing_store_free(st);
}

static void test_compare(void) {
    turing_store *st = load2(R1, R2);
    if (!st) return;
    turing_query cells[80], all[160];
    size_t nc = turing_cells(st, cells, 80), na = 0;
    for (int pack = 0; pack < 2; ++pack)
        for (size_t i = 0; i < nc; ++i) all[na] = cells[i], all[na++].pack = pack;
    turing_query named[8];
    size_t nn = 0;
    const uint64_t shp[4][2] = {{4096, 64}, {16384, 64}, {4096, 4096}, {16384, 4096}};
    for (int pack = 0; pack < 2; ++pack)
        for (int s = 0; s < 4; ++s) named[nn++] = (turing_query){shp[s][0], shp[s][1], 300, pack, 0};
    const uint64_t seed = 0x7475726967303031ull;
    turing_regret r;
    const char *label[4] = {"S1-S4 x2 pack in-sample", "S1-S4 x2 pack leave-one-out", "72 cells in-sample",
                            "72 cells leave-one-out"};
    for (int i = 0; i < 4; ++i) {
        int rc = turing_compare(st, i < 2 ? named : all, i < 2 ? nn : na, i & 1, seed, &r);
        CHECK(rc == 0 && r.decisions == (i < 2 ? 8u : 72u), "compare %s", label[i]);
        CHECK(r.field_cites_ok == r.decisions && r.hist_cites_ok == 0, "cites: Field all verify, control none");
        CHECK(r.field_mean >= 0 && r.hist_mean >= 0, "regret non-negative");
        printf("  regret %-28s Field mean %6.2f%% max %6.2f%% | control mean %6.2f%% max %6.2f%% | same pick %zu/%zu\n",
               label[i], 100 * r.field_mean, 100 * r.field_max, 100 * r.hist_mean, 100 * r.hist_max, r.agree,
               r.decisions);
    }
    CHECK(turing_compare(st, named, 4, 0, seed, &r) == 0 && r.field_max == 0.0 && r.hist_max == 0.0,
          "pack once S1-S4: both selectors pick the oracle");
    turing_regret on;
    CHECK(turing_compare_online(st, all, na, 10, seed, &on) == 0 && on.decisions == 720, "online replay");
    printf("  regret %-28s Field mean %6.2f%% max %6.2f%% | control mean %6.2f%% max %6.2f%%\n", "72 cells cold online x10",
           100 * on.field_mean, 100 * on.field_max, 100 * on.hist_mean, 100 * on.hist_max);
    turing_regret on2;
    CHECK(turing_compare_online(st, all, na, 10, seed, &on2) == 0 && on2.hist_mean == on.hist_mean,
          "online replay deterministic for a seed");
    turing_store_free(st);
}

int main(void) {
    test_digests();
    test_rebuild_stable();
    test_refusals();
    test_decisions_verify();
    test_receipt_tamper();
    test_history();
    test_compare();
    printf("test-turing: %d passed, %d failed\n", pass, fail);
    return fail ? 1 : 0;
}
