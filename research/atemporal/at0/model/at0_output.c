/* OMEGA-AT0-ENGINE v1: the engine's versioned output. Its values block is the
 * AT0_RESULT_V1 section 1 values block without the oracle's reference lines,
 * so a runner can splice it into a result. This is a private format of the
 * model directory, not a contract and not an identity. */
#include "at0_model.h"
#include <math.h>
#include <string.h>

int at0_f64_format(double x, char *buf, size_t cap)
{
    if (!isfinite(x)) return snprintf(buf, cap, "nonfinite");
    if (x == 0.0) x = 0.0;                     /* fold -0.0 to +0.0 */
    uint64_t bits;
    memcpy(&bits, &x, sizeof bits);
    return snprintf(buf, cap, "f64:%016llx", (unsigned long long)bits);
}

typedef struct { char *buf; size_t cap; size_t len; int overflow; } out_t;
static void put(out_t *o, const char *s)
{
    size_t n = strlen(s);
    if (o->len + n >= o->cap) { o->overflow = 1; return; }
    memcpy(o->buf + o->len, s, n); o->len += n; o->buf[o->len] = 0;
}
static void put_value(out_t *o, double v, at0_scaled b)
{
    char t[64];
    at0_f64_format(v, t, sizeof t); put(o, t); put(o, " ");
    at0_scaled_format(b, t, sizeof t); put(o, t);
}

long at0_engine_emit(const at0_case *c, const at0_engine_result *r, char *buf, size_t cap)
{
    if (!c || !r || !buf || cap == 0) return -1;
    static const char *axes[3] = { "X", "Y", "Z" };
    static const char *signs[2] = { "PLUS", "MINUS" };
    out_t o = { buf, cap, 0, 0 };
    char t[512];
    buf[0] = 0;
    put(&o, "OMEGA-AT0-ENGINE v1\ndomain omega.at0.engine.v1\ncontract AT0_RESULT_V2\n");
    snprintf(t, sizeof t, "case_name %s\ncase_id %s\nacceptance_id %s\ncase_file_sha256 %s\n",
             c->name, c->case_id, c->acceptance_id, c->case_file_sha256); put(&o, t);
    put(&o, "begin numerics\narithmetic BINARY64\n");
    put(&o, r->bound_kind == AT0_BOUND_ESTIMATED ? "bound_kind ESTIMATED\n" : "bound_kind NONE\n");
    put(&o, "threads 1\nend numerics\nbegin values\n");
    snprintf(t, sizeof t, "physical_state_kernel_dim %d\n", r->kernel_dim); put(&o, t);
    put(&o, "constraint_residual ");
    if (r->psi_nonzero) put_value(&o, r->constraint_residual, r->constraint_residual_bound); else put(&o, "undefined 0@0");
    put(&o, "\n");
    put(&o, "povm_residual "); put_value(&o, r->povm_residual, r->povm_residual_bound); put(&o, "\n");
    for (int k = 0; k < r->label_count; k++) {
        const at0_label_values *lv = &r->label[k];
        const char *status = lv->status == AT0_LABEL_DEFINED ? "DEFINED"
                           : lv->status == AT0_LABEL_UNDEFINED ? "UNDEFINED" : "INDETERMINATE";
        snprintf(t, sizeof t, "label %d %s %s\n", k, c->labels[k], status); put(&o, t);
    }
    for (int k = 0; k < r->label_count; k++) {
        const at0_label_values *lv = &r->label[k];
        snprintf(t, sizeof t, "clock_probability %d ", k); put(&o, t);
        if (r->psi_nonzero) put_value(&o, lv->clock_probability, lv->clock_probability_bound); else put(&o, "undefined 0@0");
        put(&o, "\n");
    }
    for (int k = 0; k < r->label_count; k++) {
        const at0_label_values *lv = &r->label[k];
        for (int ax = 0; ax < 3; ax++) {
            for (int sg = 0; sg < 2; sg++) {
                snprintf(t, sizeof t, "pauli %d %s %s ", k, axes[ax], signs[sg]); put(&o, t);
                if (lv->status == AT0_LABEL_DEFINED) put_value(&o, lv->pauli[ax][sg], lv->pauli_bound[ax][sg]);
                else put(&o, "undefined 0@0");
                put(&o, "\n");
            }
        }
    }
    put(&o, "end values\nend\n");
    return o.overflow ? -1 : (long)o.len;
}
