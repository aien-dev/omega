/*
 * fakegen.c -- local stand-in for an OpenAI-compatible chat endpoint, for the GEN runner's CPU-only tests
 * (omega#477, GEN-2). Modelled on tools/exp445/fake445.c (same request parsing, usage and aien_sampling
 * record); adds the GEN conditions, seed-dependent answers, broken seeding, timeouts and one-off 500s.
 * Never used with a real model. Serves 127.0.0.1 only, one connection at a time:
 *   fakegen <port_file> <answers_dir>
 * binds an ephemeral port and writes it to <port_file>.
 *
 * Condition of a request, from the system text the GEN runner builds (FORMATS.md section 3):
 *   D  "Retrieved records: verified helper functions"      (genret abstraction suffix)
 *   C  "Retrieved records: verified solutions to earlier"  (genret raw suffix)
 *   B  "Worked examples ("                                  (the many-shot prefix, no suffix)
 *   A  anything else
 * Task entry: the last "\nentry <name>" (JSON-escaped newline), as fake445. Answer file, first found:
 *   <answers>/<cond>/<entry><R>.seed<S>   <answers>/<cond>/<entry><R>   <answers>/any/<entry><R>.seed<S>
 *   <answers>/any/<entry><R>
 * with <R> = ".repair" when the request holds an assistant turn, else empty; .seed<S> only when the
 * request has "seed": S. Missing: "I cannot do this.". Directives at the start of an answer file:
 *   @@500                 HTTP 500 every time
 *   @@500ONCE\n<rest>     HTTP 500 the first time this file is used, then <rest>
 *   @@SLEEP <s>\n<rest>   sleep <s> seconds, then <rest> (the client's timeout fires first in tests)
 *   @@SLEEPONCE <s>\n<rest>  the sleep only the first time
 * <answers>/sampling.json (optional), as fake445 plus GEN knobs:
 *   "p_repeat": x             reported p_repeat (default 0.5)
 *   "ignore_seed": true       report seed 0 (broken seeding)
 *   "force_greedy": true      report mode greedy for sampled requests (broken sampling)
 *   "vary_untasked": true     number the answers to prompts with no task entry (sampling-gate prompts)
 *   "seed_salt": true         sampled replies get "\n\nseed <S>\n" appended after the answer (outside
 *                             any fenced block): honest seed-dependent output, same program
 *   "seed_salt_broken": true  the salt ignores the seed (every seed gives the same text)
 *   "nondet_greedy": true     greedy replies get a running counter appended (greedy not deterministic)
 * Usage: prompt_tokens = body bytes / 4, completion_tokens = answer bytes / 4 + 1; an answer longer than
 * max_tokens * 4 bytes is cut with finish_reason "length". Every POST body is appended to
 * <answers>/requests.log (one line each). GET .../models answers <answers>/models.json.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char *slurp(const char *p, size_t *n)
{
    struct stat st;
    if (stat(p, &st) || !S_ISREG(st.st_mode)) return NULL;
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = L < 0 ? NULL : malloc((size_t)L + 1);
    if (!b) { fclose(f); return NULL; }
    *n = fread(b, 1, (size_t)L, f);
    b[*n] = 0;
    fclose(f);
    return b;
}

static void word_after(const char *body, const char *key, char *out, size_t n)
{
    out[0] = 0;
    const char *p = strstr(body, key);
    if (!p) return;
    p += strlen(key);
    size_t k = 0;
    while (k + 1 < n && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) out[k++] = *p++;
    out[k] = 0;
}

static double num_after(const char *s, const char *key, double dflt)
{
    const char *p = s ? strstr(s, key) : NULL;
    if (!p) return dflt;
    p += strlen(key);
    while (*p == ' ') p++;
    char *e;
    double v = strtod(p, &e);
    return e == p ? dflt : v;
}

static void send_all(int fd, const char *b, size_t n)
{
    while (n) { ssize_t w = write(fd, b, n); if (w <= 0) return; b += w; n -= (size_t)w; }
}

/* once: 1 the first time marker m is asked for (and records it), 0 afterwards */
static int once(const char *m)
{
    if (access(m, F_OK) == 0) return 0;
    FILE *f = fopen(m, "w");
    if (f) fclose(f);
    return 1;
}

/* marker path for an answer file: <dir>/once.<file path with '/' as '_'> */
static void marker(char *out, size_t n, const char *dir, const char *path)
{
    int k = snprintf(out, n, "%s/once.", dir);
    for (const char *p = path + strlen(dir); *p && (size_t)k + 1 < n; p++) out[k++] = *p == '/' ? '_' : *p;
    out[k] = 0;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: fakegen <port_file> <answers_dir>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    const char *dir = argv[2];
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 16) || getsockname(ls, (struct sockaddr *)&a, &al)) { perror("fakegen"); return 1; }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[1]);
    FILE *pf = fopen(tmp, "w");
    if (!pf) return 1;
    fprintf(pf, "%u\n", ntohs(a.sin_port));
    fclose(pf);
    rename(tmp, argv[1]);
    unsigned long untasked = 0, greedy_n = 0;
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        size_t cap = 4 << 20, len = 0;
        char *buf = malloc(cap + 1);
        long clen = -1;
        char *body = NULL;
        while (len < cap) {
            ssize_t r = read(c, buf + len, cap - len);
            if (r <= 0) break;
            len += (size_t)r;
            buf[len] = 0;
            char *h = strstr(buf, "\r\n\r\n");
            if (h && clen < 0) {
                char *cl = strcasestr(buf, "Content-Length:");
                clen = cl && cl < h ? atol(cl + 15) : 0;
                body = h + 4;
            }
            if (body && (long)(len - (size_t)(body - buf)) >= clen) break;
        }
        if (!body || clen < 0 || (size_t)(body - buf) + (size_t)clen > len) { close(c); free(buf); continue; }
        body[clen] = 0;
        if (!strncmp(buf, "GET ", 4)) {
            char mp[4096];
            snprintf(mp, sizeof mp, "%s/models.json", dir);
            size_t mn = 0;
            char *mj = slurp(mp, &mn);
            if (!mj) { mj = strdup("{\"object\":\"list\",\"data\":[]}"); mn = strlen(mj); }
            char hd[256];
            int hn = snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", mn);
            send_all(c, hd, (size_t)hn);
            send_all(c, mj, mn);
            free(mj); free(buf); close(c);
            continue;
        }
        char logp[4096];
        snprintf(logp, sizeof logp, "%s/requests.log", dir);
        FILE *lf = fopen(logp, "a");
        if (lf) { fprintf(lf, "%s\n", body); fclose(lf); }
        const char *cond = strstr(body, "Retrieved records: verified helper functions") ? "D"
                         : strstr(body, "Retrieved records: verified solutions to earlier") ? "C"
                         : strstr(body, "Worked examples (") ? "B" : "A";
        char entry[80], mt[32], seed[32];
        {
            const char *p = body, *q, *last = NULL;
            while ((q = strstr(p, "\\nentry "))) { last = q; p = q + 1; }
            entry[0] = 0;
            if (last) word_after(last, "\\nentry ", entry, sizeof entry);
        }
        word_after(body, "\"max_tokens\":", mt, sizeof mt);
        word_after(body, "\"seed\":", seed, sizeof seed);
        long maxt = mt[0] ? atol(mt) : 1 << 30;
        int repair = strstr(body, "\"role\":\"assistant\"") || strstr(body, "\"role\": \"assistant\"");
        char sjp[4096];
        snprintf(sjp, sizeof sjp, "%s/sampling.json", dir);
        size_t sjn = 0;
        char *sj = slurp(sjp, &sjn);
        double temp = num_after(body, "\"temperature\":", 1.0), prep = num_after(sj, "\"p_repeat\":", 0.5);
        int greedy_req = temp == 0.0;
        int greedy = greedy_req || (sj && strstr(sj, "\"force_greedy\": true"));
        long sd = (sj && strstr(sj, "\"ignore_seed\": true")) ? 0 : (long)num_after(body, "\"seed\":", 0);

        char ap[4200] = "";
        size_t an = 0;
        char *ans = NULL;
        const char *cands[2] = {cond, "any"};
        for (int i = 0; i < 2 && !ans && entry[0]; i++) {
            if (seed[0]) { snprintf(ap, sizeof ap, "%s/%s/%s%s.seed%s", dir, cands[i], entry, repair ? ".repair" : "", seed); ans = slurp(ap, &an); }
            if (!ans) { snprintf(ap, sizeof ap, "%s/%s/%s%s", dir, cands[i], entry, repair ? ".repair" : ""); ans = slurp(ap, &an); }
        }
        if (!ans) { ans = strdup("I cannot do this."); an = strlen(ans); ap[0] = 0; }
        int fail500 = 0;
        for (;;) { /* directives, possibly stacked */
            char mk[4400];
            if (!strncmp(ans, "@@500ONCE\n", 10)) {
                marker(mk, sizeof mk, dir, ap[0] ? ap : dir);
                strcat(mk, ".500");
                if (once(mk)) { fail500 = 1; break; }
                memmove(ans, ans + 10, an - 10 + 1); an -= 10; continue;
            }
            if (!strncmp(ans, "@@500", 5)) { fail500 = 1; break; }
            int so = !strncmp(ans, "@@SLEEPONCE ", 12);
            if (so || !strncmp(ans, "@@SLEEP ", 8)) {
                char *nl = strchr(ans, '\n');
                unsigned s = (unsigned)atoi(ans + (so ? 12 : 8));
                size_t cut = nl ? (size_t)(nl - ans) + 1 : an;
                int doit = 1;
                if (so) { marker(mk, sizeof mk, dir, ap[0] ? ap : dir); strcat(mk, ".sleep"); doit = once(mk); }
                if (doit) sleep(s);
                memmove(ans, ans + cut, an - cut + 1); an -= cut; continue;
            }
            break;
        }
        if (!fail500) {
            char extra[96] = "";
            if (!entry[0] && sj && strstr(sj, "\"vary_untasked\": true")) snprintf(extra, sizeof extra, " #%lu", ++untasked);
            else if (!greedy_req && sj && strstr(sj, "\"seed_salt_broken\": true")) snprintf(extra, sizeof extra, "\n\nseed salt\n");
            else if (!greedy_req && sj && strstr(sj, "\"seed_salt\": true")) snprintf(extra, sizeof extra, "\n\nseed %s\n", seed[0] ? seed : "none");
            else if (greedy_req && sj && strstr(sj, "\"nondet_greedy\": true")) snprintf(extra, sizeof extra, "\n\nrun %lu\n", ++greedy_n);
            if (extra[0]) {
                char *nb = malloc(an + sizeof extra);
                memcpy(nb, ans, an);
                memcpy(nb + an, extra, strlen(extra) + 1);
                an += strlen(extra);
                free(ans);
                ans = nb;
            }
        }
        if (fail500) {
            const char *e = "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 27\r\nConnection: close\r\n\r\n{\"error\":\"fake 500 error\"}\n";
            send_all(c, e, strlen(e));
        } else {
            const char *fin = "stop";
            if ((long)an > maxt * 4) { an = (size_t)maxt * 4; fin = "length"; }
            size_t jcap = an * 6 + 1024, j = 0;
            char *js = malloc(jcap);
            j += (size_t)snprintf(js + j, jcap - j, "{\"id\":\"fake\",\"object\":\"chat.completion\",\"model\":\"fake\",\"choices\":[{\"index\":0,\"finish_reason\":\"%s\",\"message\":{\"role\":\"assistant\",\"content\":\"", fin);
            for (size_t i = 0; i < an; i++) {
                unsigned char ch = (unsigned char)ans[i];
                if (ch == '"' || ch == '\\') { js[j++] = '\\'; js[j++] = (char)ch; }
                else if (ch == '\n') { js[j++] = '\\'; js[j++] = 'n'; }
                else if (ch < 0x20) j += (size_t)snprintf(js + j, jcap - j, "\\u%04x", ch);
                else js[j++] = (char)ch;
            }
            j += (size_t)snprintf(js + j, jcap - j, "\"}}],\"usage\":{\"prompt_tokens\":%ld,\"completion_tokens\":%zu,\"total_tokens\":%zu,"
                                  "\"aien_sampling\":{\"mode\":\"%s\",\"seed\":%ld,\"p_repeat\":%g,\"kept\":[1,2],\"sampled_tokens\":%zu}}}\n",
                                  clen / 4, an / 4 + 1, (size_t)(clen / 4) + an / 4 + 1, greedy ? "greedy" : "sampled", sd, prep, an / 4 + 1);
            char hd[256];
            int hn = snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", j);
            send_all(c, hd, (size_t)hn);
            send_all(c, js, j);
            free(js);
        }
        free(sj); free(ans); free(buf);
        close(c);
    }
}
