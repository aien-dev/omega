// Offline encoding/placement oracle for omega #308 (BSSY/BSYNC). Never linked into AIEN.
extern "C" __global__ void k_diamond(unsigned *o, const unsigned *a) {
    unsigned x = a[threadIdx.x], y;
    if (x & 1) y = x * 3u + 7u; else { y = x >> 2; y = y * y + 1u; y ^= 0x5a5a; }
    y += __shfl_down_sync(0xffffffffu, y, 1);
    o[threadIdx.x] = y;
}
extern "C" __global__ void k_loop(unsigned *o, const unsigned *a) {
    unsigned j = a[threadIdx.x], p = a[threadIdx.x + 64], acc = 0;
    while (j < 32) { acc |= 1u << j; j += p; }
    acc += __shfl_down_sync(0xffffffffu, acc, 1);
    o[threadIdx.x] = acc;
}
extern "C" __global__ void k_nested(unsigned *o, const unsigned *a) {
    unsigned x = a[threadIdx.x], acc = 0;
    if (x & 1) { unsigned j = x >> 1; while (j < 32) { acc += j; j += (x & 7) + 1; } }
    acc += __shfl_down_sync(0xffffffffu, acc, 1);
    __syncthreads();
    o[threadIdx.x] = acc;
}
extern "C" __global__ void k_break(unsigned *o, const unsigned *a) {
    unsigned x = a[threadIdx.x], acc = 0;
    for (unsigned i = 0; i < 64; i++) { acc += a[i]; if (acc > x) break; }
    o[threadIdx.x] = acc + __shfl_down_sync(0xffffffffu, acc, 1);
}
