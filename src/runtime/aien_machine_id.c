/*
 * aien_machine_id.c -- persistence and the local index table for the
 * canonical machine identity. The codec is in aien_machine_id.h.
 */
#include "aien_machine_id.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

static int write_all(int fd, const uint8_t *p, size_t n) {
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += (size_t)w; n -= (size_t)w;
    }
    return 0;
}

int aien_mid_store(const char *path, const AienMachineId *m) {
    if (!path || !m) return AIEN_MID_E_ARG;
    uint8_t rec[AIEN_MID_RECORD_BYTES];
    int rc = aien_mid_encode(m, rec);
    if (rc != AIEN_MID_OK) return rc;

    char tmp[4096];
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", path) >= sizeof tmp) return AIEN_MID_E_ARG;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return AIEN_MID_E_IO;
    if (write_all(fd, rec, sizeof rec) != 0 || fsync(fd) != 0) {
        close(fd); unlink(tmp); return AIEN_MID_E_IO;
    }
    if (close(fd) != 0 || rename(tmp, path) != 0) { unlink(tmp); return AIEN_MID_E_IO; }

    /* fsync the directory so the rename survives a crash */
    char dir[4096];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash == dir) dir[1] = 0;
    else if (slash) *slash = 0;
    else { dir[0] = '.'; dir[1] = 0; }
    int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) { fsync(dfd); close(dfd); }
    return AIEN_MID_OK;
}

int aien_mid_load(const char *path, AienMachineId *out) {
    if (!path || !out) return AIEN_MID_E_ARG;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return AIEN_MID_E_IO;
    uint8_t buf[AIEN_MID_RECORD_BYTES + 1];
    size_t got = 0;
    for (;;) {   /* read one byte past a record so a longer file is caught */
        ssize_t r = read(fd, buf + got, sizeof buf - got);
        if (r < 0) { if (errno == EINTR) continue; close(fd); return AIEN_MID_E_IO; }
        if (r == 0) break;
        got += (size_t)r;
        if (got == sizeof buf) break;
    }
    close(fd);
    return aien_mid_decode(buf, got, out);
}

void aien_mid_index_init(AienMachineIndex *t, AienMachineId *slots, uint32_t capacity) {
    t->capacity = slots ? capacity : 0;
    t->count = 0;
    t->slots = slots;
}

uint32_t aien_mid_index_find(const AienMachineIndex *t, const AienMachineId *m) {
    if (!t || !m) return 0;
    for (uint32_t i = 0; i < t->count; i++)
        if (aien_mid_equal(&t->slots[i], m)) return i + 1;
    return 0;
}

uint32_t aien_mid_index_bind(AienMachineIndex *t, const AienMachineId *m) {
    if (!t || !m || aien_mid_is_zero_(m->id) || !aien_mid_root_valid_(m->root)) return 0;
    uint32_t k = aien_mid_index_find(t, m);
    if (k) return k;
    if (t->count >= t->capacity) return 0;
    t->slots[t->count] = *m;
    return ++t->count;
}

int aien_mid_index_get(const AienMachineIndex *t, uint32_t index, AienMachineId *out) {
    if (!t || !out || index == 0 || index > t->count) return AIEN_MID_E_ARG;
    *out = t->slots[index - 1];
    return AIEN_MID_OK;
}
