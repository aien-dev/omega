/*
 * osc_native.c -- executable mapping for OSC-1 machine code.
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#include "osc_native.h"
#include "osc_ir.h"
#include "osc_rt.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int osc_native_map(OscNative *nm, const uint8_t *code, size_t len) {
    if (!nm || !code || len == 0 || (len & 3)) return -1;
    memset(nm, 0, sizeof *nm);
#if defined(__aarch64__)
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t size = (len + page - 1) & ~(page - 1);
    void *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return -1;
    memcpy(mem, code, len);
    __builtin___clear_cache((char *)mem, (char *)mem + len);
    if (mprotect(mem, size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, size);
        return -1;
    }
    nm->mem = mem;
    nm->size = size;
    nm->len = len;
    return 0;
#else
    return -2;
#endif
}

void *osc_native_at(const OscNative *nm, uint32_t off) {
    if (!nm || !nm->mem || off >= nm->len || (off & 3)) return NULL;
    return (uint8_t *)nm->mem + off;
}

void osc_native_unmap(OscNative *nm) {
    if (nm && nm->mem) munmap(nm->mem, nm->size);
    if (nm) memset(nm, 0, sizeof *nm);
}

int osc_native_call(const OscUnit *u, int fi, const OscNative *nm, uint32_t entry_off, OscRt *rt, const uint64_t *args,
                    unsigned nargs, uint64_t *ret) {
    if (!u || !nm || fi < 0 || (unsigned)fi >= u->nfuncs) return -1;
    void *entry = osc_native_at(nm, entry_off);
    if (!entry) return -1;
    if (osc_ir_slice_args_ok(&u->funcs[fi], args, nargs)) return -1; /* refused: never calls in */
    return osc_rt_call_native(rt, entry, args, nargs, ret);
}
