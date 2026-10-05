/*
 * CR-5 tests: omega_tensor_reduce_axes and omega_tensor_sum_to_shape
 * (src/tensor/omega_tensor.h, docs/tensor/M20_OMEGA_TENSOR.md). Included by
 * tests/test_omega_tensor.c after its helpers (g, mk, rd, CHECK, ref_tree,
 * rand_mod, mismatches, prod). The reference is independent of the library:
 * plain row-major index loops plus the test's own copy of the frozen E1
 * reduction tree (ref_tree), applied in descending axis order.
 */

/* One axis of a dense row-major array: out has shape with that axis removed
 * (keepdims does not change the dense layout). */
static void rm_ref_axis(OmegaTensorReduceOp op, const float *x, uint32_t rank, const uint64_t *shape,
                        uint32_t axis, float *out) {
    uint64_t outer = prod(axis, shape), n = shape[axis];
    uint64_t inner = prod(rank - axis - 1, shape + axis + 1);
    float *row = malloc((size_t)n * sizeof(float));
    for (uint64_t o = 0; o < outer; o++)
        for (uint64_t i = 0; i < inner; i++) {
            for (uint64_t r = 0; r < n; r++) row[r] = x[(o * n + r) * inner + i];
            out[o * inner + i] = ref_tree(op, row, (size_t)n);
        }
    free(row);
}

/* Reference multi-axis reduce in the given axis sequence. Returns a malloc'd
 * array; *orank / oshape get the result shape. */
static float *rm_ref_multi(OmegaTensorReduceOp op, const float *x, uint32_t rank, const uint64_t *shape,
                           uint32_t n, const uint32_t *seq, bool keepdims, uint32_t *orank, uint64_t *oshape) {
    uint64_t cur_shape[OMEGA_TENSOR_MAX_RANK];
    uint32_t cur_rank = rank;
    memcpy(cur_shape, shape, rank * sizeof(uint64_t));
    float *cur = malloc((size_t)prod(rank, shape) * sizeof(float));
    memcpy(cur, x, (size_t)prod(rank, shape) * sizeof(float));
    for (uint32_t k = 0; k < n; k++) {
        uint32_t a = seq[k];
        float *nx = malloc((size_t)(prod(cur_rank, cur_shape) / cur_shape[a]) * sizeof(float));
        rm_ref_axis(op, cur, cur_rank, cur_shape, a, nx);
        free(cur);
        cur = nx;
        if (keepdims) cur_shape[a] = 1;
        else {
            for (uint32_t d = a; d + 1 < cur_rank; d++) cur_shape[d] = cur_shape[d + 1];
            cur_rank--;
        }
    }
    *orank = cur_rank;
    memcpy(oshape, cur_shape, cur_rank * sizeof(uint64_t));
    return cur;
}

static bool rm_shape_is(OmegaTensor t, uint32_t rank, const uint64_t *shape) {
    OmegaTensorInfo in;
    if (omega_tensor_info(g, t, &in) || in.rank != rank) return false;
    for (uint32_t d = 0; d < rank; d++) if (in.shape[d] != shape[d]) return false;
    return true;
}

static void test_reduce_axes(void) {
    uint64_t s[4] = {3, 5, 4, 6};
    float x[360];
    fill(x, 360, rand_mod);
    OmegaTensor X = mk(4, s, x);
    int order_matters = 0;
    for (uint32_t mask = 1; mask < 16; mask++) {
        uint32_t asc[4], desc[4], n = 0;
        for (uint32_t d = 0; d < 4; d++) if (mask & (1u << d)) asc[n++] = d;
        for (uint32_t i = 0; i < n; i++) desc[i] = asc[n - 1 - i];
        for (int op = 0; op < OMEGA_TR_COUNT; op++) {
            if (op == OMEGA_TR_MEAN && n > 1) continue;
            for (int kd = 0; kd < 2; kd++) {
                uint32_t er;
                uint64_t es[4];
                float *ex = rm_ref_multi((OmegaTensorReduceOp)op, x, 4, s, n, desc, kd, &er, es);
                uint64_t cnt = prod(er, es);
                /* axes passed in ascending order: the op must sort them */
                OmegaTensor O;
                int rc = omega_tensor_reduce_axes(g, (OmegaTensorReduceOp)op, X, n, asc, kd, &O);
                float *got = rc ? NULL : rd(O);
                CHECK(!rc && got && rm_shape_is(O, er, es) && mismatches(ex, got, (size_t)cnt) == 0,
                      "reduce_axes mask %u op %d keepdims %d vs reference (rc %d)", mask, op, kd, rc);
                /* equality with the explicit chain of single-axis reduce calls */
                OmegaTensor cur = X, nx;
                int crc = 0;
                for (uint32_t i = 0; i < n && !crc; i++) {
                    crc = omega_tensor_reduce(g, (OmegaTensorReduceOp)op, cur, desc[i], kd, &nx);
                    if (i > 0) omega_tensor_release(g, cur);
                    if (!crc) cur = nx;
                }
                bool eq = false;
                if (!crc && !rc) omega_tensor_value_equal(g, cur, O, &eq);
                CHECK(!crc && eq, "reduce_axes mask %u op %d keepdims %d == descending reduce chain", mask, op, kd);
                if (!crc) omega_tensor_release(g, cur);
                /* the declared order is observable: ascending gives other bits */
                if (op == OMEGA_TR_SUM && kd && n > 1) {
                    uint32_t ar;
                    uint64_t as[4];
                    float *alt = rm_ref_multi(OMEGA_TR_SUM, x, 4, s, n, asc, true, &ar, as);
                    if (mismatches(ex, alt, (size_t)cnt)) order_matters++;
                    free(alt);
                }
                free(got);
                free(ex);
                if (!rc) omega_tensor_release(g, O);
            }
        }
    }
    CHECK(order_matters > 0, "test data distinguishes descending from ascending order (%d subsets)", order_matters);
    /* full reduction to rank 0 */
    {
        uint32_t all[4] = {2, 0, 3, 1};
        OmegaTensor O;
        int rc = omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 4, all, false, &O);
        CHECK(!rc && rm_shape_is(O, 0, s), "reduce_axes over every axis gives rank 0");
        if (!rc) omega_tensor_release(g, O);
    }
    /* MEAN over one axis equals omega_tensor_reduce; over two it is refused */
    {
        uint32_t one[1] = {2}, two[2] = {1, 2};
        OmegaTensor O1, O2;
        bool eq = false;
        int r1 = omega_tensor_reduce_axes(g, OMEGA_TR_MEAN, X, 1, one, true, &O1);
        int r2 = omega_tensor_reduce(g, OMEGA_TR_MEAN, X, 2, true, &O2);
        if (!r1 && !r2) omega_tensor_value_equal(g, O1, O2, &eq);
        CHECK(!r1 && !r2 && eq, "MEAN one axis == omega_tensor_reduce MEAN");
        if (!r1) omega_tensor_release(g, O1);
        if (!r2) omega_tensor_release(g, O2);
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_MEAN, X, 2, two, false, &O1) == OMEGA_TENSOR_ERR_BAD_ARGS,
              "MEAN over two axes refused (no mean of means)");
    }
    /* naxes 0: dense copy, same value */
    {
        OmegaTensor O;
        bool eq = false;
        int rc = omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 0, NULL, false, &O);
        if (!rc) omega_tensor_value_equal(g, X, O, &eq);
        CHECK(!rc && eq, "naxes 0 is a copy");
        if (!rc) omega_tensor_release(g, O);
    }
    /* strided view input equals its contiguous copy */
    {
        uint32_t perm[4] = {3, 1, 0, 2}, ax2[2] = {0, 3};
        uint64_t st[4] = {0, 1, 0, 0}, step[4] = {1, 2, 1, 2};
        OmegaTensor P, V, C, O1, O2;
        int rc = omega_tensor_permute(g, X, perm, &P);
        rc = rc ? rc : omega_tensor_slice(g, P, st, (uint64_t[4]){6, 5, 3, 4}, step, &V);
        rc = rc ? rc : omega_tensor_contiguous(g, V, &C);
        bool eq = false;
        if (!rc) {
            int a = omega_tensor_reduce_axes(g, OMEGA_TR_SUM, V, 2, ax2, true, &O1);
            int b = omega_tensor_reduce_axes(g, OMEGA_TR_SUM, C, 2, ax2, true, &O2);
            if (!a && !b) omega_tensor_value_equal(g, O1, O2, &eq);
            CHECK(!a && !b && eq, "reduce_axes of strided view == of its copy");
            if (!a) omega_tensor_release(g, O1);
            if (!b) omega_tensor_release(g, O2);
            omega_tensor_release(g, C);
            omega_tensor_release(g, V);
            omega_tensor_release(g, P);
        } else CHECK(0, "view setup rc %d", rc);
    }
    /* refusals: no tensor is made */
    {
        uint32_t lt0, ls0, lt1, ls1;
        omega_tensor_live_counts(g, &lt0, &ls0);
        uint32_t dup[2] = {1, 1}, oor[1] = {4}, five[5] = {0, 1, 2, 3, 0};
        OmegaTensor O;
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 2, dup, false, &O) == OMEGA_TENSOR_ERR_AXIS,
              "duplicate axis refused");
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 1, oor, false, &O) == OMEGA_TENSOR_ERR_AXIS,
              "out-of-range axis refused");
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 5, five, false, &O) == OMEGA_TENSOR_ERR_AXIS,
              "more axes than rank refused");
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 1, NULL, false, &O) == OMEGA_TENSOR_ERR_BAD_ARGS,
              "NULL axes refused");
        CHECK(omega_tensor_reduce_axes(g, (OmegaTensorReduceOp)9, X, 1, oor, false, &O) == OMEGA_TENSOR_ERR_BAD_ARGS,
              "unknown reduce op refused");
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, X, 1, dup, false, NULL) == OMEGA_TENSOR_ERR_BAD_ARGS,
              "NULL out refused");
        float sc = 1.0f;
        OmegaTensor S = mk(0, NULL, &sc);
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, S, 0, NULL, false, &O) == OMEGA_TENSOR_ERR_RANK,
              "rank-0 input refused");
        omega_tensor_release(g, S);
        uint16_t h[6] = {0};
        uint64_t s23[2] = {2, 3};
        OmegaTensor F;
        if (!omega_tensor_from_data(g, OMEGA_DT_F16, 2, s23, h, &F)) {
            CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, F, 1, dup, false, &O) == OMEGA_TENSOR_ERR_DTYPE,
                  "f16 reduce_axes refused");
            omega_tensor_release(g, F);
        } else CHECK(0, "f16 create");
        OmegaTensor Y = mk(4, s, x);
        omega_tensor_release(g, Y);
        CHECK(omega_tensor_reduce_axes(g, OMEGA_TR_SUM, Y, 1, dup, false, &O) == OMEGA_TENSOR_ERR_STALE,
              "stale handle refused");
        omega_tensor_live_counts(g, &lt1, &ls1);
        CHECK(lt0 == lt1 && ls0 == ls1, "refusals made no tensor");
    }
    omega_tensor_release(g, X);
}

/* x (rank rx, shape sx) full of exact small integers, broadcast to S, summed
 * back: every element must be exactly x * (prod(S) / prod(sx)). */
static void rm_unbroadcast_case(uint32_t rx, const uint64_t *sx, uint32_t rs, const uint64_t *S) {
    uint64_t nx = prod(rx, sx), ns = prod(rs, S);
    float *x = malloc((size_t)nx * sizeof(float));
    for (uint64_t i = 0; i < nx; i++) x[i] = (float)((int)(i % 7) - 3);
    OmegaTensor X = mk(rx, sx, x), B, O;
    int rc = omega_tensor_broadcast_to(g, X, rs, S, &B);
    int rc2 = rc ? rc : omega_tensor_sum_to_shape(g, B, rx, sx, &O);
    float *got = rc2 ? NULL : rd(O);
    size_t bad = got ? 0 : 1;
    for (uint64_t i = 0; got && i < nx; i++)
        bad += !same(got[i], (float)(((int)(i % 7) - 3) * (int)(ns / nx)));
    CHECK(!rc && !rc2 && got && rm_shape_is(O, rx, sx) && bad == 0,
          "sum_to_shape(broadcast_to(x, S), shape(x)) exact, rank %u -> %u (rc %d/%d)", rs, rx, rc, rc2);
    free(got);
    if (!rc2) omega_tensor_release(g, O);
    if (!rc) omega_tensor_release(g, B);
    omega_tensor_release(g, X);
    free(x);
}

static void test_sum_to_shape(void) {
    { uint64_t sx[3] = {3, 1, 4}, S[4] = {2, 3, 5, 4}; rm_unbroadcast_case(3, sx, 4, S); }
    { uint64_t sx[1] = {4}, S[3] = {2, 3, 4}; rm_unbroadcast_case(1, sx, 3, S); }
    { uint64_t S[2] = {3, 2}; rm_unbroadcast_case(0, NULL, 2, S); }
    { uint64_t sx[2] = {1, 4}, S[2] = {3, 4}; rm_unbroadcast_case(2, sx, 2, S); }
    { uint64_t sx[3] = {2, 1, 3}, S[5] = {1, 2, 2, 5, 3}; rm_unbroadcast_case(3, sx, 5, S); }
    { uint64_t sx[2] = {1, 1}, S[3] = {4, 3, 2}; rm_unbroadcast_case(2, sx, 3, S); }
    /* random floats: equals the declared chain (axis 2 keepdims, then axis 0) */
    uint64_t s[4] = {2, 3, 5, 4}, tgt[3] = {3, 1, 4};
    float x[120];
    fill(x, 120, rand_mod);
    OmegaTensor T = mk(4, s, x), O, A, Bc;
    int rc = omega_tensor_sum_to_shape(g, T, 3, tgt, &O);
    int ra = omega_tensor_reduce(g, OMEGA_TR_SUM, T, 2, true, &A);
    int rb = ra ? ra : omega_tensor_reduce(g, OMEGA_TR_SUM, A, 0, false, &Bc);
    bool eq = false;
    if (!rc && !rb) omega_tensor_value_equal(g, O, Bc, &eq);
    CHECK(!rc && !rb && eq && rm_shape_is(O, 3, tgt), "sum_to_shape == descending single-axis SUM chain");
    {
        uint32_t er;
        uint64_t es[4];
        uint32_t seq[2] = {2, 0};
        bool kd[2] = {true, false};
        /* reference: axis 2 with keepdims, then axis 0 removed */
        float *e1 = rm_ref_multi(OMEGA_TR_SUM, x, 4, s, 1, seq, kd[0], &er, es);
        uint64_t s2[4] = {2, 3, 1, 4};
        float *e2 = rm_ref_multi(OMEGA_TR_SUM, e1, 4, s2, 1, seq + 1, kd[1], &er, es);
        float *got = rc ? NULL : rd(O);
        CHECK(got && mismatches(e2, got, 12) == 0, "sum_to_shape vs independent reference");
        free(got);
        free(e1);
        free(e2);
    }
    if (!rc) omega_tensor_release(g, O);
    if (!rb) omega_tensor_release(g, Bc);
    if (!ra) omega_tensor_release(g, A);
    /* same shape: a copy; size-1 leading axes of t are dropped, bits kept */
    {
        bool e = false;
        rc = omega_tensor_sum_to_shape(g, T, 4, s, &O);
        if (!rc) omega_tensor_value_equal(g, T, O, &e);
        CHECK(!rc && e, "sum_to_shape to own shape is a copy");
        if (!rc) omega_tensor_release(g, O);
        float y[6] = {omega_bits_to_float(0x80000000U), 1.0f, 2.0f, 3.0f, 4.0f, 5.0f};
        uint64_t s1[3] = {1, 2, 3}, s23[2] = {2, 3};
        OmegaTensor Y = mk(3, s1, y);
        float *gy = NULL;
        rc = omega_tensor_sum_to_shape(g, Y, 2, s23, &O);
        if (!rc) gy = rd(O);
        CHECK(!rc && gy && rm_shape_is(O, 2, s23) && mismatches(gy, y, 6) == 0,
              "size-1 leading axis dropped, bits (incl. -0) kept");
        free(gy);
        if (!rc) omega_tensor_release(g, O);
        omega_tensor_release(g, Y);
    }
    /* refusals: no tensor is made */
    {
        uint32_t lt0, ls0, lt1, ls1;
        omega_tensor_live_counts(g, &lt0, &ls0);
        uint64_t bad1[3] = {3, 2, 4}, bad2[2] = {4, 5}, r5[5] = {1, 2, 3, 5, 4}, z[3] = {3, 0, 4},
                 r9[9] = {1, 1, 1, 1, 1, 1, 1, 1, 1};
        CHECK(omega_tensor_sum_to_shape(g, T, 3, bad1, &O) == OMEGA_TENSOR_ERR_SHAPE,
              "incompatible size (2 vs 5) refused");
        CHECK(omega_tensor_sum_to_shape(g, T, 2, bad2, &O) == OMEGA_TENSOR_ERR_SHAPE,
              "incompatible trailing shape refused");
        CHECK(omega_tensor_sum_to_shape(g, T, 5, r5, &O) == OMEGA_TENSOR_ERR_SHAPE,
              "target rank above t's rank refused");
        CHECK(omega_tensor_sum_to_shape(g, T, 3, z, &O) == OMEGA_TENSOR_ERR_SHAPE, "zero dim refused");
        CHECK(omega_tensor_sum_to_shape(g, T, 9, r9, &O) == OMEGA_TENSOR_ERR_RANK, "rank 9 refused");
        CHECK(omega_tensor_sum_to_shape(g, T, 3, NULL, &O) == OMEGA_TENSOR_ERR_BAD_ARGS, "NULL shape refused");
        CHECK(omega_tensor_sum_to_shape(g, T, 3, tgt, NULL) == OMEGA_TENSOR_ERR_BAD_ARGS, "NULL out refused");
        uint16_t h[6] = {0};
        uint64_t s23[2] = {2, 3}, s3[1] = {3};
        OmegaTensor F;
        if (!omega_tensor_from_data(g, OMEGA_DT_F16, 2, s23, h, &F)) {
            CHECK(omega_tensor_sum_to_shape(g, F, 1, s3, &O) == OMEGA_TENSOR_ERR_DTYPE, "f16 sum_to_shape refused");
            omega_tensor_release(g, F);
        } else CHECK(0, "f16 create");
        OmegaTensor Y = mk(4, s, x);
        omega_tensor_release(g, Y);
        CHECK(omega_tensor_sum_to_shape(g, Y, 3, tgt, &O) == OMEGA_TENSOR_ERR_STALE, "stale handle refused");
        omega_tensor_live_counts(g, &lt1, &ls1);
        CHECK(lt0 == lt1 && ls0 == ls1, "sum_to_shape refusals made no tensor");
    }
    omega_tensor_release(g, T);
}
