#ifndef M16_NATIVE_H
#define M16_NATIVE_H
#include <stddef.h>
#include <stdint.h>
typedef struct { struct { char err[256]; } rm; } M16NativeContext;
int m16_native_open(M16NativeContext *ctx);
int m16_native_close(M16NativeContext *ctx);
int m16_native_wait_marker(volatile uint32_t *word, uint32_t want, uint64_t ms);
#endif
