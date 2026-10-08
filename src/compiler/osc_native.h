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

#include "osc_ir.h"
#include "osc_rt.h"

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
/* Checked native entry: refuses (-1, no call) malformed or overlapping slice args via osc_ir_slice_args_ok, else
 * osc_rt_call_native on the code at entry_off. Returns the trap code (0 = ok) or -1. osc_rt_call_native is raw and unchecked. */
int osc_native_call(const OscUnit *u, int fi, const OscNative *nm, uint32_t entry_off, OscRt *rt, const uint64_t *args,
                    unsigned nargs, uint64_t *ret);

#endif /* OSC_NATIVE_H */
