/* Independent SHA-256 (FIPS 180-4), lane D scorer. */
#ifndef IS_SHA256_H
#define IS_SHA256_H
#include <stddef.h>
#include <stdint.h>
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } is_sha256;
void is_sha256_init(is_sha256 *c);
void is_sha256_update(is_sha256 *c, const void *p, size_t len);
void is_sha256_final(is_sha256 *c, uint8_t out[32]);
void is_sha256_hex(const uint8_t d[32], char out[65]);
int is_sha256_file(const char *path, uint8_t out[32], uint64_t *bytes);
#endif
