/* POLYGLOT-0 lane B1: Omega-X 2-bit crumb realization, run kernel in hand
 * AArch64 assembly (GNU as 2.42). spec/polyglot-0.md sections 1 and 5.
 *
 * Derivation: written by hand from the crumb code and the algorithm. gcc -S
 * output of the MA-3 C kernels was NOT generated or consulted. Not
 * compiler-derived.
 *
 * int omx_asm_crumb_run(const oma_rz_plan *p, const int8_t *x, int32_t *y)
 *   p->m at [p,#0], p->n at [p,#8], p->mem at [p,#16] (static-asserted in
 *   src/polyglot/omx_asm.c).
 *   Packed layout (identical to MA-3 R2c_crumb): per row, ceil(n/64) chunks
 *   of 16 bytes (row_bytes = 16*ceil(n/64)); bits 2k..2k+1 of byte j of
 *   chunk c hold weight c*64 + 16k + j as a 2-bit two's-complement code
 *   (+1 = 01, 0 = 00, -1 = 11); columns >= n are code 00.
 *   Returns 0 (OMA_RZ_OK) or -1 (OMA_RZ_E_ARG) for NULL p/x/y/mem or an
 *   empty shape.
 *
 * Decode: w_k = (int8)(b << (6 - 2k)) >> 6 (arithmetic): shl + sshr for
 *   k = 0..2, a single sshr for k = 3. Lane j of decoded vector k pairs with
 *   x[c*64 + 16k + j], so each decoded vector goes straight into SDOT with
 *   one 16-byte x vector: 7 shifts + 4 SDOT per 64 weights per row.
 * Rows in blocks of 4 (x loaded once per chunk per block, 8 accumulators
 *   v24-v31), leftover rows one at a time.
 * x tail: if n % 64 != 0 the last chunk's x is copied once per call into a
 *   zeroed 64-byte stack buffer, so x is never read past x[n-1]. Weight
 *   reads stay inside m*row_bytes; y writes are exactly m int32.
 * Registers: v0-v7, v16-v31 only (d8-d15 untouched), x0-x16. 64 bytes of
 *   stack. int32 accumulation is exact for n <= OMA_RZ_MAX_N.
 */
	.arch armv8.6-a+dotprod+i8mm
	.text

/* one row, one 64-weight chunk: wv = 16 packed bytes, x in v0-v3 */
	.macro CROW wv, A, B, t0, t1, t2, t3
	shl	\t0\().16b, \wv\().16b, #6
	shl	\t1\().16b, \wv\().16b, #4
	shl	\t2\().16b, \wv\().16b, #2
	sshr	\t3\().16b, \wv\().16b, #6
	sshr	\t0\().16b, \t0\().16b, #6
	sshr	\t1\().16b, \t1\().16b, #6
	sshr	\t2\().16b, \t2\().16b, #6
	sdot	\B\().4s, \t3\().16b, v3.16b
	sdot	\A\().4s, \t0\().16b, v0.16b
	sdot	\B\().4s, \t1\().16b, v1.16b
	sdot	\A\().4s, \t2\().16b, v2.16b
	.endm

/* four rows (x10..x13), one chunk */
	.macro CHUNK4
	ldr	q4, [x10], #16
	ldr	q5, [x11], #16
	ldr	q6, [x12], #16
	ldr	q7, [x13], #16
	CROW	v4, v24, v28, v16, v17, v18, v19
	CROW	v5, v25, v29, v20, v21, v22, v23
	CROW	v6, v26, v30, v16, v17, v18, v19
	CROW	v7, v27, v31, v20, v21, v22, v23
	.endm

	.p2align 6
	.globl	omx_asm_crumb_run
	.type	omx_asm_crumb_run, %function
omx_asm_crumb_run:
	cbz	x0, .Lcr_err
	cbz	x1, .Lcr_err
	cbz	x2, .Lcr_err
	ldp	x3, x4, [x0]		// x3 = m, x4 = n
	ldr	x5, [x0, #16]		// x5 = packed base of current row block
	cbz	x5, .Lcr_err
	cbz	x3, .Lcr_err
	cbz	x4, .Lcr_err

	sub	sp, sp, #64
	movi	v0.16b, #0
	stp	q0, q0, [sp]
	stp	q0, q0, [sp, #32]
	lsr	x6, x4, #6		// x6 = full chunks
	ands	x8, x4, #63		// x8 = tail columns (0..63)
	cinc	x7, x6, ne		// chunks per row
	lsl	x7, x7, #4		// x7 = row_bytes
	cbz	x8, 1f
	add	x9, x1, x6, lsl #6	// x + 64*full
	mov	x10, sp
	mov	x14, x8
0:	ldrb	w11, [x9], #1
	strb	w11, [x10], #1
	subs	x14, x14, #1
	b.ne	0b
1:

.Lcr_blk4:
	cmp	x3, #4
	b.lo	.Lcr_rows1
	movi	v24.16b, #0
	movi	v25.16b, #0
	movi	v26.16b, #0
	movi	v27.16b, #0
	movi	v28.16b, #0
	movi	v29.16b, #0
	movi	v30.16b, #0
	movi	v31.16b, #0
	mov	x9, x1
	mov	x10, x5
	add	x11, x10, x7
	add	x12, x11, x7
	add	x13, x12, x7
	cbz	x6, 1f
	mov	x14, x6
0:	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [x9], #64
	CHUNK4
	subs	x14, x14, #1
	b.ne	0b
1:	cbz	x8, 2f
	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [sp]
	CHUNK4
2:	add	v24.4s, v24.4s, v28.4s
	add	v25.4s, v25.4s, v29.4s
	add	v26.4s, v26.4s, v30.4s
	add	v27.4s, v27.4s, v31.4s
	addp	v24.4s, v24.4s, v25.4s
	addp	v26.4s, v26.4s, v27.4s
	addp	v24.4s, v24.4s, v26.4s	// [r0 r1 r2 r3]
	str	q24, [x2], #16
	add	x5, x5, x7, lsl #2
	sub	x3, x3, #4
	b	.Lcr_blk4

.Lcr_rows1:
	cbz	x3, .Lcr_ok
.Lcr_row1:
	movi	v24.16b, #0
	movi	v28.16b, #0
	mov	x9, x1
	mov	x10, x5
	cbz	x6, 1f
	mov	x14, x6
0:	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [x9], #64
	ldr	q4, [x10], #16
	CROW	v4, v24, v28, v16, v17, v18, v19
	subs	x14, x14, #1
	b.ne	0b
1:	cbz	x8, 2f
	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [sp]
	ldr	q4, [x10], #16
	CROW	v4, v24, v28, v16, v17, v18, v19
2:	add	v24.4s, v24.4s, v28.4s
	addv	s24, v24.4s
	str	s24, [x2], #4
	add	x5, x5, x7
	subs	x3, x3, #1
	b.ne	.Lcr_row1
.Lcr_ok:
	add	sp, sp, #64
	mov	w0, #0
	ret

.Lcr_err:
	mov	w0, #-1
	ret
	.size	omx_asm_crumb_run, . - omx_asm_crumb_run

	.section .note.GNU-stack, "", %progbits
