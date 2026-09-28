/*
 * rx_cog_engines.c -- stand-in cognition engines. See rx_cog_engines.h.
 */
#include "rx_cog_engines.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

uint64_t cog_rng(uint64_t *s) {
    uint64_t z = (*s += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static float frand(uint64_t *s) { return (float)((cog_rng(s) >> 40) * (1.0 / 16777216.0)); }

/* ---- recognition ---- */

static double rec_f(const float *x) {
    return 1.5 * x[0] * x[1] + sin(2.5 * x[2]) + 0.8 * x[3] * x[3] - 0.45 + 0.6 * x[4] -
           0.9 * x[5] * x[6] + 0.3 * cos(3.0 * x[7]) + 0.25 * x[8] * x[9] * x[10];
}

int rec_truth(const RecInput *in) { return rec_f(in->x) > 0.0; }

void rec_sample(uint64_t *rng, RecInput *in) {
    for (int i = 0; i < COG_DIM; i++) in->x[i] = 2.0f * frand(rng) - 1.0f;
}

typedef struct {
    int nin, h1, h2;
    float *w1, *b1, *w2, *b2, *w3, b3;
} Mlp;

static float gauss(uint64_t *s) {
    float a = frand(s) + 1e-7f, b = frand(s);
    return sqrtf(-2.0f * logf(a)) * cosf(6.2831853f * b);
}

static void mlp_init(Mlp *m, int nin, int h1, int h2, uint64_t seed) {
    free(m->w1);
    memset(m, 0, sizeof *m);
    m->nin = nin;
    m->h1 = h1;
    m->h2 = h2;
    int last = h2 ? h2 : h1;
    size_t total = (size_t)nin * h1 + h1 + (size_t)h1 * h2 + h2 + last;
    float *p = calloc(total, sizeof(float));
    m->w1 = p;
    p += (size_t)nin * h1;
    m->b1 = p;
    p += h1;
    m->w2 = p;
    p += (size_t)h1 * h2;
    m->b2 = p;
    p += h2;
    m->w3 = p;
    uint64_t s = seed;
    for (int i = 0; i < nin * h1; i++) m->w1[i] = gauss(&s) / sqrtf((float)nin);
    for (int i = 0; i < h1 * h2; i++) m->w2[i] = gauss(&s) / sqrtf((float)h1);
    for (int i = 0; i < last; i++) m->w3[i] = gauss(&s) / sqrtf((float)last);
}

static size_t mlp_params(const Mlp *m) {
    int last = m->h2 ? m->h2 : m->h1;
    return (size_t)m->nin * m->h1 + m->h1 + (size_t)m->h1 * m->h2 + m->h2 + last + 1;
}

/* Returns the logit; fills hidden activations when given. */
static float mlp_forward(const Mlp *m, const float *x, float *a1, float *a2) {
    float t1[256], t2[256];
    if (!a1) a1 = t1;
    if (!a2) a2 = t2;
    for (int j = 0; j < m->h1; j++) {
        const float *w = m->w1 + (size_t)j * m->nin;
        float s = m->b1[j];
        for (int i = 0; i < m->nin; i++) s += w[i] * x[i];
        a1[j] = tanhf(s);
    }
    const float *top = a1;
    int last = m->h1;
    if (m->h2) {
        for (int k = 0; k < m->h2; k++) {
            const float *w = m->w2 + (size_t)k * m->h1;
            float s = m->b2[k];
            for (int j = 0; j < m->h1; j++) s += w[j] * a1[j];
            a2[k] = tanhf(s);
        }
        top = a2;
        last = m->h2;
    }
    float s = m->b3;
    for (int k = 0; k < last; k++) s += m->w3[k] * top[k];
    return s;
}

static float sigmoid(float z) { return 1.0f / (1.0f + expf(-z)); }

static void mlp_step(Mlp *m, const float *x, int y, float lr) {
    float a1[256], a2[256], d1[256], d2[256];
    float z = mlp_forward(m, x, a1, a2);
    float g = sigmoid(z) - (float)y; /* d loss / d logit */
    const float *top = m->h2 ? a2 : a1;
    int last = m->h2 ? m->h2 : m->h1;
    float *dtop = m->h2 ? d2 : d1;
    for (int k = 0; k < last; k++) {
        dtop[k] = g * m->w3[k] * (1.0f - top[k] * top[k]);
        m->w3[k] -= lr * g * top[k];
    }
    m->b3 -= lr * g;
    if (m->h2) {
        for (int j = 0; j < m->h1; j++) d1[j] = 0;
        for (int k = 0; k < m->h2; k++) {
            float *w = m->w2 + (size_t)k * m->h1;
            for (int j = 0; j < m->h1; j++) {
                d1[j] += d2[k] * w[j];
                w[j] -= lr * d2[k] * a1[j];
            }
            m->b2[k] -= lr * d2[k];
        }
        for (int j = 0; j < m->h1; j++) d1[j] *= 1.0f - a1[j] * a1[j];
    }
    for (int j = 0; j < m->h1; j++) {
        float *w = m->w1 + (size_t)j * m->nin;
        for (int i = 0; i < m->nin; i++) w[i] -= lr * d1[j] * x[i];
        m->b1[j] -= lr * d1[j];
    }
}

/* Small policy: logistic regression over x and x^2. */
static float g_policy_w[2 * COG_DIM + 1];

static void policy_features(const float *x, float *phi) {
    for (int i = 0; i < COG_DIM; i++) {
        phi[i] = x[i];
        phi[COG_DIM + i] = x[i] * x[i];
    }
}

static float policy_logit(const float *x) {
    float phi[2 * COG_DIM];
    policy_features(x, phi);
    float s = g_policy_w[2 * COG_DIM];
    for (int i = 0; i < 2 * COG_DIM; i++) s += g_policy_w[i] * phi[i];
    return s;
}

static Mlp g_neural, g_neural_backup_shape, g_general, g_ens_a, g_ens_b;
static float *g_neural_backup;

void cog_engines_train(uint64_t seed, uint32_t examples, uint32_t epochs) {
    RecInput *xs = malloc(examples * sizeof(RecInput));
    uint8_t *ys = malloc(examples);
    uint64_t rng = seed;
    for (uint32_t i = 0; i < examples; i++) {
        rec_sample(&rng, &xs[i]);
        ys[i] = (uint8_t)rec_truth(&xs[i]);
    }
    mlp_init(&g_neural, COG_DIM, 12, 0, seed ^ 1);
    mlp_init(&g_general, COG_DIM, 96, 96, seed ^ 2);
    mlp_init(&g_ens_a, COG_DIM, 64, 64, seed ^ 3);
    mlp_init(&g_ens_b, COG_DIM, 64, 64, seed ^ 4);
    memset(g_policy_w, 0, sizeof g_policy_w);
    uint32_t *order = malloc(examples * sizeof(uint32_t));
    for (uint32_t i = 0; i < examples; i++) order[i] = i;
    for (uint32_t e = 0; e < epochs; e++) {
        for (uint32_t i = examples - 1; i > 0; i--) {
            uint32_t j = (uint32_t)(cog_rng(&rng) % (i + 1));
            uint32_t t = order[i];
            order[i] = order[j];
            order[j] = t;
        }
        float lr = 0.05f / (1.0f + 0.5f * (float)e);
        for (uint32_t k = 0; k < examples; k++) {
            const RecInput *x = &xs[order[k]];
            int y = ys[order[k]];
            float phi[2 * COG_DIM];
            policy_features(x->x, phi);
            float g = sigmoid(policy_logit(x->x)) - (float)y;
            for (int i = 0; i < 2 * COG_DIM; i++) g_policy_w[i] -= lr * g * phi[i];
            g_policy_w[2 * COG_DIM] -= lr * g;
            mlp_step(&g_neural, x->x, y, lr);
            mlp_step(&g_general, x->x, y, lr);
            mlp_step(&g_ens_a, x->x, y, lr);
            mlp_step(&g_ens_b, x->x, y, lr);
        }
    }
    free(order);
    free(xs);
    free(ys);
    size_t n = mlp_params(&g_neural);
    free(g_neural_backup);
    g_neural_backup = malloc(n * sizeof(float));
    memcpy(g_neural_backup, g_neural.w1, (n - 1) * sizeof(float));
    g_neural_backup[n - 1] = g_neural.b3;
    g_neural_backup_shape = g_neural;
}

void cog_damage_neural(uint64_t seed, float scale) {
    uint64_t s = seed;
    size_t n = (size_t)g_neural.nin * g_neural.h1;
    for (size_t i = 0; i < n; i++) g_neural.w1[i] += scale * gauss(&s);
}

void cog_restore_neural(void) {
    size_t n = mlp_params(&g_neural);
    memcpy(g_neural.w1, g_neural_backup, (n - 1) * sizeof(float));
    g_neural.b3 = g_neural_backup[n - 1];
}

static uint32_t conf_of(float p) {
    float c = p > 0.5f ? p : 1.0f - p;
    uint32_t v = (uint32_t)(c * (float)RX_COG_CONF_ONE);
    return v > RX_COG_CONF_ONE ? RX_COG_CONF_ONE : v;
}

static int rec_answer(float p, void *output, uint32_t *conf) {
    ((RecOutput *)output)->label = p > 0.5f;
    *conf = conf_of(p);
    return 0;
}

static int eng_policy(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_RECOGNIZE) return -1;
    return rec_answer(sigmoid(policy_logit(((const RecInput *)input)->x)), output, conf);
}

static int eng_neural(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_RECOGNIZE) return -1;
    return rec_answer(sigmoid(mlp_forward(&g_neural, ((const RecInput *)input)->x, NULL, NULL)),
                      output, conf);
}

static int eng_general(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_RECOGNIZE) return -1;
    return rec_answer(sigmoid(mlp_forward(&g_general, ((const RecInput *)input)->x, NULL, NULL)),
                      output, conf);
}

static int eng_ensemble(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_RECOGNIZE) return -1;
    const float *x = ((const RecInput *)input)->x;
    float p = (sigmoid(mlp_forward(&g_general, x, NULL, NULL)) +
               sigmoid(mlp_forward(&g_ens_a, x, NULL, NULL)) +
               sigmoid(mlp_forward(&g_ens_b, x, NULL, NULL)) +
               sigmoid(mlp_forward(&g_neural, x, NULL, NULL))) / 4.0f;
    return rec_answer(p, output, conf);
}

/* ---- planning ---- */

void plan_map(uint64_t seed, uint8_t *blocked) {
    uint64_t s = seed;
    for (int i = 0; i < GRID_CELLS; i++) blocked[i] = frand(&s) < 0.28f;
}

static int manhattan(int a, int b) {
    return abs(a % GRID_W - b % GRID_W) + abs(a / GRID_W - b / GRID_W);
}

void plan_sample(uint64_t *rng, const uint8_t *blocked, PlanInput *in) {
    for (;;) {
        int a = (int)(cog_rng(rng) % GRID_CELLS), b = (int)(cog_rng(rng) % GRID_CELLS);
        if (blocked[a] || blocked[b]) continue;
        int d = manhattan(a, b);
        if (d < 20 || d > 90) continue;
        in->blocked = blocked;
        in->start = (uint16_t)a;
        in->goal = (uint16_t)b;
        in->bound = (uint16_t)(d + (int)(cog_rng(rng) % (uint64_t)(d / 2 + 1)));
        return;
    }
}

static int neighbours(int c, int *out) {
    int x = c % GRID_W, y = c / GRID_W, n = 0;
    if (x > 0) out[n++] = c - 1;
    if (x < GRID_W - 1) out[n++] = c + 1;
    if (y > 0) out[n++] = c - GRID_W;
    if (y < GRID_H - 1) out[n++] = c + GRID_W;
    return n;
}

/* Scratch shared by the planners; stamps avoid clearing per call. */
static uint32_t g_stamp, g_seen[GRID_CELLS];
static int32_t g_parent[GRID_CELLS], g_cost[GRID_CELLS];
static int32_t g_heap[GRID_CELLS * 4], g_key[GRID_CELLS * 4];
static int g_heap_n;

static void heap_push(int cell, int key) {
    int i = g_heap_n++;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (g_key[p] <= key) break;
        g_heap[i] = g_heap[p];
        g_key[i] = g_key[p];
        i = p;
    }
    g_heap[i] = cell;
    g_key[i] = key;
}

static int heap_pop(void) {
    int top = g_heap[0];
    int cell = g_heap[--g_heap_n], key = g_key[g_heap_n], i = 0;
    for (;;) {
        int l = 2 * i + 1, r = l + 1, m = i;
        int mk = key;
        if (l < g_heap_n && g_key[l] < mk) m = l, mk = g_key[l];
        if (r < g_heap_n && g_key[r] < mk) m = r;
        if (m == i) break;
        g_heap[i] = g_heap[m];
        g_key[i] = g_key[m];
        i = m;
    }
    g_heap[i] = cell;
    g_key[i] = key;
    return top;
}

static int emit_path(const PlanInput *in, PlanOutput *out) {
    int n = 0;
    for (int c = in->goal; c != in->start; c = g_parent[c]) {
        if (n >= PATH_MAX_STEPS) return -1;
        out->path[n++] = (uint16_t)c;
    }
    for (int i = 0; i < n / 2; i++) {
        uint16_t t = out->path[i];
        out->path[i] = out->path[n - 1 - i];
        out->path[n - 1 - i] = t;
    }
    out->len = (uint16_t)n;
    return 0;
}

/* The check a verifiable planner runs before it claims a path. */
static int path_valid(const PlanInput *in, const PlanOutput *out) {
    if (out->len > in->bound || out->len == 0) return 0;
    int at = in->start;
    for (int i = 0; i < out->len; i++) {
        int c = out->path[i];
        if (c >= GRID_CELLS || in->blocked[c] || manhattan(at, c) != 1) return 0;
        at = c;
    }
    return at == in->goal;
}

/* Best-first search on key = weight_g * g + h, stopping after `budget`
 * expansions. weight_g 0 is greedy. */
static int heuristic_plan(const PlanInput *in, PlanOutput *out, uint32_t *conf, int weight_g,
                          int weight_h, uint32_t budget) {
    g_stamp++;
    g_heap_n = 0;
    g_seen[in->start] = g_stamp;
    g_cost[in->start] = 0;
    heap_push(in->start, weight_h * manhattan(in->start, in->goal));
    uint32_t expanded = 0;
    out->status = -1;
    out->len = 0;
    *conf = 0;
    while (g_heap_n && expanded < budget) {
        int c = heap_pop();
        expanded++;
        if (c == in->goal) {
            if (emit_path(in, out) == 0 && path_valid(in, out)) {
                out->status = 1;
                *conf = RX_COG_CONF_ONE;
            } else {
                out->len = 0;
            }
            return 0;
        }
        int nb[4], k = neighbours(c, nb);
        for (int i = 0; i < k; i++) {
            int d = nb[i];
            if (in->blocked[d]) continue;
            int g = g_cost[c] + 1;
            if (g_seen[d] == g_stamp && g_cost[d] <= g) continue;
            if (g > in->bound) continue;
            g_seen[d] = g_stamp;
            g_cost[d] = g;
            g_parent[d] = c;
            if (g_heap_n < GRID_CELLS * 4) heap_push(d, weight_g * g + weight_h * manhattan(d, in->goal));
        }
    }
    return 0;
}

static int eng_greedy(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_PLAN) return -1;
    const PlanInput *in = input;
    return heuristic_plan(in, output, conf, 0, 1, 4u * in->bound);
}

static int eng_focused(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_PLAN) return -1;
    const PlanInput *in = input;
    return heuristic_plan(in, output, conf, 4, 5, 24u * in->bound);
}

/* Exhaustive breadth-first exploration to depth `bound`: exact either way. */
static int32_t g_queue[GRID_CELLS];

static int eng_jspace(void *ctx, uint32_t op, const void *input, void *output, uint32_t *conf) {
    (void)ctx;
    if (op != RX_COG_OP_PLAN) return -1;
    const PlanInput *in = input;
    PlanOutput *out = output;
    g_stamp++;
    int head = 0, tail = 0;
    g_queue[tail++] = in->start;
    g_seen[in->start] = g_stamp;
    g_cost[in->start] = 0;
    out->len = 0;
    while (head < tail) {
        int c = g_queue[head++];
        if (c == in->goal) {
            if (emit_path(in, out) != 0 || !path_valid(in, out)) return -1;
            out->status = 1;
            *conf = RX_COG_CONF_ONE;
            return 0;
        }
        if (g_cost[c] >= in->bound) continue;
        int nb[4], k = neighbours(c, nb);
        for (int i = 0; i < k; i++) {
            int d = nb[i];
            if (in->blocked[d] || g_seen[d] == g_stamp) continue;
            g_seen[d] = g_stamp;
            g_cost[d] = g_cost[c] + 1;
            g_parent[d] = c;
            g_queue[tail++] = d;
        }
    }
    out->status = 0;
    *conf = RX_COG_CONF_ONE;
    return 0;
}

int plan_truth(const PlanInput *in) {
    PlanInput wide = *in;
    wide.bound = GRID_CELLS - 1;
    static PlanOutput out;
    uint32_t conf;
    if (eng_jspace(NULL, RX_COG_OP_PLAN, &wide, &out, &conf) != 0) return -2;
    return out.status == 1 ? out.len : -1;
}

int plan_correct(const PlanInput *in, const PlanOutput *out, int truth_len) {
    if (out->status == 1) return path_valid(in, out);
    if (out->status == 0) return truth_len < 0 || truth_len > in->bound;
    return 0;
}

/* ---- registration ---- */

RxCogEngineFn cog_engine_fn(uint32_t id) {
    switch (id) {
    case ENG_POLICY: return eng_policy;
    case ENG_NEURAL: return eng_neural;
    case ENG_GENERAL: return eng_general;
    case ENG_ENSEMBLE: return eng_ensemble;
    case ENG_GREEDY: return eng_greedy;
    case ENG_FOCUSED: return eng_focused;
    case ENG_JSPACE: return eng_jspace;
    }
    return NULL;
}

int cog_register_all(RxCogRouter *r) {
    const uint32_t REC = 1u << RX_COG_OP_RECOGNIZE, PLAN = 1u << RX_COG_OP_PLAN;
    const uint64_t grid = GRID_CELLS * 16ull;
    struct {
        RxCogDeclaration d;
        uint32_t reference;
    } t[] = {
        {{ENG_POLICY, RX_COG_CLASS_SMALL_POLICY, REC, 0, (2 * COG_DIM + 1) * 4ull, 0, 0,
          RX_COG_PREC_APPROX, 0}, 0},
        {{ENG_NEURAL, RX_COG_CLASS_NEURAL_MODULE, REC, 0, mlp_params(&g_neural) * 4ull, 0, 0,
          RX_COG_PREC_APPROX, 0}, 0},
        {{ENG_GENERAL, RX_COG_CLASS_GENERAL, REC, RX_COG_HW_CPU_P, mlp_params(&g_general) * 4ull, 0, 0,
          RX_COG_PREC_APPROX, 0}, REC},
        {{ENG_ENSEMBLE, RX_COG_CLASS_ENSEMBLE, REC, RX_COG_HW_CPU_P,
          (mlp_params(&g_general) + 2 * mlp_params(&g_ens_a) + mlp_params(&g_neural)) * 4ull, 0, 0,
          RX_COG_PREC_APPROX, 0}, 0},
        {{ENG_GREEDY, RX_COG_CLASS_DETERMINISTIC, PLAN, 0, grid, 0, 256, RX_COG_PREC_EXACT, 1}, 0},
        {{ENG_FOCUSED, RX_COG_CLASS_DETERMINISTIC, PLAN, 0, grid, 0, 1024, RX_COG_PREC_EXACT, 1}, 0},
        {{ENG_JSPACE, RX_COG_CLASS_JSPACE, PLAN, 0, grid, 0, GRID_CELLS, RX_COG_PREC_EXACT, 1}, PLAN},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        int rc = rx_route_register(r, &t[i].d, cog_engine_fn(t[i].d.cognitive_engine_id), NULL,
                                   t[i].reference);
        if (rc != RX_COG_OK) return rc;
    }
    return RX_COG_OK;
}
