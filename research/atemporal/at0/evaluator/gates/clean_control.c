/* Negative control for gates/isolation.sh: pure arithmetic, no forbidden
 * symbol. The gate must PASS on this object. */
#include <math.h>
double at0_clean_phase(double x) { return cos(x) * cos(x) + sin(x) * sin(x); }
