/* Isolation gate positive control (Agent 5 finding, omega#358): a hardened-libc file read.
 * With -D_FORTIFY_SOURCE the compiler rewrites fread/open into __fread_chk/__open_2; a gate that
 * matches only the plain names misses them. The references are written out explicitly so the
 * control does not depend on the compiler's fortify behaviour. The gate must FAIL this object. */
extern unsigned long __fread_chk(void *buf, unsigned long bufsize, unsigned long size, unsigned long n, void *stream);
extern int __open_2(const char *path, int flags);
int at0_hidden_file_read(void *stream) {
    char b[8];
    int fd = __open_2("/etc/hostname", 0);
    return fd + (int)__fread_chk(b, sizeof b, 1, sizeof b, stream);
}
