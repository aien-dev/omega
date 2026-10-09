/* Positive control for gates/isolation.sh: a compute-like object that secretly
 * reads the wall clock. The gate must FAIL on this object. Never linked into
 * anything else. */
#include <time.h>
double at0_mutant_phase(double x) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return x + (double)(ts.tv_nsec % 7) * 1e-18;
}
