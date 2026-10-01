/* test_st_holdout.c - G3 sealed-holdout commitment format v1 tests.
 * Build: gcc -std=c11 -Wall -Wextra -Werror -pedantic -O2
 *        -D_POSIX_C_SOURCE=200809L -Isrc -o /tmp/t tests/searchtrace/
 *        test_st_holdout.c src/searchtrace/st_holdout.c src/sha256.c */
#include "searchtrace/st_holdout.h"
#include "sha256.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_run, g_pass;

static void check(const char *name, int ok)
{
    g_run++;
    if (ok)
        g_pass++;
    printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}

/* ---- known-answer vector ------------------------------------------------
 * Computed once, independently of this C code, with coreutils only:
 *   salt = bytes 0x00..0x1f; taskset = "task alpha\ntask beta\n" (21 B, 2 tasks)
 *   salt_digest = { printf 'omega.g3.holdout.salt.v1\000'; <salt>; } | sha256sum
 *   commitment  = { printf 'omega.g3.holdout.commit.v1\000'; <salt>;
 *                   printf '\000\000\000\000\000\000\000\025\000\000\000\002';
 *                   printf 'task alpha\ntask beta\n'; } | sha256sum
 *   end         = sha256sum of the 7 record lines before "end "
 *   record digest = sha256sum of the full record (file name). */
#define KAT_TASKSET "task alpha\ntask beta\n"
#define KAT_SD "731be661741f6bbf7b8ed2dc8150a6a35238d7b133817bdf1347415bff4ec4a4"
#define KAT_CM "7008574e68b49d88b0debe5101eec7823a0f11a99833cb5e6aedc9262822b20d"
#define KAT_END "f241a91082e4ee19e4612c4fb7a3c6108182449c515bee35be6797bd8ef387ee"
#define KAT_RD "20ba763127e9ffd0b563365d1db13f7d3b813f39c9d3b215c1447a48e9f39a28"

#define L_MAGIC "OMEGA-G3-HOLDOUT-COMMIT v1\n"
#define L_DOM "domain omega.g3.holdout.commit.v1\n"
#define L_ID "holdout_id g3-kat-1\n"
#define L_TC "task_count 2\n"
#define L_TL "taskset_len 21\n"
#define L_SD "salt_digest " KAT_SD "\n"
#define L_CM "commitment " KAT_CM "\n"
#define KAT_RECORD L_MAGIC L_DOM L_ID L_TC L_TL L_SD L_CM "end " KAT_END "\n"

static void kat_salt(uint8_t s[32])
{
    for (int i = 0; i < 32; i++)
        s[i] = (uint8_t)i;
}

static void hexs(const uint8_t *d, size_t n, char *o)
{
    static const char x[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        o[2 * i] = x[d[i] >> 4];
        o[2 * i + 1] = x[d[i] & 15];
    }
    o[2 * n] = 0;
}

/* Appends a correct "end <sha256(body)>\n" so a format check is reached. */
static size_t reseal(const char *body, size_t bl, char *out)
{
    uint8_t d[32];
    char h[65];
    sha256_hash((const uint8_t *)body, bl, d);
    hexs(d, 32, h);
    memcpy(out, body, bl);
    memcpy(out + bl, "end ", 4);
    memcpy(out + bl + 4, h, 64);
    out[bl + 68] = '\n';
    return bl + 69;
}

static int refuses_with(const char *body, size_t bl, const char *needle)
{
    char rec[2048], why[128] = "";
    StHoldoutCommitment c;
    size_t n = reseal(body, bl, rec);
    int r = st_holdout_parse(rec, n, &c, why, sizeof why);
    if (r >= 0 || !strstr(why, needle)) {
        printf("  (r=%d why='%s', wanted '%s')\n", r, why, needle);
        return 0;
    }
    return 1;
}
#define REFUSE(name, body, needle) \
    check(name, refuses_with(body, sizeof(body) - 1, needle))

static int write_file(const char *path, const void *d, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(d, 1, n, f) == n;
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static int file_equals(const char *path, const char *d, size_t n)
{
    char buf[2048];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t got = fread(buf, 1, sizeof buf, f);
    fclose(f);
    return got == n && memcmp(buf, d, n) == 0;
}


static int cli(int argc, ...)
{
    char *argv[8];
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++)
        argv[i] = va_arg(ap, char *);
    va_end(ap);
    argv[argc] = NULL;
    fflush(stdout);
    return st_holdout_cli(argc, argv);
}

int main(void)
{
    uint8_t salt[32];
    char rec[1024], rec2[1024], why[128];
    size_t rl = 0, rl2 = 0;
    StHoldoutCommitment c;
    const uint8_t *ts = (const uint8_t *)KAT_TASKSET;
    size_t tsl = sizeof(KAT_TASKSET) - 1;
    kat_salt(salt);
    memset(&c, 0, sizeof c);

    /* --- commit / KAT / determinism --- */
    int r = st_holdout_commit("g3-kat-1", ts, tsl, salt, rec, sizeof rec, &rl);
    check("commit ok", r == 0);
    check("kat record bytes", r == 0 && rl == sizeof(KAT_RECORD) - 1 &&
                                  memcmp(rec, KAT_RECORD, rl) == 0);
    r = st_holdout_commit("g3-kat-1", ts, tsl, salt, rec2, sizeof rec2, &rl2);
    check("deterministic bytes", r == 0 && rl == rl2 && !memcmp(rec, rec2, rl));
    check("commit refuses zero tasks",
          st_holdout_commit("x", (const uint8_t *)"hello\n ask\n", 11, salt,
                            rec2, sizeof rec2, &rl2) == ST_HOLDOUT_ENOTASKS);
    check("commit refuses empty taskset",
          st_holdout_commit("x", ts, 0, salt, rec2, sizeof rec2, &rl2) ==
              ST_HOLDOUT_ENOTASKS);
    check("commit refuses bad label",
          st_holdout_commit("a b", ts, tsl, salt, rec2, sizeof rec2, &rl2) ==
              ST_HOLDOUT_ELABEL);
    check("commit refuses 65-char label",
          st_holdout_commit("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                            "aaaaaaaaaa", ts, tsl, salt, rec2, sizeof rec2,
                            &rl2) == ST_HOLDOUT_ELABEL);
    check("commit refuses small buffer",
          st_holdout_commit("g3-kat-1", ts, tsl, salt, rec2, rl - 1, &rl2) ==
              ST_HOLDOUT_ECAP);
    check("task count rule",
          st_holdout_count_tasks((const uint8_t *)"task a\n\ntask\n task b\n"
                                 "task c", sizeof("task a\n\ntask\n task b\ntask c") - 1) == 2);

    /* --- parse --- */
    r = st_holdout_parse(rec, rl, &c, why, sizeof why);
    char h[65];
    hexs(c.record_digest, 32, h);
    check("parse ok + kat record digest", r == 0 && !strcmp(h, KAT_RD) &&
                                              c.task_count == 2 &&
                                              c.taskset_len == 21 &&
                                              !strcmp(c.holdout_id, "g3-kat-1"));
    hexs(c.commitment, 32, h);
    check("kat commitment hex", r == 0 && !strcmp(h, KAT_CM));

    int all = 1;
    for (size_t n = 0; n < rl; n++)
        if (st_holdout_parse(rec, n, &c, why, sizeof why) >= 0)
            all = 0;
    check("truncation at every length refused", all);
    all = 1;
    for (size_t i = 0; i < rl; i++) {
        memcpy(rec2, rec, rl);
        rec2[i] = (char)(rec2[i] ^ 0x01);
        if (st_holdout_parse(rec2, rl, &c, why, sizeof why) >= 0)
            all = 0;
    }
    check("single-byte mutation at every offset refused", all);

    REFUSE("wrong version", "OMEGA-G3-HOLDOUT-COMMIT v2\n" L_DOM L_ID L_TC
                            L_TL L_SD L_CM, "magic");
    REFUSE("wrong magic", "OMEGA-G4-HOLDOUT-COMMIT v1\n" L_DOM L_ID L_TC
                          L_TL L_SD L_CM, "magic");
    REFUSE("wrong domain", L_MAGIC "domain omega.g3.holdout.commit.v2\n" L_ID
                           L_TC L_TL L_SD L_CM, "domain");
    REFUSE("missing field", L_MAGIC L_DOM L_ID L_TC L_SD L_CM, "taskset_len");
    REFUSE("extra field", L_MAGIC L_DOM L_ID "note x\n" L_TC L_TL L_SD L_CM,
           "expected field");
    REFUSE("reordered fields", L_MAGIC L_DOM L_ID L_TL L_TC L_SD L_CM,
           "expected field");
    REFUSE("bad hex", L_MAGIC L_DOM L_ID L_TC L_TL
           "salt_digest g31be661741f6bbf7b8ed2dc8150a6a35238d7b133817bdf1347415bff4ec4a4\n"
           L_CM, "bad hex");
    REFUSE("short hex", L_MAGIC L_DOM L_ID L_TC L_TL L_SD
           "commitment 7008574e\n", "64");
    REFUSE("uppercase hex", L_MAGIC L_DOM L_ID L_TC L_TL L_SD
           "commitment 7008574E68b49d88b0debe5101eec7823a0f11a99833cb5e6aedc9262822b20d\n",
           "uppercase");
    REFUSE("bad label char", L_MAGIC L_DOM "holdout_id g3/kat\n" L_TC L_TL
           L_SD L_CM, "label");
    REFUSE("empty label", L_MAGIC L_DOM "holdout_id \n" L_TC L_TL L_SD L_CM,
           "label");
    REFUSE("zero tasks", L_MAGIC L_DOM L_ID "task_count 0\n" L_TL L_SD L_CM,
           "zero tasks");
    REFUSE("leading zero decimal", L_MAGIC L_DOM L_ID "task_count 02\n" L_TL
           L_SD L_CM, "decimal");
    REFUSE("task_count overflow", L_MAGIC L_DOM L_ID "task_count 4294967296\n"
           L_TL L_SD L_CM, "range");
    REFUSE("len too small for count", L_MAGIC L_DOM L_ID L_TC
           "taskset_len 9\n" L_SD L_CM, "too small");
    REFUSE("CRLF line endings", "OMEGA-G3-HOLDOUT-COMMIT v1\r\ndomain "
           "omega.g3.holdout.commit.v1\r\n" L_ID L_TC L_TL L_SD L_CM, "CR");

    {
        const char body[] = L_MAGIC L_DOM L_ID L_TC L_TL L_SD L_CM;
        check("missing end line refused",
              st_holdout_parse(body, sizeof body - 1, &c, why, sizeof why) < 0 &&
                  strstr(why, "truncated"));
    }
    memcpy(rec2, rec, rl);
    rec2[rl - 2] = rec2[rl - 2] == '0' ? '1' : '0';
    check("end digest mismatch",
          st_holdout_parse(rec2, rl, &c, why, sizeof why) == ST_HOLDOUT_EDIGEST);
    memcpy(rec2, rec, rl);
    rec2[rl] = '\n';
    check("trailing bytes refused",
          st_holdout_parse(rec2, rl + 1, &c, why, sizeof why) < 0 &&
              strstr(why, "trailing"));
    memcpy(rec2, rec, rl);
    rec2[rl] = 'x';
    check("trailing garbage refused",
          st_holdout_parse(rec2, rl + 1, &c, why, sizeof why) < 0);
    memcpy(rec2, rec, rl);
    rec2[sizeof(L_MAGIC L_DOM) + 12] = '\0';
    check("NUL byte refused",
          st_holdout_parse(rec2, rl, &c, why, sizeof why) < 0 &&
              strstr(why, "NUL"));

    /* --- reveal --- */
    r = st_holdout_parse(rec, rl, &c, why, sizeof why);
    check("reveal round trip PASS",
          r == 0 && st_holdout_reveal(&c, ts, tsl, salt, why, sizeof why) == 0);
    uint8_t bad[32];
    memcpy(bad, salt, 32);
    bad[7] ^= 0x80;
    check("reveal refuses swapped salt byte",
          st_holdout_reveal(&c, ts, tsl, bad, why, sizeof why) ==
              ST_HOLDOUT_ESALT);
    {
        const char x[] = "task alpha\ntask beta\ntask gamma\n";
        check("reveal refuses extra task line",
              st_holdout_reveal(&c, (const uint8_t *)x, sizeof x - 1, salt, why,
                                sizeof why) == ST_HOLDOUT_ELEN);
    }
    {
        const char x[] = "task alpha\nxask beta\n";
        check("reveal refuses count mismatch (same length)",
              st_holdout_reveal(&c, (const uint8_t *)x, sizeof x - 1, salt, why,
                                sizeof why) == ST_HOLDOUT_ECOUNT);
    }
    {
        const char x[] = "task alphb\ntask beta\n";
        check("reveal refuses commitment mismatch",
              st_holdout_reveal(&c, (const uint8_t *)x, sizeof x - 1, salt, why,
                                sizeof why) == ST_HOLDOUT_ECOMMIT);
    }

    /* --- receipt --- */
    const char *gc = "0123456789abcdef0123456789abcdef01234567";
    char rc1[1024], rc2[1024];
    size_t r1 = 0, r2 = 0;
    check("receipt refuses 39-char commit",
          st_holdout_reveal_receipt(&c, "0123456789abcdef0123456789abcdef0123456",
                                    rc1, sizeof rc1, &r1) == ST_HOLDOUT_ECOMMITID);
    check("receipt refuses uppercase commit",
          st_holdout_reveal_receipt(&c, "0123456789ABCDEF0123456789abcdef01234567",
                                    rc1, sizeof rc1, &r1) == ST_HOLDOUT_ECOMMITID);
    check("receipt refuses 41-char commit",
          st_holdout_reveal_receipt(&c, "0123456789abcdef0123456789abcdef012345678",
                                    rc1, sizeof rc1, &r1) == ST_HOLDOUT_ECOMMITID);
    int a = st_holdout_reveal_receipt(&c, gc, rc1, sizeof rc1, &r1);
    int b = st_holdout_reveal_receipt(&c, gc, rc2, sizeof rc2, &r2);
    rc1[r1 < sizeof rc1 ? r1 : 0] = 0;
    check("receipt deterministic + names fields",
          a == 0 && b == 0 && r1 == r2 && !memcmp(rc1, rc2, r1) &&
              strstr(rc1, "verdict PASS\n") &&
              strstr(rc1, "commitment " KAT_CM "\n") &&
              strstr(rc1, "record_digest " KAT_RD "\n") &&
              strstr(rc1, "holdout_id g3-kat-1\n") && strstr(rc1, gc));
    {
        uint8_t d[32];
        char hh[65];
        if (a == 0)
            sha256_hash((const uint8_t *)rc1, r1 - 69, d);
        else
            memset(d, 0, sizeof d);
        hexs(d, 32, hh);
        check("receipt end digest valid",
              a == 0 && !memcmp(rc1 + r1 - 69, "end ", 4) &&
                  !memcmp(rc1 + r1 - 65, hh, 64));
    }

    /* --- CLI --- */
    char dir[] = "/tmp/st_holdout_XXXXXX";
    if (!mkdtemp(dir)) {
        check("mkdtemp", 0);
    } else {
        char tsp[256], sp[256], sp31[256], recp[512], bogus[256];
        snprintf(tsp, sizeof tsp, "%s/taskset.txt", dir);
        snprintf(sp, sizeof sp, "%s/salt.bin", dir);
        snprintf(sp31, sizeof sp31, "%s/salt31.bin", dir);
        snprintf(bogus, sizeof bogus, "%s/bogus.txt", dir);
        snprintf(recp, sizeof recp, "%s/g3-commit-%s.txt", dir, KAT_RD);
        write_file(tsp, KAT_TASKSET, tsl);
        write_file(sp, salt, 32);
        write_file(sp31, salt, 31);
        write_file(bogus, "task alphb\ntask beta\n", tsl);

        check("cli usage: unknown subcommand", cli(1, "frob") == 1);
        check("cli usage: wrong argc", cli(2, "commit", "x") == 1);
        check("cli commit refuses 31-byte salt",
              cli(5, "commit", "g3-kat-1", tsp, sp31, dir) == 2);
        check("cli commit writes content-addressed file",
              cli(5, "commit", "g3-kat-1", tsp, sp, dir) == 0 &&
                  file_equals(recp, KAT_RECORD, rl));
        check("cli commit idempotent",
              cli(5, "commit", "g3-kat-1", tsp, sp, dir) == 0);
        check("cli verify ok", cli(2, "verify", recp) == 0);
        unsetenv("OMEGA_REPO_COMMIT");
        check("cli reveal refuses unset OMEGA_REPO_COMMIT",
              cli(5, "reveal", recp, tsp, sp, dir) == 2);
        setenv("OMEGA_REPO_COMMIT", "XYZ", 1);
        check("cli reveal refuses bad OMEGA_REPO_COMMIT",
              cli(5, "reveal", recp, tsp, sp, dir) == 2);
        setenv("OMEGA_REPO_COMMIT", gc, 1);
        check("cli reveal refuses wrong taskset",
              cli(5, "reveal", recp, bogus, sp, dir) == 2);
        check("cli reveal PASS", cli(5, "reveal", recp, tsp, sp, dir) == 0);
        {
            char rp[512];
            uint8_t d[32];
            char hh[65];
            sha256_hash((const uint8_t *)rc1, r1, d);
            hexs(d, 32, hh);
            snprintf(rp, sizeof rp, "%s/g3-reveal-%s.txt", dir, hh);
            check("cli reveal receipt content-addressed",
                  file_equals(rp, rc1, r1));
        }
        write_file(recp, KAT_RECORD "x", rl + 1);
        check("cli verify refuses corrupted record", cli(2, "verify", recp) == 2);
        check("cli commit refuses existing different bytes",
              cli(5, "commit", "g3-kat-1", tsp, sp, dir) == 2);

        DIR *dd = opendir(dir);
        struct dirent *e;
        while (dd && (e = readdir(dd))) {
            char p[512];
            if (e->d_name[0] == '.')
                continue;
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        if (dd)
            closedir(dd);
        rmdir(dir);
    }

    printf("st_holdout: %d/%d PASS\n", g_pass, g_run);
    return g_pass == g_run ? 0 : 1;
}
