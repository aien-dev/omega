#include "dual_ref.h"
#include <math.h>
double dual_ref_gradient(double estimate, double budget, double scale, double uncertainty, double k_sigma)
{
    double p = (estimate - budget) / scale, band = k_sigma * uncertainty / scale;
    if (p > band) return p - band;
    if (p < -band) return p + band;
    return 0.0;
}
double dual_ref_closed_form(double l0, double g, double eta, double rho, unsigned long t)
{
    double a;
    if (rho == 0.0) return l0 + eta * g * (double)t;
    a = pow(1.0 - rho, (double)t);
    return a * l0 + eta * g * (1.0 - a) / rho;
}
double dual_ref_fixed_point(double g, double eta, double rho) { return eta * g / rho; }
