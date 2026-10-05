/* Append-only verifier receipts. A receipt is never overwritten (O_EXCL). */
#ifndef PD0_RECEIPT_H
#define PD0_RECEIPT_H
#include <stddef.h>
#include <stdint.h>
#ifndef PD0_VERIFIER_COMMIT
#define PD0_VERIFIER_COMMIT "unknown"
#endif
/* writes <dir>/<kind>-<16 hex of input_hash>-<n>.receipt; returns 0 or PD0V_RECEIPT_IO; path_out gets the file name */
int pd0_receipt_write(const char *dir, const char *kind, const uint8_t input_hash[32], int code, const char *extra, char *path_out, size_t cap);
#endif
