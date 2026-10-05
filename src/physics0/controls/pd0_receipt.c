#include "pd0_receipt.h"
#include "pd0_codes.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static int errno_is_exist(void) { return errno == EEXIST; }
int pd0_receipt_write(const char *dir, const char *kind, const uint8_t h[32], int code, const char *extra, char *path_out, size_t cap)
{
    char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    for (unsigned n = 0; n < 100000; n++) {
        char path[1024]; snprintf(path, sizeof path, "%s/%s-%.16s-%u.receipt", dir, kind, hex, n);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0444);
        if (fd < 0) { if (errno_is_exist()) continue; return PD0V_RECEIPT_IO; }
        char body[4096]; int len = snprintf(body, sizeof body,
            "kind: %s\nverifier_commit: %s\ninput_sha256: %s\ncode: %d\nname: %s\nrecorder: STAND_IN\nrange_check: STAND_IN\n%s%s",
            kind, PD0_VERIFIER_COMMIT, hex, code, pd0v_name(code), extra ? extra : "", extra && extra[strlen(extra) - 1] != '\n' ? "\n" : "");
        if (len < 0 || write(fd, body, (size_t)len) != len) { close(fd); return PD0V_RECEIPT_IO; }
        close(fd); if (path_out) snprintf(path_out, cap, "%s", path); return 0;
    }
    return PD0V_RECEIPT_IO;
}
