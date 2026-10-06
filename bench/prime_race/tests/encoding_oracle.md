# Encoding oracle for BW_IR_LOP3_LUT, BW_IR_SHF_L_U32, BW_IR_IMAD_HI_U32

Offline oracle only (nothing here is linked into AIEN). Recorded 2026-10-05 on the Spark with
`Cuda compilation tools, release 13.0, V13.0.88`:

```sh
nvcc -arch=sm_121 -cubin -O3 -o ops.cubin ops.cu
cuobjdump -sass ops.cubin
```

Source (`ops.cu`, scratch dir outside omega):

```cuda
// Offline encoding oracle for omega BW_IR_LOP3_LUT / SHF_L_U32 / IMAD_HI_U32. Never linked into AIEN.
extern "C" __global__ void k_or(unsigned *o, const unsigned *a) { unsigned x=a[threadIdx.x], y=a[threadIdx.x+64], z=a[threadIdx.x+128]; o[threadIdx.x] = x | y; o[threadIdx.x+64] = ~(x | y); o[threadIdx.x+128] = (x & y) ^ z; o[threadIdx.x+192] = ~(x|y) & z; }
extern "C" __global__ void k_shl(unsigned *o, const unsigned *a) { unsigned x=a[threadIdx.x], n=a[threadIdx.x+64]; o[threadIdx.x] = x << n; }
extern "C" __global__ void k_shl1(unsigned *o, const unsigned *a) { unsigned n=a[threadIdx.x]; o[threadIdx.x] = 1u << n; }
extern "C" __global__ void k_hi(unsigned *o, const unsigned *a) { unsigned x=a[threadIdx.x], y=a[threadIdx.x+64]; o[threadIdx.x] = __umulhi(x, y); }
extern "C" __global__ void k_hi2(unsigned *o, const unsigned *a) { unsigned x=a[threadIdx.x], y=a[threadIdx.x+64], z=a[threadIdx.x+128]; o[threadIdx.x] = __umulhi(x, y) + z; }
```

Instructions used as golden words in `omega_blackwell_verify_codegen_fixtures_intops`
(src/omega_blackwell_codegen.c). Low word, then high word; the high word's top 23 bits are
the control word, which our scheduler writes and the fixtures do not compare.

```
/*00b0*/ IMAD.HI.U32 R9, R9, R0, R6 ; /* 0x0000000009097227 */	 /* 0x008fca00078e0006 */
/*0090*/ IMAD.HI.U32 R7, R0, R7, RZ ; /* 0x0000000700077227 */	 /* 0x008fca00078e00ff */
/*0090*/ SHF.L.U32 R7, R9, R2, RZ ; /* 0x0000000209077219 */	 /* 0x008fca00000006ff */
/*0090*/ SHF.L.U32 R7, R0, R7, RZ ; /* 0x0000000700077219 */	 /* 0x008fca00000006ff */
/*00a0*/ LOP3.LUT R9, R7, R0, RZ, 0xfc, !PT ; /* 0x0000000007097212 */	 /* 0x008fc400078efcff */
/*00b0*/ LOP3.LUT R11, R6, R7, R0, 0x78, !PT ; /* 0x00000007060b7212 */	 /* 0x010fc600078e7800 */
/*00d0*/ LOP3.LUT R13, R6, R7, R0, 0x10, !PT ; /* 0x00000007060d7212 */	 /* 0x000fe400078e1000 */
/*00e0*/ LOP3.LUT R7, RZ, R9, RZ, 0x33, !PT ; /* 0x00000009ff077212 */	 /* 0x000fe200078e33ff */
```

The whole sieve kernel is also decoded by `nvdisasm -b SM121` in gb10_sieve_host_test
(every word decodes; every BRA target equals the IR target).
