/*
 * osh_trace.c -- core-only step trace of the Omega-native shell (OSH-AIENOS-0 differential fixture).
 * Mirrors the sequencing of src/osh/host/osh_shell.c in script-file mode (run_src / lex_parse / run_list) over
 * osh_core_call, but executes nothing: on PIPELINE_READY it only records the request and takes the next fake status;
 * NEED_VAR is answered from a fixed variable table and the fixture's positionals.
 * usage: osh_trace interp|native lex.osc parse.osc expand.osc script
 * Per fixture, optional:  <script minus .sh>.status  whitespace-separated pipeline statuses, used in order (then 0)
 *                         <script minus .sh>.args    positional parameters $1.., one per line; $0 is "osh_trace"
 * Output, one line per unit call:   step <n> unit=<lex|parse|expand> in_len=<n> ret=<u64> ws_sha256=<hex of WS_CELLS*8 bytes LE>
 * and on each pipeline request:     request <n> sha256=<hex of the request record cells>
 * and for driver-level events:      event <text>; the last line is always  event exit=<status>  (2 after a refusal)
 * Not modelled (nothing runs): the exit builtin, exit with too many arguments dropping the list, ^C, -c exact end of
 * input. INPUT_MAX models the AIENOS read-only input window (4096 bytes): a list buffer that would exceed it is
 * refused, so above 4096 bytes this is not a reference for the Linux shell (its buffer is 1 MiB). Native mode needs
 * an AArch64 host, as every osh test does.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osh_core.h"
#include "osh_host.h"
#include "osh_layout.h"
#include "sha256.h"

#define WS ((size_t)OSH_WS_CELLS)
#define INPUT_MAX 4096u
enum { ST_MORE = 100, ST_BUDGET = 101, ST_NEED_VAR = 102, ST_PIPE = 103, ST_COMPLETE = 104 };

static OshCore core;
static int native_mode;
static uint64_t w[OSH_WS_CELLS];
static uint8_t buf[INPUT_MAX];
static size_t blen;
static unsigned long step_no, req_no;
static const char *src_text;
static size_t src_len, src_pos;

static void hex(const uint8_t *p, size_t n, char *o)
{
    uint8_t d[SHA256_DIGEST_SIZE];
    sha256_hash(p, n, d);
    for (int i = 0; i < SHA256_DIGEST_SIZE; i++) sprintf(o + 2 * i, "%02x", d[i]);
}

static uint64_t call(int unit)
{
    static const char *const nm[] = {"lex", "parse", "expand"};
    uint64_t r = osh_core_call(&core, unit, native_mode, buf, blen, w, WS);
    char h[65];
    hex((const uint8_t *)w, WS * 8, h);
    printf("step %lu unit=%s in_len=%zu ret=%llu ws_sha256=%s\n", ++step_no, nm[unit], blen, (unsigned long long)r, h);
    return r;
}

static void reset_list(void)
{
    memset(w, 0, sizeof w);
    w[OSH_S_MAGIC] = OSH_ABI_MAGIC;
    w[OSH_S_VERSION] = OSH_ABI_VERSION;
    w[OSH_S_WS_CELLS] = OSH_WS_CELLS;
    blen = 0;
}

static uint64_t run_entry(int unit)
{
    for (int i = 0; i < 100000; i++) {
        uint64_t st = call(unit);
        if (st != ST_BUDGET) return st;
    }
    return OSH_CORE_FAULT;
}

static uint64_t lex_parse(void)
{
    uint64_t st = run_entry(OSH_U_LEX);
    if (st != 0) return st;
    return run_entry(OSH_U_PARSE);
}

static int append(const uint8_t *p, size_t n)
{
    if (blen + n > INPUT_MAX) {
        printf("event input_window_exceeded have=%zu add=%zu max=%u\n", blen, n, INPUT_MAX);
        return -1;
    }
    memcpy(buf + blen, p, n);
    blen += n;
    return 0;
}

static int read_line(void)
{
    if (src_pos >= src_len) return 0;
    const char *s = src_text + src_pos;
    size_t n = 0;
    while (src_pos + n < src_len && s[n] != '\n') n++;
    int nl = src_pos + n < src_len;
    if (append((const uint8_t *)s, n) != 0) return -1;
    if (append((const uint8_t *)"\n", 1) != 0) return -1;
    src_pos += n + (size_t)nl;
    return 1;
}

static size_t dec(uint64_t v, uint8_t *o)
{
    uint8_t t[24];
    size_t n = 0;
    do { t[n++] = (uint8_t)('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; i++) o[i] = t[n - 1 - i];
    return n;
}

static const char *var_get(const char *name)
{
    if (!strcmp(name, "HOME")) return "/home/t";
    if (!strcmp(name, "X")) return "a b";
    if (!strcmp(name, "EMPTY")) return "";
    return NULL;
}

#define MAX_FAKE 256
#define MAX_POS 9
static int last_status;             /* the shell's $?: fake pipeline statuses, 2 after a refusal */
static int fake[MAX_FAKE], nfake, fake_i;
static char *pos[MAX_POS];
static int npos;
static const char ARG0[] = "osh_trace";

static int next_fake(void) { return fake_i < nfake ? fake[fake_i++] : 0; }

static void answer(void)
{
    uint64_t kind = w[OSH_S_VR_KIND], a = w[OSH_S_VR_A], len = w[OSH_S_VR_LEN];
    const uint8_t *val = NULL;
    uint8_t num[24];
    size_t vl = 0;
    int found = 0;
    switch (kind) {
    case 1: {
        char name[256];
        if (a <= blen && len < sizeof name && len <= blen - a) {
            memcpy(name, buf + a, (size_t)len);
            name[len] = 0;
            const char *v = var_get(name);
            if (v) { found = 1; val = (const uint8_t *)v; vl = strlen(v); }
        }
        break;
    }
    case 2: found = 1; vl = dec((uint64_t)(unsigned)last_status, num); val = num; break;
    case 3: /* as osh_shell.c answer() */
        if (a == 0) { found = 1; val = (const uint8_t *)ARG0; vl = strlen(ARG0); }
        else if (a >= 1 && a <= (uint64_t)npos) { found = 1; val = (const uint8_t *)pos[a - 1]; vl = strlen(pos[a - 1]); }
        break;
    case 4: found = 1; vl = dec((uint64_t)npos, num); val = num; break;
    default: break;
    }
    w[OSH_STAGING + 0] = (uint64_t)found;
    w[OSH_STAGING + 1] = vl;
    w[OSH_STAGING + 2] = (uint64_t)npos;
    w[OSH_STAGING + 3] = 0;
    for (size_t i = 0; i < vl && i < 1024; i++) w[OSH_STAGING + 4 + i] = val[i];
}

/* returns 1 when the driver must stop */
static int run_list(void)
{
    w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)last_status;
    for (int guard = 0; guard < 1000000; guard++) {
        uint64_t st = call(OSH_U_EXPAND);
        switch (st) {
        case ST_BUDGET: continue;
        case ST_NEED_VAR: answer(); continue;
        case ST_PIPE: {
            uint64_t nc = w[OSH_REQUEST + 1];
            if (nc > (OSH_OUT - OSH_REQUEST - OSH_REQ_HDR_CELLS) / OSH_CMD_CELLS) { /* never read past REQUEST */
                printf("event request_ncmds_out_of_range ncmds=%llu\n", (unsigned long long)nc);
                last_status = 2;
                return 1;
            }
            size_t cells = (size_t)(OSH_REQ_HDR_CELLS + nc * OSH_CMD_CELLS);
            char h[65];
            hex((const uint8_t *)(w + OSH_REQUEST), cells * 8, h);
            printf("request %lu sha256=%s\n", ++req_no, h);
            last_status = next_fake(); /* nothing ran: the fixture's next status */
            w[OSH_S_LAST_STATUS] = (uint64_t)(unsigned)last_status;
            continue;
        }
        case ST_COMPLETE: return 0;
        default: last_status = 2; return 1; /* refused or fault: non-interactive shell stops with 2 */
        }
    }
    return 1;
}

static int run_src(void)
{
    for (;;) {
        reset_list();
        uint64_t st;
        int eof = 0;
        for (;;) {
            int r = read_line();
            if (r < 0) { last_status = 2; return 2; }
            if (r == 0) {
                eof = 1;
                if (blen == 0) return 0;
                w[OSH_S_EOI] = 1;
            }
            st = lex_parse();
            if (st == 0 && w[OSH_PX_STATE] != 4) st = eof ? ST_COMPLETE : ST_MORE;
            if (st == ST_MORE && !eof) continue;
            break;
        }
        if (st == ST_COMPLETE) {
        } else if (st == 0) {
            if (run_list()) return 1;
        } else {
            last_status = 2; /* lexer or parser refusal */
            return 2;
        }
        if (eof) return 0;
    }
}

/* read a whole file; a missing optional file gives NULL */
static char *slurp(const char *path, size_t *n, int optional)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (optional) return NULL;
        fprintf(stderr, "osh_trace: cannot open %s\n", path);
        exit(2);
    }
    size_t cap = 1 << 16, len = 0;
    char *b = malloc(cap + 1);
    size_t r;
    if (!b) { fprintf(stderr, "osh_trace: out of memory\n"); exit(2); }
    while ((r = fread(b + len, 1, cap - len, f)) > 0) {
        len += r;
        if (len == cap) {
            char *nb = realloc(b, (cap *= 2) + 1);
            if (!nb) { fprintf(stderr, "osh_trace: out of memory\n"); exit(2); }
            b = nb;
        }
    }
    if (ferror(f)) { fprintf(stderr, "osh_trace: cannot read %s\n", path); exit(2); }
    fclose(f);
    b[len] = 0;
    *n = len;
    return b;
}

/* <script minus .sh> + ext, or exit when the script name does not end in .sh */
static void sidecar(const char *script, const char *ext, char *out, size_t cap)
{
    size_t l = strlen(script);
    if (l < 3 || strcmp(script + l - 3, ".sh") != 0 || l - 3 + strlen(ext) + 1 > cap) {
        fprintf(stderr, "osh_trace: script must end in .sh: %s\n", script);
        exit(2);
    }
    memcpy(out, script, l - 3);
    strcpy(out + l - 3, ext);
}

static void load_fixture(const char *script)
{
    char p[4096];
    size_t n;
    sidecar(script, ".status", p, sizeof p);
    char *s = slurp(p, &n, 1);
    if (s) {
        char *e;
        for (char *q = s; nfake < MAX_FAKE;) {
            while (*q == ' ' || *q == '\t' || *q == '\n') q++;
            if (!*q) break;
            long v = strtol(q, &e, 10);
            if (e == q || v < 0 || v > 255) { fprintf(stderr, "osh_trace: bad status in %s\n", p); exit(2); }
            fake[nfake++] = (int)v;
            q = e;
        }
    }
    sidecar(script, ".args", p, sizeof p);
    s = slurp(p, &n, 1);
    if (s) {
        for (char *q = s; *q && npos < MAX_POS;) {
            char *nl = strchr(q, '\n');
            if (nl) *nl = 0;
            pos[npos++] = q;
            if (!nl) break;
            q = nl + 1;
        }
    }
}

int main(int argc, char **argv)
{
    if (argc != 6) { fprintf(stderr, "usage: osh_trace interp|native lex.osc parse.osc expand.osc script\n"); return 2; }
    {
        uint16_t one = 1;
        if (*(uint8_t *)&one != 1) { fprintf(stderr, "osh_trace: needs a little-endian host\n"); return 2; }
    }
    native_mode = strcmp(argv[1], "native") == 0;
    const char *srcs[3];
    size_t lens[3];
    for (int i = 0; i < 3; i++) srcs[i] = slurp(argv[2 + i], &lens[i], 0);
    char err[300];
    if (osh_core_init(&core, srcs, lens, err, sizeof err) != 0) { fprintf(stderr, "osh_trace: %s\n", err); return 2; }
    src_text = slurp(argv[5], &src_len, 0);
    load_fixture(argv[5]);
    run_src();
    printf("event exit=%d\n", last_status);
    return 0;
}
