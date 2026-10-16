/*
 * famsig.c -- GEN family signatures and the split disjointness rule (aien-spark/omega#476).
 * Contract: tests/one_layer/generalization/families/FAMSIG_V1.md (frozen; this tool is its reference).
 * Deterministic: no clock, no randomness, no environment; output depends only on the arguments and files.
 *
 *   famsig canon <shape>             print "gs1:<canonical form>" of one nesting shape
 *   famsig token <shape>             print the family token "gs1-<16 hex>" (record family field)
 *   famsig edges <shape>             print the composition edges "parent>child", one per line, sorted
 *   famsig meta <file>...            validate gen-meta / gen-split files; one line per task:
 *                                    "<task_id>\t<split>\t<signature>"
 *   famsig rule [--show-heldout] <file>...
 *                                    apply the disjointness rule (R1..R5) over every task of every file;
 *                                    report, then "RULE PASS" (exit 0) or "RULE FAIL" (exit 1). Heldout
 *                                    family signatures are printed only with --show-heldout.
 *   famsig split <gen-split file>    validate a sealed split file; print split_sha256, famlist_sha256,
 *                                    tasks, families (no signature, no task metadata)
 *   famsig famlist <gen-split file>  print the FAMLIST bytes whose sha256 is famlist_sha256
 * Exit: 0 ok, 1 refused (reason on stderr) or rule failure, 2 usage.
 */
#include "famsig_lib.h"

static int usage(void)
{
    fprintf(stderr, "usage: famsig canon <shape> | token <shape> | edges <shape> | meta <file>... | rule [--show-heldout] <file>... | split <SPLIT> | famlist <SPLIT>\n");
    return 2;
}

static int read_all(int argc, char **argv, int from, FsSet *set)
{
    char err[512];
    for (int i = from; i < argc; i++)
        if (fs_read(argv[i], set, err, sizeof err)) { fprintf(stderr, "famsig: %s\n", err); return -1; }
    return 0;
}

static int read_split(const char *path, FsSet *set)
{
    char err[512];
    if (fs_read(path, set, err, sizeof err)) { fprintf(stderr, "famsig: %s\n", err); return -1; }
    if (!set->is_split) { fprintf(stderr, "famsig: %s: not a gen-split file\n", path); return -1; }
    if (set->n == 0) { fprintf(stderr, "famsig: %s: no task lines\n", path); return -1; }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return usage();
    const char *cmd = argv[1];
    if (!strcmp(cmd, "canon") || !strcmp(cmd, "edges") || !strcmp(cmd, "token")) {
        if (argc != 3) return usage();
        FsShape sh;
        if (fs_shape(argv[2], &sh)) { fprintf(stderr, "famsig: shape: %s\n", sh.err); return 1; }
        if (cmd[0] == 'c') { printf("gs1:%s\n", sh.canon); return 0; }
        if (cmd[0] == 't') { char sig[FS_SIGMAX + 4], tok[24]; snprintf(sig, sizeof sig, "gs1:%s", sh.canon); fs_token(sig, tok); printf("%s\n", tok); return 0; }
        char e[64][16];
        int ne = fs_edges(&sh, e, 64);
        for (int i = 0; i < ne; i++) printf("%s\n", e[i]);
        return 0;
    }
    if (!strcmp(cmd, "meta")) {
        if (argc < 3) return usage();
        FsSet set = {0};
        if (read_all(argc, argv, 2, &set)) return 1;
        for (int i = 0; i < set.n; i++) printf("%s\t%s\t%s\n", set.m[i].id, fs_split[set.m[i].split], set.m[i].sig);
        return 0;
    }
    if (!strcmp(cmd, "rule")) {
        int show = 0, from = 2;
        if (argc > 2 && !strcmp(argv[2], "--show-heldout")) { show = 1; from = 3; }
        if (argc <= from) return usage();
        FsSet set = {0};
        if (read_all(argc, argv, from, &set)) { printf("RULE FAIL\n"); return 1; }
        int f = fs_rule(&set, stdout, show);
        printf(f ? "RULE FAIL\n" : "RULE PASS\n");
        return f ? 1 : 0;
    }
    if (!strcmp(cmd, "split") || !strcmp(cmd, "famlist")) {
        if (argc != 3) return usage();
        FsSet set = {0};
        if (read_split(argv[2], &set)) return 1;
        int nf;
        char *fl = fs_famlist(&set, &nf);
        if (cmd[0] == 'f') { fputs(fl, stdout); return 0; }
        unsigned char d[32];
        char hex[65];
        sha256_hash((const unsigned char *)fl, strlen(fl), d);
        fs_hex(d, hex);
        printf("split_sha256=%s\nfamlist_sha256=%s\ntasks=%d\nfamilies=%d\n", set.split_sha256, hex, set.n, nf);
        free(fl);
        return 0;
    }
    return usage();
}
