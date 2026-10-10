/* Positive control for gates/isolation.sh (AT-1 G2): a compute-like object
 * that reads the wall clock through libc. The symbol scan must reject it.
 * Compiled only, never linked or run. */
#include <time.h>
double at1_hidden_clock_phase(double x) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return x + (double)(ts.tv_nsec % 7) * 1e-18;
}
