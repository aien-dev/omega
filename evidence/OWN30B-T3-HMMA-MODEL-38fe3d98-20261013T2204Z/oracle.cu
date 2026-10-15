// Offline encoding oracle only (never shipped): nvcc 13 -arch=sm_121 -cubin, then cuobjdump -sass.
#include <stdint.h>
extern "C" __global__ void hmma_indep(const uint32_t *a, const uint32_t *b, float *c) {
  int l = threadIdx.x;
  uint32_t a0=a[l],a1=a[l+32],a2=a[l+64],a3=a[l+96],b0=b[l],b1=b[l+32],b2=b[l+64],b3=b[l+96];
  float d[4][4] = {};
  #pragma unroll
  for (int it = 0; it < 4; it++) {
    #pragma unroll
    for (int j = 0; j < 4; j++) {
      uint32_t bb0 = j&1 ? b2 : b0, bb1 = j&2 ? b3 : b1;
      asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
        : "+f"(d[j][0]), "+f"(d[j][1]), "+f"(d[j][2]), "+f"(d[j][3]) : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(bb0), "r"(bb1));
    }
  }
  for (int j = 0; j < 4; j++) for (int q = 0; q < 4; q++) c[(j*4+q)*32+l] = d[j][q];
}
extern "C" __global__ void qmma_e4m3(const uint32_t *a, const uint32_t *b, float *c) {
  int l = threadIdx.x;
  uint32_t a0=a[l],a1=a[l+32],a2=a[l+64],a3=a[l+96],b0=b[l],b1=b[l+32];
  float d0=0,d1=0,d2=0,d3=0;
  asm volatile("mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};"
    : "+f"(d0), "+f"(d1), "+f"(d2), "+f"(d3) : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1));
  c[l]=d0; c[l+32]=d1; c[l+64]=d2; c[l+96]=d3;
}
extern "C" __global__ void cvt_e4m3(const uint16_t *a, uint32_t *c) {
  int l = threadIdx.x; uint32_t r;
  asm volatile("{ .reg .b16 t; mov.b16 t, %1; cvt.rn.f16x2.e4m3x2 %0, t; }" : "=r"(r) : "h"(a[l]));
  c[l] = r;
}
