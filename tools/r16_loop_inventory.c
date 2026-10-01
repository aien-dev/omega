/*
 * r16_loop_inventory: R16-G2 code-search gate (spec/r16-orchestrator-retirement.md).
 *
 * Scans the five R16 repositories for loop-shaped sites, joins every site
 * with spec/r16-orchestrator-retirement-map.md, prints a machine-readable
 * JSON inventory on stdout and a human summary on stderr.
 *
 * Exit status: 0 PASS, 1 FAIL (unclassified site, "?" row, omega class-A
 * site still reachable, stale/duplicate/bad map row, or a repository
 * SKIPPED), 2 usage or I/O error.
 *
 * Deterministic: files come from `git ls-files` (sorted, tracked files only),
 * no network, no clock, no randomness. C and Rust only (asm, shell, Mojo are
 * outside the gate's file-type scope; the map says so).
 *
 * What counts as a site (see --patterns):
 *   loop head  : C while/for/do, Rust while/for..in/loop
 *   flagged if : unbounded (while(1), while(true), for(;;), Rust loop,
 *                while true)  OR  a wait/hand-off word appears in the
 *                loop condition or body (word list below)
 *   named term : any code line naming run_until_complete (anywhere in an
 *                identifier), or max_steps / max_turns (whole identifier)
 *   cli-mode   : C line testing argv[ against "--demonstrate-...", "--reference-..."
 *                or "--run-..."
 *   manual row : map row whose line cell starts with "manual"; its evidence
 *                line must still exist in the file
 * Comments and string/char literal contents are blanked before matching.
 * Identifiers are split into words at '_', digits and case changes, so
 * "return" never matches "turn" and "HeartbeatEngine" matches "heartbeat".
 *
 * Map join key: repo + path + enclosing symbol + evidence (the source line,
 * whitespace-collapsed). Line numbers are informative only.
 *
 * Class-A reachability (omega only) is a naming proxy: an omega class-A
 * site passes only if its path or symbol contains "legacy_oracle" or
 * "reference" (spec R16-G5 naming). R16-G3 does the real link-map check.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NREPO 5
static const char *REPO_NAME[NREPO] = {"omega", "aien-sovereign-core", "aegis-runtime", "aienos", "physics"};
static const char *REPO_ENV[NREPO] = {"R16_REPO_OMEGA", "R16_REPO_SOVEREIGN_CORE", "R16_REPO_AEGIS_RUNTIME",
                                      "R16_REPO_AIENOS", "R16_REPO_PHYSICS"};
/* Excluded path prefixes / substrings (declared in --patterns and the map). */
static const char *EXCLUDE_SUB[] = {"vendor/", "third_party/", "tests/r16_inventory/", NULL};

/* Words that make a bounded loop loop-shaped (wait / hand-off / sequencing). */
static const char *BODY_WORDS[] = {"sleep", "usleep", "nanosleep", "msleep", "poll", "recv", "heartbeat",
                                   "tick", "dispatch", "schedule", "scheduler", "orchestrate", "orchestrator",
                                   "turn", "turns", "pulse", "yield", "epoll", NULL};
/* Named terms (identifier, lowercased, underscores removed, substring). */
static const char *NAMED[] = {"rununtilcomplete", "maxsteps", "maxturns", NULL};
static const char *NAMED_SHOW[] = {"run_until_complete", "max_steps", "max_turns", NULL};
/* Search-term hit counts reported for the map (spec §4: each term with its count). */
static const char *TERM_WORDS[] = {"poll", "sleep", "wait", "recv", "heartbeat", "tick", "dispatch", "schedule",
                                   "orchestrate", "turn", "step", "pulse", "yield", NULL};

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "r16_loop_inventory: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(2);
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}
static char *xstrdup(const char *s) {
    char *d = strdup(s);
    if (!d) die("out of memory");
    return d;
}

/* ---------- sites ---------- */
typedef struct {
    int repo;
    char *path, *symbol, *evidence, *trigger, *lines;
    int first_line, count;
    int row; /* matched map row or -1 */
} Site;
static Site *sites;
static int nsites, capsites;

/* ---------- map rows ---------- */
typedef struct {
    char *id, *repo, *path, *line, *symbol, *oldrole, *newrole, *cls, *auth, *reason, *evidence;
    int repo_idx, hits;
} Row;
static Row *rows;
static int nrows, caprows;
static char map_sha[NREPO][64];

static unsigned long term_hits[NREPO][32];
static unsigned long named_hits[NREPO][8];

/* ---------- helpers ---------- */
static char *normalize_ws(const char *s, size_t n) {
    char *o = xrealloc(NULL, n + 1);
    size_t k = 0;
    int sp = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\r') continue;
        if (c == ' ' || c == '\t') {
            sp = 1;
            continue;
        }
        if (sp && k) o[k++] = ' ';
        sp = 0;
        o[k++] = c;
    }
    o[k] = 0;
    return o;
}
static int is_id(int c) { return isalnum(c) || c == '_'; }
static int ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

/* Blank comments and string/char literal contents (keeps newlines). */
static void strip_code(char *b, size_t n, int rust) {
    size_t i = 0;
    while (i < n) {
        char c = b[i];
        if (c == '/' && i + 1 < n && b[i + 1] == '/') {
            while (i < n && b[i] != '\n') b[i++] = ' ';
        } else if (c == '/' && i + 1 < n && b[i + 1] == '*') {
            int depth = 0;
            do {
                if (i + 1 < n && b[i] == '/' && b[i + 1] == '*') {
                    depth++;
                    b[i] = b[i + 1] = ' ';
                    i += 2;
                    if (!rust) depth = 1;
                } else if (i + 1 < n && b[i] == '*' && b[i + 1] == '/') {
                    depth--;
                    b[i] = b[i + 1] = ' ';
                    i += 2;
                } else {
                    if (b[i] != '\n') b[i] = ' ';
                    i++;
                }
            } while (i < n && depth > 0);
        } else if (rust && (c == 'r' || (c == 'b' && i + 1 < n && b[i + 1] == 'r')) &&
                   (i == 0 || !is_id((unsigned char)b[i - 1]))) {
            size_t j = i + (c == 'b' ? 2 : 1);
            int hashes = 0;
            while (j < n && b[j] == '#') hashes++, j++;
            if (j < n && b[j] == '"' && (hashes > 0 || j == i + (c == 'b' ? 2 : 1))) {
                j++;
                while (j < n) {
                    if (b[j] == '"') {
                        int h = 0;
                        while (h < hashes && j + 1 + h < n && b[j + 1 + h] == '#') h++;
                        if (h == hashes) {
                            j += 1 + h;
                            break;
                        }
                    }
                    j++;
                }
                for (size_t k = i; k < j && k < n; k++)
                    if (b[k] != '\n') b[k] = ' ';
                i = j;
            } else {
                i++;
            }
        } else if (c == '"') {
            b[i++] = ' ';
            while (i < n && b[i] != '"') {
                if (b[i] == '\\' && i + 1 < n) {
                    b[i] = ' ';
                    i++;
                }
                if (b[i] != '\n') b[i] = ' ';
                i++;
            }
            if (i < n) b[i++] = ' ';
        } else if (c == '\'') {
            /* char literal vs Rust lifetime / label */
            size_t j = i + 1;
            int lit = 0;
            if (j < n && b[j] == '\\') {
                lit = 1;
            } else {
                for (size_t k = j + 1; k < n && k <= j + 4; k++)
                    if (b[k] == '\'') {
                        lit = 1;
                        break;
                    }
                if (rust && j < n && is_id((unsigned char)b[j]) && !(j + 1 < n && b[j + 1] == '\'')) lit = 0;
            }
            if (!lit) {
                i++;
                continue;
            }
            b[i++] = ' ';
            while (i < n && b[i] != '\'' && b[i] != '\n') {
                if (b[i] == '\\' && i + 1 < n) {
                    b[i] = ' ';
                    i++;
                }
                b[i] = ' ';
                i++;
            }
            if (i < n && b[i] == '\'') b[i++] = ' ';
        } else {
            i++;
        }
    }
}

/* Split identifier into lowercase words at '_', digits and case changes. */
static int split_words(const char *id, size_t len, char words[][40], int maxw) {
    int nw = 0;
    size_t i = 0;
    while (i < len && nw < maxw) {
        while (i < len && !isalpha((unsigned char)id[i])) i++;
        if (i >= len) break;
        size_t s = i;
        if (isupper((unsigned char)id[i])) {
            i++;
            if (i < len && isupper((unsigned char)id[i])) {
                while (i < len && isupper((unsigned char)id[i]) &&
                       !(i + 1 < len && islower((unsigned char)id[i + 1])))
                    i++;
            } else {
                while (i < len && islower((unsigned char)id[i])) i++;
            }
        } else {
            while (i < len && islower((unsigned char)id[i])) i++;
        }
        size_t L = i - s;
        if (L > 39) L = 39;
        for (size_t k = 0; k < L; k++) words[nw][k] = (char)tolower((unsigned char)id[s + k]);
        words[nw][L] = 0;
        nw++;
    }
    return nw;
}
static int word_in(const char *w, const char **list) {
    for (int i = 0; list[i]; i++)
        if (!strcmp(w, list[i])) return i;
    return -1;
}
/* Returns index into NAMED if the identifier names a gated term:
   run_until_complete anywhere in the name; max_steps / max_turns only as the
   whole name (so size constants such as CL_MAX_STEPS are counted, not gated). */
static int named_in_ident(const char *id, size_t len) {
    char t[256];
    size_t k = 0;
    for (size_t i = 0; i < len && k < sizeof t - 1; i++)
        if (id[i] != '_') t[k++] = (char)tolower((unsigned char)id[i]);
    t[k] = 0;
    for (int i = 0; NAMED[i]; i++)
        if (i == 0 ? strstr(t, NAMED[i]) != NULL : !strcmp(t, NAMED[i])) return i;
    return -1;
}
/* Counting form: substring for all terms. */
static int named_count_in_ident(const char *id, size_t len) {
    char t[256];
    size_t k = 0;
    for (size_t i = 0; i < len && k < sizeof t - 1; i++)
        if (id[i] != '_') t[k++] = (char)tolower((unsigned char)id[i]);
    t[k] = 0;
    for (int i = 0; NAMED[i]; i++)
        if (strstr(t, NAMED[i])) return i;
    return -1;
}

/* First body word found in stripped range [a,b); writes it to out. */
static int find_body_word(const char *s, size_t a, size_t b, char *out, size_t outn) {
    size_t i = a;
    while (i < b) {
        if (is_id((unsigned char)s[i]) && (i == 0 || !is_id((unsigned char)s[i - 1]))) {
            size_t j = i;
            while (j < b && is_id((unsigned char)s[j])) j++;
            char words[16][40];
            int nw = split_words(s + i, j - i, words, 16);
            for (int w = 0; w < nw; w++)
                if (word_in(words[w], BODY_WORDS) >= 0) {
                    size_t wl = strnlen(words[w], 39);
                    if (wl >= outn) wl = outn - 1;
                    memcpy(out, words[w], wl);
                    out[wl] = 0;
                    return 1;
                }
            i = j;
        } else {
            i++;
        }
    }
    return 0;
}

static void add_site(int repo, const char *path, int line, const char *symbol, const char *evidence,
                     const char *trigger) {
    for (int i = nsites - 1; i >= 0 && sites[i].repo == repo && !strcmp(sites[i].path, path); i--) {
        if (!strcmp(sites[i].symbol, symbol) && !strcmp(sites[i].evidence, evidence)) {
            if (sites[i].first_line == line) return; /* same line flagged twice */
            size_t L = strlen(sites[i].lines);
            sites[i].lines = xrealloc(sites[i].lines, L + 16);
            snprintf(sites[i].lines + L, 16, ",%d", line);
            sites[i].count++;
            return;
        }
    }
    if (nsites == capsites) {
        capsites = capsites ? capsites * 2 : 256;
        sites = xrealloc(sites, sizeof *sites * (size_t)capsites);
    }
    Site *st = &sites[nsites++];
    st->repo = repo;
    st->path = xstrdup(path);
    st->symbol = xstrdup(symbol);
    st->evidence = xstrdup(evidence);
    st->trigger = xstrdup(trigger);
    char buf[16];
    snprintf(buf, sizeof buf, "%d", line);
    st->lines = xstrdup(buf);
    st->first_line = line;
    st->count = 1;
    st->row = -1;
}

/* ---------- per-file scan ---------- */
static void scan_file(int repo, const char *root, const char *rel) {
    int rust = ends_with(rel, ".rs");
    size_t fulln = strlen(root) + strlen(rel) + 2;
    char *full = xrealloc(NULL, fulln);
    snprintf(full, fulln, "%s/%s", root, rel);
    FILE *f = fopen(full, "rb");
    if (!f) {
        free(full);
        return; /* tracked but absent (e.g. sparse); ignore */
    }
    fseek(f, 0, SEEK_END);
    long fl = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fl < 0) die("cannot size %s", full);
    size_t n = (size_t)fl;
    char *orig = xrealloc(NULL, n + 1);
    if (fread(orig, 1, n, f) != n) die("cannot read %s", full);
    fclose(f);
    free(full);
    orig[n] = 0;
    char *s = xrealloc(NULL, n + 1);
    memcpy(s, orig, n + 1);
    strip_code(s, n, rust);

    /* line starts */
    int nl = 1;
    for (size_t i = 0; i < n; i++)
        if (orig[i] == '\n') nl++;
    size_t *ls = xrealloc(NULL, sizeof(size_t) * (size_t)(nl + 1));
    int *lnof_dummy = NULL;
    (void)lnof_dummy;
    ls[0] = 0;
    int li = 1;
    for (size_t i = 0; i < n; i++)
        if (orig[i] == '\n') ls[li++] = i + 1;
    ls[nl] = n + 1;

    /* enclosing symbol per line */
    char **sym = xrealloc(NULL, sizeof(char *) * (size_t)nl);
    {
        char cur[128] = "-", cand[128] = "-";
        int depth = 0, pd = 0;
        for (int L = 0; L < nl; L++) {
            size_t a = ls[L], b = ls[L + 1] - 1;
            if (b > n) b = n;
            if (rust) {
                /* nearest preceding `fn name` */
                for (size_t i = a; i + 3 < b; i++) {
                    if (s[i] == 'f' && s[i + 1] == 'n' && (s[i + 2] == ' ' || s[i + 2] == '\t') &&
                        (i == 0 || !is_id((unsigned char)s[i - 1]))) {
                        size_t j = i + 2;
                        while (j < b && (s[j] == ' ' || s[j] == '\t')) j++;
                        size_t k = j;
                        while (k < b && is_id((unsigned char)s[k])) k++;
                        if (k > j && k - j < sizeof cur) {
                            memcpy(cur, s + j, k - j);
                            cur[k - j] = 0;
                        }
                        break;
                    }
                }
                sym[L] = xstrdup(cur);
            } else {
                const char *startsym = depth > 0 ? cur : "-";
                for (size_t i = a; i < b; i++) {
                    char c = s[i];
                    if (depth == 0) {
                        if (c == ')' && pd > 0) pd--;
                        if (c == '(' && pd++ == 0) {
                            size_t k = i;
                            while (k > a && (s[k - 1] == ' ' || s[k - 1] == '\t')) k--;
                            size_t j = k;
                            while (j > a && is_id((unsigned char)s[j - 1])) j--;
                            if (k > j && k - j < sizeof cand && strcmp(cand, "=") != 0) {
                                memcpy(cand, s + j, k - j);
                                cand[k - j] = 0;
                            }
                        } else if (c == ';') {
                            strcpy(cand, "-");
                        } else if (c == '=') {
                            strcpy(cand, "=");
                        }
                    }
                    if (c == '{') {
                        if (depth == 0) strcpy(cur, strcmp(cand, "=") ? cand : "-");
                        depth++;
                    } else if (c == '}') {
                        if (depth > 0) depth--;
                        if (depth == 0) {
                            strcpy(cand, "-");
                        }
                    }
                }
                /* a loop on this line belongs to the function open at its start
                   or opened on it */
                sym[L] = xstrdup(depth > 0 ? cur : startsym);
                if (depth == 0) strcpy(cur, "-");
            }
        }
    }

    /* token scan */
    int line = 0;
    for (size_t i = 0; i < n; i++) {
        while (line + 1 < nl && ls[line + 1] <= i) line++;
        if (!is_id((unsigned char)s[i]) || (i > 0 && is_id((unsigned char)s[i - 1]))) continue;
        size_t j = i;
        while (j < n && is_id((unsigned char)s[j])) j++;
        size_t len = j - i;
        /* named terms and term counts */
        int nt = named_in_ident(s + i, len);
        char words[16][40];
        int nw = split_words(s + i, len, words, 16);
        for (int w = 0; w < nw; w++) {
            int t = word_in(words[w], TERM_WORDS);
            if (t < 0 && (!strcmp(words[w], "schedule") || !strcmp(words[w], "scheduler") ||
                          !strcmp(words[w], "sched")))
                t = word_in("schedule", TERM_WORDS);
            if (t < 0 && (!strcmp(words[w], "orchestrator") || !strcmp(words[w], "orchestration")))
                t = word_in("orchestrate", TERM_WORDS);
            if (t < 0 && !strcmp(words[w], "turns")) t = word_in("turn", TERM_WORDS);
            if (t < 0 && !strcmp(words[w], "steps")) t = word_in("step", TERM_WORDS);
            if (t < 0 && (!strcmp(words[w], "usleep") || !strcmp(words[w], "nanosleep") || !strcmp(words[w], "msleep")))
                t = word_in("sleep", TERM_WORDS);
            if (t >= 0) term_hits[repo][t]++;
        }
        size_t la = ls[line], lb = ls[line + 1] - 1;
        if (lb > n) lb = n;
        int ntc = named_count_in_ident(s + i, len);
        if (ntc >= 0) named_hits[repo][ntc]++;
        if (nt >= 0) {
            char *ev = normalize_ws(orig + la, lb - la);
            char trig[64];
            snprintf(trig, sizeof trig, "named:%s", NAMED_SHOW[nt]);
            add_site(repo, rel, line + 1, sym[line], ev, trig);
            free(ev);
        }
        const char *kw = NULL;
        if (len == 5 && !memcmp(s + i, "while", 5)) kw = "while";
        else if (len == 3 && !memcmp(s + i, "for", 3)) kw = "for";
        else if (rust && len == 4 && !memcmp(s + i, "loop", 4)) kw = "loop";
        else if (!rust && len == 2 && !memcmp(s + i, "do", 2)) kw = "do";
        if (!kw) {
            i = j - 1;
            continue;
        }
        size_t k = j;
        while (k < n && isspace((unsigned char)s[k])) k++;
        if (!strcmp(kw, "loop") && (k >= n || s[k] != '{')) kw = NULL;
        else if (!strcmp(kw, "do") && (k >= n || s[k] != '{')) kw = NULL;
        else if (!rust && (!strcmp(kw, "while") || !strcmp(kw, "for")) && (k >= n || s[k] != '(')) kw = NULL;
        else if (rust && !strcmp(kw, "for")) {
            /* `for pat in expr {` : require " in " before '{' on this line */
            int ok = 0;
            for (size_t q = k; q + 1 < n && s[q] != '{' && s[q] != '\n' && s[q] != ';'; q++)
                if (s[q] == 'i' && s[q + 1] == 'n' && q > 0 && !is_id((unsigned char)s[q - 1]) &&
                    (q + 2 >= n || !is_id((unsigned char)s[q + 2]))) {
                    ok = 1;
                    break;
                }
            if (!ok) kw = NULL;
        }
        if (kw && !rust && !strcmp(kw, "while")) {
            /* do { } while (..); tail: previous non-space is '}' */
            size_t p = i;
            while (p > 0 && isspace((unsigned char)s[p - 1])) p--;
            if (p > 0 && s[p - 1] == '}') kw = NULL;
        }
        if (!kw) {
            i = j - 1;
            continue;
        }
        /* extent: condition + body */
        size_t e = k;
        int par = 0, unbounded = 0;
        if (!strcmp(kw, "loop")) unbounded = 1;
        if (!rust && s[k] == '(') {
            size_t q = k;
            int d = 0;
            for (; q < n; q++) {
                if (s[q] == '(') d++;
                else if (s[q] == ')' && --d == 0) break;
            }
            char cond[64];
            size_t ci = 0;
            for (size_t r = k + 1; r < q && ci < sizeof cond - 1; r++)
                if (!isspace((unsigned char)s[r])) cond[ci++] = s[r];
            cond[ci] = 0;
            if (!strcmp(kw, "while") && (!strcmp(cond, "1") || !strcmp(cond, "true") || !strcmp(cond, "!0")))
                unbounded = 1;
            if (!strcmp(kw, "for") && !strcmp(cond, ";;")) unbounded = 1;
            e = q + 1;
        }
        if (rust && !strcmp(kw, "while")) {
            size_t q = k;
            while (q < n && isspace((unsigned char)s[q])) q++;
            if (q + 4 <= n && !memcmp(s + q, "true", 4) && (q + 4 == n || !is_id((unsigned char)s[q + 4])))
                unbounded = 1;
        }
        /* find body start '{' at paren depth 0, or ';' ending a single statement */
        size_t endp = n;
        for (size_t q = e; q < n; q++) {
            char c = s[q];
            if (c == '(' || c == '[') par++;
            else if ((c == ')' || c == ']') && par > 0) par--;
            else if (par == 0 && c == ';') {
                endp = q + 1;
                break;
            } else if (par == 0 && c == '{') {
                int d = 0;
                size_t r = q;
                for (; r < n; r++) {
                    if (s[r] == '{') d++;
                    else if (s[r] == '}' && --d == 0) break;
                }
                endp = r < n ? r + 1 : n;
                if (!strcmp(kw, "do")) { /* include the while(...) tail */
                    while (endp < n && s[endp] != ';') endp++;
                }
                break;
            }
        }
        char word[40] = "";
        int hit = find_body_word(s, j, endp, word, sizeof word);
        if (unbounded || hit) {
            char trig[96];
            if (unbounded && hit) snprintf(trig, sizeof trig, "%s:unbounded+%s", kw, word);
            else if (unbounded) snprintf(trig, sizeof trig, "%s:unbounded", kw);
            else snprintf(trig, sizeof trig, "%s:body:%s", kw, word);
            char *ev = normalize_ws(orig + la, lb - la);
            add_site(repo, rel, line + 1, sym[line], ev, trig);
            free(ev);
        }
        i = j - 1;
    }
    /* CLI mode dispatch to a hand-sequenced demonstration or gate runner (C). */
    if (!rust)
        for (int L = 0; L < nl; L++) {
            size_t a = ls[L], b = ls[L + 1] - 1;
            if (b > n) b = n;
            char *ev = normalize_ws(orig + a, b - a);
            char *code = normalize_ws(s + a, b - a); /* strings blanked: argv[ must be code */
            int in_code = strstr(code, "argv[") != NULL;
            free(code);
            if (in_code && (strstr(ev, "\"--demonstrate-") || strstr(ev, "\"--reference-") ||
                            strstr(ev, "\"--run-")) &&
                strspn(s + a, " \t") < b - a)
                add_site(repo, rel, L + 1, sym[L], ev, "cli-mode");
            free(ev);
        }
    for (int L = 0; L < nl; L++) free(sym[L]);
    free(sym);
    free(ls);
    free(s);
    free(orig);
}

static int excluded(const char *rel) {
    for (int i = 0; EXCLUDE_SUB[i]; i++) {
        size_t L = strlen(EXCLUDE_SUB[i]);
        if (!strncmp(rel, EXCLUDE_SUB[i], L)) return 1;
        const char *p = strstr(rel, EXCLUDE_SUB[i]);
        if (p && p > rel && p[-1] == '/' && strcmp(EXCLUDE_SUB[i], "tests/r16_inventory/")) return 1;
    }
    return 0;
}

static int run_cmd(const char *cmd, char *out, size_t outn) {
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    size_t k = fread(out, 1, outn - 1, p);
    out[k] = 0;
    while (k && (out[k - 1] == '\n' || out[k - 1] == '\r')) out[--k] = 0;
    return pclose(p);
}

/* Shell-quote a path into buf (single quotes). */
static void shq(char *buf, size_t n, const char *s) {
    size_t k = 0;
    buf[k++] = '\'';
    for (; *s && k + 5 < n; s++) {
        if (*s == '\'') {
            memcpy(buf + k, "'\\''", 4);
            k += 4;
        } else
            buf[k++] = *s;
    }
    buf[k++] = '\'';
    buf[k] = 0;
}

static char repo_head[NREPO][64];
static int repo_skipped[NREPO];

static void scan_repo(int r, const char *root) {
    char q[4200], cmd[8600];
    shq(q, sizeof q, root);
    snprintf(cmd, sizeof cmd, "git -C %s rev-parse HEAD 2>/dev/null", q);
    if (run_cmd(cmd, repo_head[r], sizeof repo_head[r]) != 0 || !repo_head[r][0]) {
        repo_skipped[r] = 1;
        return;
    }
    snprintf(cmd, sizeof cmd, "git -C %s ls-files -z -- '*.c' '*.h' '*.rs'", q);
    FILE *p = popen(cmd, "r");
    if (!p) die("popen failed");
    char path[4096];
    size_t k = 0;
    int c;
    char **files = NULL;
    int nf = 0, cf = 0;
    while ((c = fgetc(p)) != EOF) {
        if (c == 0) {
            path[k] = 0;
            if (k && !excluded(path)) {
                if (nf == cf) {
                    cf = cf ? cf * 2 : 512;
                    files = xrealloc(files, sizeof(char *) * (size_t)cf);
                }
                files[nf++] = xstrdup(path);
            }
            k = 0;
        } else if (k < sizeof path - 1)
            path[k++] = (char)c;
    }
    if (pclose(p) != 0) die("git ls-files failed in %s", root);
    /* git ls-files order is already sorted by path bytes; keep it */
    for (int i = 0; i < nf; i++) {
        scan_file(r, root, files[i]);
        free(files[i]);
    }
    free(files);
}

/* ---------- map parsing ---------- */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t L = strlen(s);
    while (L && (s[L - 1] == ' ' || s[L - 1] == '\t' || s[L - 1] == '\r' || s[L - 1] == '\n')) s[--L] = 0;
    return s;
}
static char *untick(char *s) {
    s = trim(s);
    size_t L = strlen(s);
    while (L >= 2 && s[0] == '`' && s[L - 1] == '`') {
        s[L - 1] = 0;
        s++;
        L -= 2;
    }
    return trim(s);
}
/* split a markdown table row on unescaped '|', unescaping "\|" */
static int split_row(char *line, char **cells, int maxc) {
    int nc = 0;
    char *p = line;
    while (*p == ' ') p++;
    if (*p != '|') return 0;
    p++;
    char *start = p, *w = p;
    while (*p) {
        if (*p == '\\' && p[1] == '|') {
            *w++ = '|';
            p += 2;
        } else if (*p == '|') {
            *w = 0;
            if (nc < maxc) cells[nc++] = start;
            p++;
            start = w = p;
        } else
            *w++ = *p++;
    }
    return nc;
}
static int repo_index(const char *name) {
    for (int i = 0; i < NREPO; i++)
        if (!strcmp(name, REPO_NAME[i])) return i;
    return -1;
}
static int map_errors;
static void load_map(const char *mp) {
    FILE *f = fopen(mp, "r");
    if (!f) die("cannot open map %s", mp);
    char *line = NULL;
    size_t cap = 0;
    int mode = 0; /* 1 shas, 2 rows */
    int lineno = 0;
    while (getline(&line, &cap, f) > 0) {
        lineno++;
        if (strstr(line, "<!-- r16-inventory:shas -->")) { mode = 1; continue; }
        if (strstr(line, "<!-- r16-inventory:rows -->")) { mode = 2; continue; }
        if (strstr(line, "<!-- /r16-inventory")) { mode = 0; continue; }
        if (!mode) continue;
        char *cells[16];
        char *copy = xstrdup(line);
        int nc = split_row(copy, cells, 16);
        if (nc == 0 || strstr(line, "---|") || strstr(line, "| id |") || strstr(line, "| repo | sha")) {
            free(copy);
            continue;
        }
        if (mode == 1 && nc >= 2) {
            int r = repo_index(untick(cells[0]));
            if (r >= 0) snprintf(map_sha[r], sizeof map_sha[r], "%s", untick(cells[1]));
            free(copy);
            continue;
        }
        if (mode == 2) {
            if (nc < 11) {
                fprintf(stderr, "MAP-ERROR line %d: row has %d cells, need 11\n", lineno, nc);
                map_errors++;
                free(copy);
                continue;
            }
            if (nrows == caprows) {
                caprows = caprows ? caprows * 2 : 256;
                rows = xrealloc(rows, sizeof *rows * (size_t)caprows);
            }
            Row *w = &rows[nrows++];
            char **fld[11] = {&w->id, &w->repo, &w->path, &w->line, &w->symbol, &w->oldrole,
                              &w->newrole, &w->cls, &w->auth, &w->reason, &w->evidence};
            for (int c = 0; c < 11; c++) *fld[c] = xstrdup(untick(cells[c]));
            char *ev = normalize_ws(w->evidence, strlen(w->evidence));
            free(w->evidence);
            w->evidence = ev;
            w->repo_idx = repo_index(w->repo);
            w->hits = 0;
            if (w->repo_idx < 0) {
                fprintf(stderr, "MAP-ERROR line %d: unknown repo '%s'\n", lineno, w->repo);
                map_errors++;
            }
        }
        free(copy);
    }
    free(line);
    fclose(f);
    for (int i = 0; i < nrows; i++)
        for (int j = i + 1; j < nrows; j++)
            if (rows[i].repo_idx == rows[j].repo_idx && !strcmp(rows[i].path, rows[j].path) &&
                !strcmp(rows[i].symbol, rows[j].symbol) && !strcmp(rows[i].evidence, rows[j].evidence)) {
                fprintf(stderr, "MAP-ERROR duplicate rows %s and %s\n", rows[i].id, rows[j].id);
                map_errors++;
            }
}

static int class_ok(const char *c) {
    return strlen(c) == 1 && ((c[0] >= 'A' && c[0] <= 'F') || c[0] == 'N');
}

#define NCLASS 8
static const char CLASS_CHARS[NCLASS] = {'A', 'B', 'C', 'D', 'E', 'F', 'N', '?'};

static int class_idx(const char *cls) {
    if (!cls || strlen(cls) != 1) return 7;
    char c = cls[0];
    if (c >= 'A' && c <= 'F') return c - 'A';
    if (c == 'N') return 6;
    return 7;
}

static int check_n_reason(const char *reason) {
    static const char *idioms[] = {
        "cas", "seqlock", "probe", "parse", "walk", "read",
        "arithmetic", "sift", "sample", "merge", "retry",
        "format", "bpe", "utf8", "token", "decode", "scan",
        NULL
    };
    if (!reason) return 0;
    char lower[2048];
    size_t len = strlen(reason);
    if (len >= sizeof(lower)) len = sizeof(lower) - 1;
    for (size_t i = 0; i < len; i++)
        lower[i] = (char)tolower((unsigned char)reason[i]);
    lower[len] = '\0';
    for (int i = 0; idioms[i]; i++) {
        if (strstr(lower, idioms[i])) return 1;
    }
    return 0;
}

static int trigger_has_wait(const char *trig) {
    static const char *wait_words[] = {
        "sleep", "usleep", "nanosleep", "msleep", "poll", "wait", NULL
    };
    if (!trig) return 0;
    for (int i = 0; wait_words[i]; i++) {
        if (strstr(trig, wait_words[i])) return 1;
    }
    return 0;
}

/* Manual rows (line cell starts with "manual"): non-loop sequencers the
   patterns cannot see. Present if the whitespace-collapsed evidence line
   occurs in the file (symbol not verified). */
static int manual_present(const char *root, const char *path, const char *evidence) {
    size_t fn = strlen(root) + strlen(path) + 2;
    char *full = xrealloc(NULL, fn);
    snprintf(full, fn, "%s/%s", root, path);
    FILE *f = fopen(full, "r");
    free(full);
    if (!f) return 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    int found = 0;
    while (!found && (got = getline(&line, &cap, f)) > 0) {
        while (got && (line[got - 1] == '\n' || line[got - 1] == '\r')) line[--got] = 0;
        char *ev = normalize_ws(line, (size_t)got);
        found = !strcmp(ev, evidence);
        free(ev);
    }
    free(line);
    fclose(f);
    return found;
}

/* ---------- output ---------- */
static void jstr(FILE *o, const char *s) {
    fputc('"', o);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(o, "\\%c", c);
        else if (c < 0x20) fprintf(o, "\\u%04x", c);
        else fputc(c, o);
    }
    fputc('"', o);
}
static void mdcell(FILE *o, const char *s, int tick) {
    if (tick) fputs(strchr(s, '`') ? "`` " : "`", o);
    for (; *s; s++) {
        if (*s == '|') fputs("\\|", o);
        else fputc(*s, o);
    }
    if (tick) fputs(strchr(s, '`') ? " ``" : "`", o);
}

static void print_patterns(FILE *o) {
    fprintf(o, "R16 loop inventory patterns (tools/r16_loop_inventory.c)\n");
    fprintf(o, "files: git ls-files '*.c' '*.h' '*.rs' (tracked only); excluded path prefixes:");
    for (int i = 0; EXCLUDE_SUB[i]; i++) fprintf(o, " %s", EXCLUDE_SUB[i]);
    fprintf(o, " (vendor/ and third_party/ also at any depth)\n");
    fprintf(o, "comments and string/char literal contents blanked before matching\n");
    fprintf(o, "loop heads: C while( for( do{ ; Rust while, for..in, loop{ ; C do-while tails not double-counted\n");
    fprintf(o, "unbounded: while(1) while(true) while(!0) for(;;) Rust loop{} Rust while true\n");
    fprintf(o, "body words (identifier words, split at _ digits and case):");
    for (int i = 0; BODY_WORDS[i]; i++) fprintf(o, " %s", BODY_WORDS[i]);
    fprintf(o, "\nflagged loop = unbounded OR a body word in condition or body\n");
    fprintf(o, "named terms (any code line; case/underscore-insensitive; run_until_complete anywhere in an identifier, max_steps and max_turns only as the whole identifier; hit counts use substring):");
    for (int i = 0; NAMED_SHOW[i]; i++) fprintf(o, " %s", NAMED_SHOW[i]);
    fprintf(o, "\ncli-mode (C): a code line testing argv[ against a \"--demonstrate-...\", \"--reference-...\" or \"--run-...\" literal\n"
               "manual rows (line cell starts with \"manual\"): evidence line must still exist in the file\n"
               "  (symbol not verified); same class rules apply");
    fprintf(o, "\njoin key: repo + path + enclosing symbol + whitespace-collapsed source line\n");
    fprintf(o, "class-A reachability (omega only, naming proxy; G3 checks the link map):"
               " path or symbol must contain legacy_oracle or reference\n");
    fprintf(o, "class N (not a central loop): data/control-flow idiom; reason must name allowed idiom; body must not contain wait words\n");
}

int main(int argc, char **argv) {
    const char *mapp = getenv("R16_MAP");
    const char *jsonp = NULL;
    int skeleton = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--patterns")) {
            print_patterns(stdout);
            return 0;
        } else if (!strcmp(argv[i], "--skeleton")) skeleton = 1;
        else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapp = argv[++i];
        else if (!strcmp(argv[i], "--json") && i + 1 < argc) jsonp = argv[++i];
        else {
            fprintf(stderr, "usage: r16_loop_inventory [--map FILE] [--json FILE] [--skeleton] [--patterns]\n"
                            "env: R16_MAP R16_REPO_OMEGA R16_REPO_SOVEREIGN_CORE R16_REPO_AEGIS_RUNTIME\n"
                            "     R16_REPO_AIENOS R16_REPO_PHYSICS\n");
            return 2;
        }
    }
    if (!mapp) mapp = "spec/r16-orchestrator-retirement-map.md";
    const char *home = getenv("HOME");
    if (!home) home = ".";
    char defp[NREPO][4096];
    for (int r = 0; r < NREPO; r++) {
        const char *e = getenv(REPO_ENV[r]);
        if (e && *e) snprintf(defp[r], sizeof defp[r], "%s", e);
        else if (r == 0) snprintf(defp[r], sizeof defp[r], ".");
        else snprintf(defp[r], sizeof defp[r], "%s/workspace/r16-survey/%s", home, REPO_NAME[r]);
    }
    if (!skeleton) load_map(mapp);
    else {
        FILE *t = fopen(mapp, "r");
        if (t) {
            fclose(t);
            load_map(mapp);
        }
    }
    int skipped = 0;
    for (int r = 0; r < NREPO; r++) {
        scan_repo(r, defp[r]);
        if (!repo_skipped[r]) fprintf(stderr, "SCANNED %s HEAD %s (%s)\n", REPO_NAME[r], repo_head[r], defp[r]);
        if (repo_skipped[r]) {
            skipped++;
            fprintf(stderr, "SKIPPED %s: no git work tree at %s (set %s). Skipping is NOT a pass.\n",
                    REPO_NAME[r], defp[r], REPO_ENV[r]);
        } else if (map_sha[r][0] && strncmp(repo_head[r], map_sha[r], strlen(map_sha[r]))) {
            fprintf(stderr, "WARN %s: scanned HEAD %.12s differs from map SHA %s\n", REPO_NAME[r],
                    repo_head[r], map_sha[r]);
        }
    }
    /* join */
    int unclassified = 0, qrows = 0, badclass = 0, areach = 0, stale = 0;
    for (int i = 0; i < nsites; i++) {
        Site *st = &sites[i];
        for (int j = 0; j < nrows; j++)
            if (rows[j].repo_idx == st->repo && !strcmp(rows[j].path, st->path) &&
                !strcmp(rows[j].symbol, st->symbol) && !strcmp(rows[j].evidence, st->evidence)) {
                st->row = j;
                rows[j].hits++;
                break;
            }
    }
    int manual_rows = 0;
    for (int j = 0; j < nrows; j++) {
        Row *w = &rows[j];
        if (strncmp(w->line, "manual", 6) || w->repo_idx < 0 || repo_skipped[w->repo_idx]) continue;
        manual_rows++;
        if (w->hits == 0 && manual_present(defp[w->repo_idx], w->path, w->evidence)) w->hits = 1;
    }
    if (skeleton) {
        printf("| id | repo | path | line | symbol | old role | new role | class | authoritative | reason | evidence |\n");
        printf("|---|---|---|---|---|---|---|---|---|---|---|\n");
        for (int i = 0; i < nsites; i++) {
            Site *st = &sites[i];
            Row *w = st->row >= 0 ? &rows[st->row] : NULL;
            printf("| %s | %s | ", w ? w->id : "NEW", REPO_NAME[st->repo]);
            mdcell(stdout, st->path, 1);
            printf(" | %s | ", st->lines);
            mdcell(stdout, st->symbol, 1);
            printf(" | %s | %s | %s | %s | ", w ? w->oldrole : st->trigger, w ? w->newrole : "?",
                   w ? w->cls : "?", w ? w->auth : "?");
            mdcell(stdout, w ? w->reason : "?", 0);
            printf(" | ");
            mdcell(stdout, st->evidence, 1);
            printf(" |\n");
        }
        return 0;
    }
    for (int i = 0; i < nsites; i++) {
        Site *st = &sites[i];
        if (st->row < 0) {
            unclassified++;
            fprintf(stderr, "UNCLASSIFIED %s %s:%s [%s] %s :: %s\n", REPO_NAME[st->repo], st->path, st->lines,
                    st->symbol, st->trigger, st->evidence);
            continue;
        }
        Row *w = &rows[st->row];
        if (!strcmp(w->cls, "?")) {
            qrows++;
            fprintf(stderr, "QUESTION %s %s %s:%s :: %s\n", w->id, REPO_NAME[st->repo], st->path, st->lines,
                    w->reason);
        } else if (!class_ok(w->cls)) {
            badclass++;
            fprintf(stderr, "BAD-CLASS %s '%s'\n", w->id, w->cls);
        } else if (w->cls[0] == 'N') {
            if (!check_n_reason(w->reason)) {
                badclass++;
                fprintf(stderr, "N-NO-IDIOM %s: reason lacks required idiom word\n", w->id);
            }
            if (trigger_has_wait(st->trigger)) {
                badclass++;
                fprintf(stderr, "N-WAIT-WORD %s [%s] contains wait word in body\n", w->id, st->trigger);
            }
        } else if (st->repo == 0 && w->cls[0] == 'A' && !strstr(st->path, "legacy_oracle") &&
                   !strstr(st->symbol, "legacy_oracle") && !strstr(st->path, "reference") &&
                   !strstr(st->symbol, "reference")) {
            areach++;
            fprintf(stderr, "A-REACHABLE %s omega %s:%s [%s] class A still under a production name\n", w->id,
                    st->path, st->lines, st->symbol);
        }
    }
    for (int j = 0; j < nrows; j++) {
        Row *w = &rows[j];
        if (strncmp(w->line, "manual", 6) || w->repo_idx < 0 || repo_skipped[w->repo_idx] || !w->hits) continue;
        if (!strcmp(w->cls, "?")) {
            qrows++;
            fprintf(stderr, "QUESTION %s %s %s (manual) :: %s\n", w->id, w->repo, w->path, w->reason);
        } else if (!class_ok(w->cls)) {
            badclass++;
            fprintf(stderr, "BAD-CLASS %s '%s'\n", w->id, w->cls);
        } else if (w->cls[0] == 'N') {
            if (!check_n_reason(w->reason)) {
                badclass++;
                fprintf(stderr, "N-NO-IDIOM %s (manual): reason lacks required idiom word\n", w->id);
            }
        } else if (w->repo_idx == 0 && w->cls[0] == 'A' && !strstr(w->path, "legacy_oracle") &&
                   !strstr(w->symbol, "legacy_oracle") && !strstr(w->path, "reference") &&
                   !strstr(w->symbol, "reference")) {
            areach++;
            fprintf(stderr, "A-REACHABLE %s omega %s [%s] (manual) class A still under a production name\n",
                    w->id, w->path, w->symbol);
        }
    }
    for (int j = 0; j < nrows; j++)
        if (rows[j].repo_idx >= 0 && !repo_skipped[rows[j].repo_idx] && rows[j].hits == 0) {
            if (rows[j].cls[0] == 'A') {
                fprintf(stderr, "RETIRED-GONE %s %s %s (class A site no longer present)\n", rows[j].id,
                        rows[j].repo, rows[j].path);
            } else {
                stale++;
                fprintf(stderr, "STALE %s %s %s [%s] :: %s\n", rows[j].id, rows[j].repo, rows[j].path,
                        rows[j].symbol, rows[j].evidence);
            }
        }
    int fail = unclassified || qrows || badclass || areach || stale || map_errors || skipped;
    int percls[NREPO][NCLASS];
    memset(percls, 0, sizeof percls);
    int cnt_n = 0;
    for (int i = 0; i < nsites; i++) {
        int c = 7;
        if (sites[i].row >= 0 && class_ok(rows[sites[i].row].cls)) {
            c = class_idx(rows[sites[i].row].cls);
            if (rows[sites[i].row].cls[0] == 'N') cnt_n++;
        }
        percls[sites[i].repo][c]++;
    }
    FILE *o = stdout;
    if (jsonp) {
        o = fopen(jsonp, "w");
        if (!o) die("cannot write %s", jsonp);
    }
    fprintf(o, "{\n  \"schema\": \"AIEN_R16_LOOP_INVENTORY_V1\",\n  \"map\": ");
    jstr(o, mapp);
    fprintf(o, ",\n  \"repos\": [\n");
    for (int r = 0; r < NREPO; r++) {
        fprintf(o, "    {\"name\": \"%s\", \"path\": ", REPO_NAME[r]);
        jstr(o, defp[r]);
        fprintf(o, ", \"head\": ");
        jstr(o, repo_head[r]);
        fprintf(o, ", \"map_sha\": ");
        jstr(o, map_sha[r]);
        fprintf(o, ", \"status\": \"%s\", \"sites_by_class\": {", repo_skipped[r] ? "SKIPPED" : "SCANNED");
        for (int c = 0; c < NCLASS; c++) fprintf(o, "%s\"%c\": %d", c ? ", " : "", CLASS_CHARS[c], percls[r][c]);
        fprintf(o, "}, \"term_hits\": {");
        for (int t = 0; TERM_WORDS[t]; t++) fprintf(o, "%s\"%s\": %lu", t ? ", " : "", TERM_WORDS[t], term_hits[r][t]);
        for (int t = 0; NAMED_SHOW[t]; t++) fprintf(o, ", \"%s\": %lu", NAMED_SHOW[t], named_hits[r][t]);
        fprintf(o, "}}%s\n", r + 1 < NREPO ? "," : "");
    }
    fprintf(o, "  ],\n  \"sites\": [\n");
    for (int i = 0; i < nsites; i++) {
        Site *st = &sites[i];
        Row *w = st->row >= 0 ? &rows[st->row] : NULL;
        fprintf(o, "    {\"repo\": \"%s\", \"path\": ", REPO_NAME[st->repo]);
        jstr(o, st->path);
        fprintf(o, ", \"lines\": \"%s\", \"symbol\": ", st->lines);
        jstr(o, st->symbol);
        fprintf(o, ", \"trigger\": ");
        jstr(o, st->trigger);
        fprintf(o, ", \"row\": ");
        jstr(o, w ? w->id : "");
        fprintf(o, ", \"class\": ");
        jstr(o, w ? w->cls : "");
        fprintf(o, ", \"authoritative\": ");
        jstr(o, w ? w->auth : "");
        fprintf(o, ", \"evidence\": ");
        jstr(o, st->evidence);
        fprintf(o, "}%s\n", i + 1 < nsites ? "," : "");
    }
    fprintf(o,
            "  ],\n  \"manual_rows\": %d,\n  \"sites_total\": %d,\n  \"unclassified\": %d,\n  \"question_rows\": %d,\n"
            "  \"bad_class\": %d,\n  \"a_reachable\": %d,\n  \"stale_rows\": %d,\n  \"map_errors\": %d,\n"
            "  \"skipped_repos\": %d,\n  \"class_n_sites\": %d,\n  \"result\": \"%s\"\n}\n",
            manual_rows, nsites, unclassified, qrows, badclass, areach, stale, map_errors, skipped, cnt_n, fail ? "FAIL" : "PASS");
    if (jsonp) fclose(o);
    fprintf(stderr,
            "R16 loop inventory: sites=%d unclassified=%d question=%d bad_class=%d a_reachable=%d stale=%d "
            "map_errors=%d skipped=%d -> %s\n",
            nsites, unclassified, qrows, badclass, areach, stale, map_errors, skipped, fail ? "FAIL" : "PASS");
    return fail ? 1 : 0;
}
