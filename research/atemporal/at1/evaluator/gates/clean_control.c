/* Negative control for gates/isolation.sh (AT-1 G2): pure arithmetic, no
 * forbidden symbol, no counter read, no system call. The gate must PASS. */
#include <math.h>
double at1_clean_phase(double x) { return cos(x) * cos(x) + sin(x) * sin(x); }
