extern "C" __global__ void k_umulhi_plus_c(unsigned *o, unsigned a, unsigned b, unsigned c) { o[threadIdx.x] = __umulhi(a, b) + c; }
extern "C" __global__ void k_wide_hi(unsigned *o, unsigned a, unsigned b, unsigned long long c64) { o[threadIdx.x] = (unsigned)(((unsigned long long)a * b + c64) >> 32); }
extern "C" __global__ void k_wide_hi_c32(unsigned *o, unsigned a, unsigned b, unsigned c) { o[threadIdx.x] = (unsigned)(((unsigned long long)a * b + c) >> 32); }
