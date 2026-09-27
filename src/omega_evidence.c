#include "omega_evidence.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/types.h>

static bool trim_line(char *buf) {
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r' || buf[len - 1] == ' ')) {
        buf[--len] = '\0';
    }
    return len > 0;
}

static bool popen_read_line(const char *cmd, char *out, size_t n) {
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[256] = {0};
    bool got = (fgets(buf, sizeof(buf), p) != NULL);
    pclose(p);
    if (!got || !trim_line(buf)) return false;
    if (strlen(buf) >= n) return false;
    strcpy(out, buf);
    return true;
}

/* ---- run_id -------------------------------------------------------------- */

static char g_run_id[96] = {0};
static bool g_run_id_ready = false;

const char *omega_evidence_run_id(void) {
    if (!g_run_id_ready) {
        time_t now = time(NULL);
        struct tm tm_info;
        gmtime_r(&now, &tm_info);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", &tm_info);

        char sha[64];
        if (!popen_read_line("git rev-parse --short=12 HEAD 2>/dev/null", sha, sizeof(sha))) {
            snprintf(sha, sizeof(sha), "unknown");
        }
        snprintf(g_run_id, sizeof(g_run_id), "%s-%s", ts, sha);
        g_run_id_ready = true;
    }
    return g_run_id;
}

/* ---- evidence path --------------------------------------------------- */

static void mkdir_p_for_file(const char *filepath) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", filepath);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

int omega_evidence_path(const char *relpath, char *out, size_t n) {
    if (!relpath || !out || n == 0) return -1;

    const char *run_id = omega_evidence_run_id();
    char candidate[1024];

    int len = snprintf(candidate, sizeof(candidate), "build/qual-runs/%s/%s", run_id, relpath);
    if (len < 0 || (size_t)len >= sizeof(candidate)) return -1;

    mkdir_p_for_file(candidate);

    if (strlen(candidate) >= n) return -1;
    strcpy(out, candidate);
    return 0;
}

/* ---- commit identity --------------------------------------------------- */

bool omega_evidence_run_commit(char out[41]) {
    char sha[64];
    if (!popen_read_line("git rev-parse HEAD 2>/dev/null", sha, sizeof(sha))) return false;
    if (strlen(sha) != 40) return false;
    memcpy(out, sha, 41);
    return true;
}

bool omega_evidence_tree_dirty(void) {
    FILE *p = popen("git status --porcelain --untracked-files=normal 2>/dev/null", "r");
    if (!p) return true; /* fail safe: unknown treated as dirty */
    char buf[256];
    bool any = (fgets(buf, sizeof(buf), p) != NULL);
    pclose(p);
    return any;
}

bool omega_evidence_physics_commit(char *out, size_t n) {
    FILE *f = fopen("physics.lock", "r");
    if (!f) return false;
    char buf[256] = {0};
    bool got = (fgets(buf, sizeof(buf), f) != NULL);
    fclose(f);
    if (!got || !trim_line(buf)) return false;
    if (strlen(buf) >= n) return false;
    strcpy(out, buf);
    return true;
}

/* ---- hardware identity --------------------------------------------------- */

void omega_evidence_hardware_from_nvrm(const Nvrm *rm, OmegaEvidenceHardware *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->alias = "sm_121";
    if (!rm) return;
    out->compute_class = rm->compute_class;
    out->rm_sm_version = rm->sm_version;
    for (int i = 0; i < 16; i++) {
        snprintf(&out->gpu_uuid_hex[i * 2], 3, "%02x", rm->gpu_uuid[i]);
    }
    out->gpu_uuid_hex[32] = '\0';
}
