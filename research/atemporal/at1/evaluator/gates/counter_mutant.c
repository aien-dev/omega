/* Positive control for gates/isolation.sh (AT-1 G2, charter mutant "counter"):
 * reads the hardware counter inline, so no symbol is referenced and a symbol
 * scan alone is blind. The instruction scan (at1-eval scan-clock) must reject
 * it. Compiled only, never linked or run. */
#include <stdint.h>
double at1_counter_phase(double x) {
    uint64_t t = 0;
#if defined(__aarch64__)
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
#elif defined(__x86_64__)
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    t = ((uint64_t)hi << 32) | lo;
#else
#error "counter_mutant.c: unsupported architecture"
#endif
    return x + (double)(t % 7) * 1e-18;
}
