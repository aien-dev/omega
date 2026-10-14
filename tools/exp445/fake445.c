/*
 * fake445.c -- tiny local stand-in for an OpenAI-compatible chat endpoint, for the exp445 driver's
 * dry-run tests only (omega#445). Serves POST /v1/chat/completions on 127.0.0.1, one connection at a
 * time, and answers from files:
 *   fake445 <port_file> <answers_dir>
 * binds an ephemeral port and writes it to <port_file>. The answer for a request is the file
 *   <answers_dir>/<arm>/<entry>          first round
 *   <answers_dir>/<arm>/<entry>.repair   when the request already holds an assistant turn
 * where <arm> is taken from "REPRESENTATION: <arm>" in the request and <entry> from the first
 * "\nentry <name>" (JSON-escaped newline). Missing file: "I cannot do this." A file starting with
 * "@@500" gives HTTP 500. Usage: prompt_tokens = request bytes / 4, completion_tokens = answer bytes
 * / 4 + 1; an answer longer than max_tokens * 4 bytes is cut there with finish_reason "length".
 * Every request body is appended to <answers_dir>/requests.log (one line each).
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static char *slurp(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long L = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)L + 1);
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

static void send_all(int fd, const char *b, size_t n)
{
    while (n) { ssize_t w = write(fd, b, n); if (w <= 0) return; b += w; n -= (size_t)w; }
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: fake445 <port_file> <answers_dir>\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 8) || getsockname(ls, (struct sockaddr *)&a, &al)) { perror("fake445"); return 1; }
    char tmp[4096];
    snprintf(tmp, sizeof tmp, "%s.tmp", argv[1]);
    FILE *pf = fopen(tmp, "w");
    if (!pf) return 1;
    fprintf(pf, "%u\n", ntohs(a.sin_port));
    fclose(pf);
    rename(tmp, argv[1]);
    for (;;) {
        int c = accept(ls, NULL, NULL);
        if (c < 0) continue;
        size_t cap = 1 << 20, len = 0;
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
        char logp[4096];
        snprintf(logp, sizeof logp, "%s/requests.log", argv[2]);
        FILE *lf = fopen(logp, "a");
        if (lf) { fprintf(lf, "%s\n", body); fclose(lf); }
        char arm[32], entry[80], mt[32];
        word_after(body, "REPRESENTATION: ", arm, sizeof arm);
        word_after(body, "\\nentry ", entry, sizeof entry);
        word_after(body, "\"max_tokens\":", mt, sizeof mt);
        long maxt = mt[0] ? atol(mt) : 1 << 30;
        int repair = strstr(body, "\"role\":\"assistant\"") || strstr(body, "\"role\": \"assistant\"");
        char ap[4096];
        snprintf(ap, sizeof ap, "%s/%s/%s%s", argv[2], arm, entry, repair ? ".repair" : "");
        size_t an = 0;
        char *ans = slurp(ap, &an);
        if (!ans) { ans = strdup("I cannot do this."); an = strlen(ans); }
        if (!strncmp(ans, "@@500", 5)) {
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
            j += (size_t)snprintf(js + j, jcap - j, "\"}}],\"usage\":{\"prompt_tokens\":%ld,\"completion_tokens\":%zu,\"total_tokens\":%zu}}\n",
                                  clen / 4, an / 4 + 1, (size_t)(clen / 4) + an / 4 + 1);
            char hd[256];
            int hn = snprintf(hd, sizeof hd, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", j);
            send_all(c, hd, (size_t)hn);
            send_all(c, js, j);
            free(js);
        }
        free(ans);
        free(buf);
        close(c);
    }
}
