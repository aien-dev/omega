/* Positive control for gates/isolation.sh (AT-1 G2, charter mutant
 * "raw-syscall"): asks the kernel for the time with an inline system call, so
 * no libc symbol is referenced. The instruction scan (at1-eval scan-clock)
 * must reject it. Compiled only, never linked or run.
 *   Linux aarch64: svc #0 with x8 = 113 (clock_gettime)
 *   macOS arm64:   svc #0x80 with x16 = 116 (gettimeofday)
 *   Linux x86_64:  syscall with rax = 228 (clock_gettime) */
#include <stdint.h>
struct at1_ts { int64_t s, ns; };
double at1_raw_syscall_phase(double x) {
    struct at1_ts ts = { 0, 0 };
#if defined(__aarch64__) && defined(__linux__)
    register long r0 __asm__("x0") = 0;
    register long r1 __asm__("x1") = (long)&ts;
    register long r8 __asm__("x8") = 113;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r8) : "memory");
#elif defined(__aarch64__) && defined(__APPLE__)
    register long r0 __asm__("x0") = (long)&ts;
    register long r1 __asm__("x1") = 0;
    register long r16 __asm__("x16") = 116;
    __asm__ volatile("svc #0x80" : "+r"(r0) : "r"(r1), "r"(r16) : "memory", "cc");
#elif defined(__x86_64__) && defined(__linux__)
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(228L), "D"(0L), "S"(&ts) : "rcx", "r11", "memory");
    (void)ret;
#else
#error "raw_syscall_mutant.c: unsupported platform"
#endif
    return x + (double)(ts.ns % 7) * 1e-18;
}
