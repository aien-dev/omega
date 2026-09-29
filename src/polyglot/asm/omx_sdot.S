/* POLYGLOT-0 lane B1: Omega-X int8 SDOT realization, run kernel in hand
 * AArch64 assembly (GNU as 2.42). spec/polyglot-0.md sections 1 and 5.
 *
 * Derivation: written by hand from the algorithm (y = W.x, W row-major int8
 * ternary, x int8, y int32, exact). gcc -S output of the MA-3 C kernels was
 * NOT generated or consulted. Not compiler-derived.
 *
 * int omx_asm_sdot_run(const oma_rz_plan *p, const int8_t *x, int32_t *y)
 *   p->m at [p,#0], p->n at [p,#8], p->mem at [p,#16] (static-asserted in
 *   src/polyglot/omx_asm.c). Weights: m rows of n bytes, no padding (the
 *   same packed form as MA-3 R1_sdot).
 *   Returns 0 (OMA_RZ_OK) or -1 (OMA_RZ_E_ARG) for NULL p/x/y/mem or an
 *   empty shape.
 *
 * Method:
 *   n >= 16: rows in blocks of 4. Per 64 columns: x loaded once (4 q regs),
 *     each row's 64 bytes loaded and SDOT-accumulated into two accumulators
 *     (8 total, v24-v31). Then 16-column steps. The final n%16 columns are
 *     done by one overlapping 16-byte load ending exactly at column n-1,
 *     with x lanes that were already counted masked to zero (mask table),
 *     so no byte outside W, x or y is ever read or written.
 *     Rows left over (m%4) use the same scheme with one row and four
 *     accumulators.
 *   n < 16: plain scalar multiply-add per element.
 *   Accumulation is int32; |partial| <= 128*n <= INT32_MAX for n <= max_n
 *   (OMA_RZ_MAX_N), so every lane sum is exact.
 * Registers: only v0-v7 and v16-v31 (d8-d15 are callee-saved and untouched),
 *   x0-x17. Leaf function, no stack use. Unaligned x and y are fine
 *   (ld1/ldr/str on normal memory).
 */
	.arch armv8.6-a+dotprod+i8mm
	.text

	.p2align 6
	.globl	omx_asm_sdot_run
	.type	omx_asm_sdot_run, %function
omx_asm_sdot_run:
	cbz	x0, .Lsd_err
	cbz	x1, .Lsd_err
	cbz	x2, .Lsd_err
	ldp	x3, x4, [x0]		// x3 = m, x4 = n
	ldr	x5, [x0, #16]		// x5 = weight base of current row block
	cbz	x5, .Lsd_err
	cbz	x3, .Lsd_err
	cbz	x4, .Lsd_err
	cmp	x4, #16
	b.lo	.Lsd_scalar

	lsr	x6, x4, #6		// x6 = number of 64-column steps
	ubfx	x7, x4, #4, #2		// x7 = remaining 16-column steps (0..3)
	and	x8, x4, #15		// x8 = tail t (0..15)
	adrp	x16, .Lsd_mask
	add	x16, x16, :lo12:.Lsd_mask
	add	x16, x16, x8		// mask: lanes >= 16-t are 0xff
	mov	x15, #16
	sub	x15, x15, x8		// back-off for the overlapping tail load

.Lsd_blk4:
	cmp	x3, #4
	b.lo	.Lsd_rows1
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
	add	x11, x10, x4
	add	x12, x11, x4
	add	x13, x12, x4
	cbz	x6, 1f
	mov	x14, x6
0:	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [x9], #64
	ld1	{v4.16b, v5.16b, v6.16b, v7.16b}, [x10], #64
	ld1	{v16.16b, v17.16b, v18.16b, v19.16b}, [x11], #64
	sdot	v24.4s, v4.16b, v0.16b
	sdot	v28.4s, v5.16b, v1.16b
	sdot	v25.4s, v16.16b, v0.16b
	sdot	v29.4s, v17.16b, v1.16b
	ld1	{v20.16b, v21.16b, v22.16b, v23.16b}, [x12], #64
	sdot	v24.4s, v6.16b, v2.16b
	sdot	v28.4s, v7.16b, v3.16b
	sdot	v25.4s, v18.16b, v2.16b
	sdot	v29.4s, v19.16b, v3.16b
	ld1	{v4.16b, v5.16b, v6.16b, v7.16b}, [x13], #64
	sdot	v26.4s, v20.16b, v0.16b
	sdot	v30.4s, v21.16b, v1.16b
	sdot	v27.4s, v4.16b, v0.16b
	sdot	v31.4s, v5.16b, v1.16b
	sdot	v26.4s, v22.16b, v2.16b
	sdot	v30.4s, v23.16b, v3.16b
	sdot	v27.4s, v6.16b, v2.16b
	sdot	v31.4s, v7.16b, v3.16b
	subs	x14, x14, #1
	b.ne	0b
1:	cbz	x7, 3f
	mov	x14, x7
2:	ldr	q0, [x9], #16
	ldr	q4, [x10], #16
	ldr	q5, [x11], #16
	ldr	q6, [x12], #16
	ldr	q7, [x13], #16
	sdot	v24.4s, v4.16b, v0.16b
	sdot	v25.4s, v5.16b, v0.16b
	sdot	v26.4s, v6.16b, v0.16b
	sdot	v27.4s, v7.16b, v0.16b
	subs	x14, x14, #1
	b.ne	2b
3:	cbz	x8, 4f
	sub	x9, x9, x15		// now x + n - 16
	sub	x10, x10, x15		// now row + n - 16
	sub	x11, x11, x15
	sub	x12, x12, x15
	sub	x13, x13, x15
	ldr	q0, [x9]
	ldr	q1, [x16]
	and	v0.16b, v0.16b, v1.16b	// zero the lanes already counted
	ldr	q4, [x10]
	ldr	q5, [x11]
	ldr	q6, [x12]
	ldr	q7, [x13]
	sdot	v28.4s, v4.16b, v0.16b
	sdot	v29.4s, v5.16b, v0.16b
	sdot	v30.4s, v6.16b, v0.16b
	sdot	v31.4s, v7.16b, v0.16b
4:	add	v24.4s, v24.4s, v28.4s
	add	v25.4s, v25.4s, v29.4s
	add	v26.4s, v26.4s, v30.4s
	add	v27.4s, v27.4s, v31.4s
	addp	v24.4s, v24.4s, v25.4s	// [r0 r0 r1 r1]
	addp	v26.4s, v26.4s, v27.4s	// [r2 r2 r3 r3]
	addp	v24.4s, v24.4s, v26.4s	// [r0 r1 r2 r3]
	str	q24, [x2], #16
	add	x5, x5, x4, lsl #2
	sub	x3, x3, #4
	b	.Lsd_blk4

.Lsd_rows1:
	cbz	x3, .Lsd_ok
.Lsd_row1:
	movi	v24.16b, #0
	movi	v25.16b, #0
	movi	v26.16b, #0
	movi	v27.16b, #0
	mov	x9, x1
	mov	x10, x5
	cbz	x6, 1f
	mov	x14, x6
0:	ld1	{v0.16b, v1.16b, v2.16b, v3.16b}, [x9], #64
	ld1	{v4.16b, v5.16b, v6.16b, v7.16b}, [x10], #64
	sdot	v24.4s, v4.16b, v0.16b
	sdot	v25.4s, v5.16b, v1.16b
	sdot	v26.4s, v6.16b, v2.16b
	sdot	v27.4s, v7.16b, v3.16b
	subs	x14, x14, #1
	b.ne	0b
1:	cbz	x7, 3f
	mov	x14, x7
2:	ldr	q0, [x9], #16
	ldr	q4, [x10], #16
	sdot	v24.4s, v4.16b, v0.16b
	subs	x14, x14, #1
	b.ne	2b
3:	cbz	x8, 4f
	sub	x9, x9, x15
	sub	x10, x10, x15
	ldr	q0, [x9]
	ldr	q1, [x16]
	and	v0.16b, v0.16b, v1.16b
	ldr	q4, [x10]
	sdot	v25.4s, v4.16b, v0.16b
4:	add	v24.4s, v24.4s, v25.4s
	add	v26.4s, v26.4s, v27.4s
	add	v24.4s, v24.4s, v26.4s
	addv	s24, v24.4s
	str	s24, [x2], #4
	add	x5, x5, x4
	subs	x3, x3, #1
	b.ne	.Lsd_row1
.Lsd_ok:
	mov	w0, #0
	ret

	/* n < 16: scalar; rows are contiguous so x5 walks all of W. */
.Lsd_scalar:
0:	mov	w6, #0
	mov	x9, x1
	mov	x14, x4
1:	ldrsb	w10, [x5], #1
	ldrsb	w11, [x9], #1
	madd	w6, w10, w11, w6
	subs	x14, x14, #1
	b.ne	1b
	str	w6, [x2], #4
	subs	x3, x3, #1
	b.ne	0b
	mov	w0, #0
	ret

.Lsd_err:
	mov	w0, #-1
	ret
	.size	omx_asm_sdot_run, . - omx_asm_sdot_run

	.section .rodata
	.p2align 4
.Lsd_mask:
	.fill	16, 1, 0x00
	.fill	16, 1, 0xff

	.section .note.GNU-stack, "", %progbits
