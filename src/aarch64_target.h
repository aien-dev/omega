/* =========================================================================
 * BOOTSTRAP REPRESENTATION - NOT PERMANENT SEMANTIC DEFINITION
 * AArch64 Target Profile for Milestone 5 (OMEGA_AARCH64)
 * ========================================================================= */

#ifndef AARCH64_TARGET_H
#define AARCH64_TARGET_H

#include <stdint.h>
#include <stdbool.h>

#define AARCH64_PROFILE_V8A_BAREMETAL 0x01
#define AARCH64_MAX_CODE_BYTES 4096

typedef enum {
    REG_X0 = 0,  REG_X1 = 1,  REG_X2 = 2,  REG_X3 = 3,
    REG_X4 = 4,  REG_X5 = 5,  REG_X6 = 6,  REG_X7 = 7,
    REG_X8 = 8,  REG_X9 = 9,  REG_X10 = 10, REG_X11 = 11,
    REG_X12 = 12, REG_X13 = 13, REG_X14 = 14, REG_X15 = 15,
    REG_X16 = 16, REG_X17 = 17, REG_X18 = 18, REG_X19 = 19,
    REG_X20 = 20, REG_X21 = 21, REG_X22 = 22, REG_X23 = 23,
    REG_X24 = 24, REG_X25 = 25, REG_X26 = 26, REG_X27 = 27,
    REG_X28 = 28, REG_X29 = 29, REG_X30 = 30, REG_XZR = 31,
    REG_SP = 31
} Aarch64Reg;

typedef enum {
    COND_EQ = 0,  /* Equal */
    COND_NE = 1,  /* Not equal */
    COND_CS = 2,  /* Carry set (identical to HS) */
    COND_CC = 3,  /* Carry clear (identical to LO) */
    COND_MI = 4,  /* Minus / negative */
    COND_PL = 5,  /* Plus / positive or zero */
    COND_VS = 6,  /* Overflow */
    COND_VC = 7,  /* No overflow */
    COND_HI = 8,  /* Unsigned higher */
    COND_LS = 9,  /* Unsigned lower or same */
    COND_GE = 10, /* Signed greater than or equal */
    COND_LT = 11, /* Signed less than */
    COND_GT = 12, /* Signed greater than */
    COND_LE = 13, /* Signed less than or equal */
    COND_AL = 14  /* Always */
} Aarch64Cond;

typedef struct {
    uint8_t profile_id;
    uint8_t abi_version;
    uint8_t default_reg_width;
} Aarch64TargetProfile;

#endif /* AARCH64_TARGET_H */
