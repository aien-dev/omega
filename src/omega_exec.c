#include "omega_exec.h"
#include <sys/mman.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int omega_exec_native_f3(const RealizationObject *real, uint64_t a, uint64_t b, uint64_t c, uint64_t *out_res) {
    if (!real || !out_res || real->code_len == 0) return -1;

#if defined(__aarch64__)
    size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
    size_t alloc_size = (real->code_len + page_size - 1) & ~(page_size - 1);

    void *mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (mem == MAP_FAILED) return -1;

    memcpy(mem, real->code_bytes, real->code_len);
    __builtin___clear_cache((char*)mem, (char*)mem + real->code_len);

    if (mprotect(mem, alloc_size, PROT_READ | PROT_EXEC) != 0) {
        munmap(mem, alloc_size);
        return -1;
    }

    typedef uint64_t (*Fn3)(uint64_t, uint64_t, uint64_t);
    union {
        void *ptr;
        Fn3 fn;
    } u;
    u.ptr = mem;
    *out_res = u.fn(a, b, c);

    munmap(mem, alloc_size);
    return 0;
#else
    (void)a; (void)b; (void)c;
    return -2; /* Not native AArch64 host */
#endif
}

#include "aarch64_encoder.h"

int omega_build_qemu_runner(const RealizationObject *real, const char *out_bin_path) {
    if (!real || !out_bin_path) return -1;

    FILE *f = fopen(out_bin_path, "wb");
    if (!f) return -1;

    uint8_t boot_code[128];
    size_t boot_pos = 0;

    /* 1. Setup inputs:
     *   MOVZ X0, 7
     *   MOVZ X1, 11
     *   MOVZ X2, 3
     */
    aarch64_emit_movz(boot_code, &boot_pos, sizeof(boot_code), true, 0, 7, 0);
    aarch64_emit_movz(boot_code, &boot_pos, sizeof(boot_code), true, 1, 11, 0);
    aarch64_emit_movz(boot_code, &boot_pos, sizeof(boot_code), true, 2, 3, 0);

    /* 2. BL to synthesized function right after the branch */
    /* BL is relative word offset: BL +8 (skips 1 insn: the branch over the function) */
    uint32_t bl_insn = 0x94000002;
    boot_code[boot_pos++] = (uint8_t)(bl_insn & 0xff);
    boot_code[boot_pos++] = (uint8_t)((bl_insn >> 8) & 0xff);
    boot_code[boot_pos++] = (uint8_t)((bl_insn >> 16) & 0xff);
    boot_code[boot_pos++] = (uint8_t)((bl_insn >> 24) & 0xff);

    /* 3. Branch over synthesized function to post_code */
    size_t fn_words = real->code_len / 4;
    aarch64_emit_b(boot_code, &boot_pos, sizeof(boot_code), (int32_t)(fn_words + 1));

    fwrite(boot_code, 1, boot_pos, f);
    fwrite(real->code_bytes, 1, real->code_len, f);

    /* 4. Post-check and UART telemetry */
    uint8_t post_code[1024];
    size_t post_pos = 0;

    /* Base UART = 0x09000000 (MOVZ X4, 0x0900, LSL #16) */
    aarch64_emit_movz(post_code, &post_pos, sizeof(post_code), true, 4, 0x0900, 16);

    /* Verify X0 == 15 */
    /* MOVZ X7, 15 */
    aarch64_emit_movz(post_code, &post_pos, sizeof(post_code), true, 7, 15, 0);
    /* SUB X6, X0, X7 */
    aarch64_emit_sub_reg(post_code, &post_pos, sizeof(post_code), true, 6, 0, 7);

    /* CBNZ X6, fail_block (+74 instructions) */
    aarch64_emit_cbnz(post_code, &post_pos, sizeof(post_code), true, 6, 74);

    /* Pass block */
    const char *pass_msg = "OMEGA_QEMU_EXEC: PASS (observed=15)\n";
    for (size_t i = 0; pass_msg[i]; ++i) {
        aarch64_emit_movz(post_code, &post_pos, sizeof(post_code), false, 5, (uint8_t)pass_msg[i], 0);
        uint32_t strb = 0x39000085; /* STRB W5, [X4] */
        post_code[post_pos++] = (uint8_t)(strb & 0xff);
        post_code[post_pos++] = (uint8_t)((strb >> 8) & 0xff);
        post_code[post_pos++] = (uint8_t)((strb >> 16) & 0xff);
        post_code[post_pos++] = (uint8_t)((strb >> 24) & 0xff);
    }
    /* Jump over fail block to exit sequence:
     * fail_msg is 33 chars * 2 insns = 66 insns.
     * Offset to exit sequence = 1 + 66 = 67 insns.
     */
    aarch64_emit_b(post_code, &post_pos, sizeof(post_code), 67);

    /* Fail block */
    const char *fail_msg = "OMEGA_QEMU_EXEC: FAIL (mismatch)\n";
    for (size_t i = 0; fail_msg[i]; ++i) {
        aarch64_emit_movz(post_code, &post_pos, sizeof(post_code), false, 5, (uint8_t)fail_msg[i], 0);
        uint32_t strb = 0x39000085; /* STRB W5, [X4] */
        post_code[post_pos++] = (uint8_t)(strb & 0xff);
        post_code[post_pos++] = (uint8_t)((strb >> 8) & 0xff);
        post_code[post_pos++] = (uint8_t)((strb >> 16) & 0xff);
        post_code[post_pos++] = (uint8_t)((strb >> 24) & 0xff);
    }

    /* Exit sequence via AArch64 Semihosting (SYS_EXIT 0x18) */
    /* 1. MOVZ W0, 0x18 */
    uint32_t exit_insns[] = {
        0x52800300, /* MOVZ W0, #0x18 (SYS_EXIT) */
        0x10000061, /* ADR X1, +12 bytes (pointing to param_block) */
        0xD45E0000, /* HLT 0xF000 (Semihosting trap) */
        0x14000000, /* B . (fallback halt loop) */
        0x00020026, /* param_block: ADP_Stopped_ApplicationExit low word */
        0x00000000, /* ADP_Stopped_ApplicationExit high word */
        0x00000000, /* exit code 0 low word */
        0x00000000  /* exit code 0 high word */
    };
    for (size_t i = 0; i < sizeof(exit_insns) / sizeof(exit_insns[0]); ++i) {
        post_code[post_pos++] = (uint8_t)(exit_insns[i] & 0xff);
        post_code[post_pos++] = (uint8_t)((exit_insns[i] >> 8) & 0xff);
        post_code[post_pos++] = (uint8_t)((exit_insns[i] >> 16) & 0xff);
        post_code[post_pos++] = (uint8_t)((exit_insns[i] >> 24) & 0xff);
    }

    fwrite(post_code, 1, post_pos, f);
    fclose(f);
    return 0;
}

int omega_exec_qemu_virt(const char *runner_bin_path, char *out_log, size_t out_log_len) {
    if (!runner_bin_path || !out_log || out_log_len == 0) return -1;

    const char *serial_file = "build/qemu_serial.txt";
    unlink(serial_file);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "timeout 5 qemu-system-aarch64 -M virt -cpu cortex-a57 -m 128M -nographic "
             "-monitor none -semihosting -serial file:%s -bios %s >/dev/null 2>&1",
             serial_file, runner_bin_path);

    int ret = system(cmd);
    (void)ret;

    FILE *f = fopen(serial_file, "r");
    if (!f) return -1;

    size_t n = fread(out_log, 1, out_log_len - 1, f);
    out_log[n] = '\0';
    fclose(f);

    return (strstr(out_log, "OMEGA_QEMU_EXEC: PASS") != NULL) ? 0 : -1;
}
