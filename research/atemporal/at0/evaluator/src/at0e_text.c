/* Byte rules, tokens and digests for the AT-0 evaluator (AT0_CASE_V1 sections 1, 2, 5). */
#include "at0e.h"
#include "sha256.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int textfile_from_bytes(const uint8_t *b, size_t len, textfile *tf, const char **why) {
    memset(tf, 0, sizeof *tf);
    *why = NULL;
    if (len == 0) { *why = "empty file"; return 1; }
    if (b[len - 1] != '\n') { *why = "last line not LF-terminated"; return 2; }
    size_t nl = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = b[i];
        if (c == '\n') { nl++; continue; }
        if (c < 0x20 || c > 0x7e) { *why = c == '\r' ? "CR byte" : c == '\t' ? "tab byte" : "byte outside 0x20..0x7E"; return 3; }
    }
    tf->bytes = malloc(len); memcpy(tf->bytes, b, len); tf->len = len;
    tf->lines = calloc(nl, sizeof(char *)); tf->n = nl;
    size_t start = 0, li = 0;
    for (size_t i = 0; i < len; i++) {
        if (b[i] != '\n') continue;
        size_t l = i - start;
        if (l == 0) { *why = "blank line"; return 4; }
        if (b[start] == ' ' || b[i - 1] == ' ') { *why = "leading or trailing space"; return 5; }
        for (size_t j = start + 1; j < i; j++) if (b[j] == ' ' && b[j - 1] == ' ') { *why = "double space"; return 6; }
        if (l >= AT0E_MAX_LINE) { *why = "line too long"; return 7; }
        tf->lines[li] = malloc(l + 1); memcpy(tf->lines[li], b + start, l); tf->lines[li][l] = 0;
        li++; start = i + 1;
    }
    return 0;
}
int textfile_read(const char *path, textfile *tf, const char **why) {
    memset(tf, 0, sizeof *tf);
    FILE *f = fopen(path, "rb");
    if (!f) { *why = "cannot open"; return 100; }
    uint8_t *buf = NULL; size_t cap = 0, len = 0;
    for (;;) {
        if (len == cap) { cap = cap ? cap * 2 : 65536; buf = realloc(buf, cap); }
        size_t got = fread(buf + len, 1, cap - len, f);
        if (got == 0) break;
        len += got;
        if (len > (1u << 26)) { fclose(f); free(buf); *why = "file too large"; return 101; }
    }
    fclose(f);
    int rc = textfile_from_bytes(buf, len, tf, why);
    free(buf);
    return rc;
}
void textfile_free(textfile *tf) {
    if (tf->lines) { for (size_t i = 0; i < tf->n; i++) free(tf->lines[i]); free(tf->lines); }
    free(tf->bytes);
    memset(tf, 0, sizeof *tf);
}

/* Split on single spaces. The byte rules already forbid double/leading/trailing spaces. */
int split_tokens(const char *line, char **tok, int max) {
    static char buf[AT0E_MAX_LINE];
    size_t l = strlen(line);
    if (l >= sizeof buf) return -1;
    memcpy(buf, line, l + 1);
    int n = 0; char *p = buf;
    while (*p) {
        if (n >= max) return -1;
        tok[n++] = p;
        char *sp = strchr(p, ' ');
        if (!sp) break;
        *sp = 0; p = sp + 1;
        if (*p == 0) return -1;
    }
    return n;
}

static int in(char c, const char *set) { return strchr(set, c) != NULL && c != 0; }
int is_label(const char *s) {
    size_t l = strlen(s);
    if (l < 1 || l > 32) return 0;
    if (!(s[0] >= 'a' && s[0] <= 'z')) return 0;
    for (size_t i = 1; i < l; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return 1;
}
int is_name(const char *s) {
    size_t l = strlen(s);
    if (l < 1 || l > 64) return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
    for (size_t i = 1; i < l; i++) {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || in(c, "_.-"))) return 0;
    }
    return 1;
}
static int is_lhex(const char *s, size_t want) {
    if (strlen(s) != want) return 0;
    for (size_t i = 0; i < want; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
int is_digest(const char *s) { return is_lhex(s, 64); }
int is_sha1hex(const char *s) { return is_lhex(s, 40); }
int is_text200(const char *s) {
    size_t l = strlen(s);
    if (l < 1 || l > 200) return 0;
    if (s[0] == ' ' || s[l - 1] == ' ') return 0;
    for (size_t i = 0; i < l; i++) if (s[i] < 0x20 || s[i] > 0x7e) return 0;
    return 1;
}
int hex_to_u64(const char *s16, uint64_t *out) {
    if (!is_lhex(s16, 16)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) { char c = s16[i]; v = (v << 4) | (uint64_t)(c <= '9' ? c - '0' : c - 'a' + 10); }
    *out = v;
    return 1;
}

static void to_hex(const uint8_t d[32], char out[65]) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = h[d[i] >> 4]; out[2 * i + 1] = h[d[i] & 15]; }
    out[64] = 0;
}
void sha256_hex(const uint8_t *data, size_t len, char out[65]) {
    uint8_t d[32]; sha256_hash(data, len, d); to_hex(d, out);
}
void sha256_tagged_hex(const char *domain, const uint8_t *data, size_t len, char out[65]) {
    sha256_ctx c; uint8_t d[32]; uint8_t z = 0;
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)domain, strlen(domain));
    sha256_update(&c, &z, 1);
    sha256_update(&c, data, len);
    sha256_final(&c, d);
    to_hex(d, out);
}

void findings_add(findings *fs, const char *code, const char *fmt, ...) {
    if (fs->n >= 256) return;
    finding *f = &fs->f[fs->n++];
    snprintf(f->code, sizeof f->code, "%s", code);
    va_list ap; va_start(ap, fmt);
    vsnprintf(f->detail, sizeof f->detail, fmt, ap);
    va_end(ap);
}
