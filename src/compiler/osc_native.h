/*
 * osc_native.h -- map OSC-1 machine code for execution (RW mmap, copy,
 * clear the instruction cache, mprotect RX), same pattern as src/omega_exec.c.
 * Run a function with osc_rt_call_native (osc_rt.h).
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 */
#ifndef OSC_NATIVE_H
#define OSC_NATIVE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    void *mem;      /* RX mapping, NULL if not mapped */
    size_t size;    /* mapping size (page multiple) */
    size_t len;     /* code bytes */
} OscNative;

/* 0 ok; -1 failure (mmap/mprotect), -2 not an AArch64 host. */
int osc_native_map(OscNative *nm, const uint8_t *code, size_t len);
/* Address of byte offset `off` in the mapping, or NULL if out of range/misaligned. */
void *osc_native_at(const OscNative *nm, uint32_t off);
void osc_native_unmap(OscNative *nm);

#endif /* OSC_NATIVE_H */
