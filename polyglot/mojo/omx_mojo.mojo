# POLYGLOT-0 lane B3: Omega-X (exact ternary GEMV y = W.x, int32 out) in Mojo 1.0.
# spec/polyglot-0.md section 1. Grace CPU only, no GPU, no Python.
#
# Exported with the C ABI and linked as a plain object file into C
# (src/polyglot/omx_mojo.c wraps these as oma_rz_impl entries).
#
# Two representations, same layouts as MA-3:
#   int8 row-major (as R1_sdot): 4 rows x 64 bytes per step, 8 SDOT accumulators.
#   2-bit crumb (as R2c_crumb): per row, chunks of 64 weights in 16 bytes; bits
#     2k..2k+1 of byte j hold weight chunk*64 + 16k + j; +1 = 01, 0 = 00, -1 = 11.
#     Decode is a sign-extending shift pair (b << (6 - 2k)) >> 6 on SIMD[int8, 16].
#
# The int8 x int8 -> int32 dot step uses the AArch64 SDOT instruction through
# llvm_intrinsic: plain SIMD widening multiply compiles to smull/saddw (5 ops per
# 16 weights instead of 1), measured in the generated assembly.
#
# Pointers arrive from C; the caller (C wrapper) guarantees shape, non-NULL and
# non-overlap exactly as for every oma_rz_impl.

from std.sys import llvm_intrinsic

comptime I8P = Pointer[Int8, MutAnyOrigin]
comptime U8P = Pointer[UInt8, MutAnyOrigin]
comptime I32P = Pointer[Int32, MutAnyOrigin]
comptime V16 = SIMD[DType.int8, 16]
comptime A4 = SIMD[DType.int32, 4]

comptime E_TRIT = -2


@always_inline
def sdot(acc: A4, a: V16, b: V16) -> A4:
    return llvm_intrinsic["llvm.aarch64.neon.sdot.v4i32.v16i8", A4](acc, a, b)


@always_inline
def ld16(p: I8P, off: Int) -> V16:
    return p.unsafe_offset(off).unsafe_load[width=16]()


@always_inline
def tail_dot(row: I8P, x: I8P, j0: Int, n: Int) -> Int32:
    var acc: Int32 = 0
    for j in range(j0, n):
        acc += Int32(row.unsafe_offset(j).unsafe_load()) * Int32(
            x.unsafe_offset(j).unsafe_load()
        )
    return acc


# ---------------------------------------------------------------- null kernel
# Same signature as the run kernels; touches nothing. Used to measure the
# C -> Mojo call boundary (there is none beyond a plain C call: static link).
@export("omx_mj_null")
def omx_mj_null(w: I8P, m: Int, n: Int, x: I8P, y: I32P) abi("C") -> Int32:
    return 0


# ---------------------------------------------------------------- int8 pack
# Validate every weight and copy to dst (m*n bytes). Returns nnz (>= 0) or
# E_TRIT. On E_TRIT dst content is unspecified (caller frees it).
@export("omx_mj_i8_pack")
def omx_mj_i8_pack(w: I8P, total: Int, dst: I8P) abi("C") -> Int64:
    var nnz: Int64 = 0
    var i = 0
    var nz16 = SIMD[DType.int32, 16](0)
    while i + 16 <= total:
        var v = ld16(w, i)
        # v in {-1,0,1}  <=>  (v + 1) as uint8 <= 2
        var u = (v + 1).cast[DType.uint8]()
        if u.gt(2).reduce_or():
            return E_TRIT
        nz16 += v.ne(0).cast[DType.int32]()
        dst.unsafe_offset(i).unsafe_store(v)
        i += 16
    nnz = Int64(nz16.reduce_add())
    while i < total:
        var s = w.unsafe_offset(i).unsafe_load()
        if s < -1 or s > 1:
            return E_TRIT
        if s != 0:
            nnz += 1
        dst.unsafe_offset(i).unsafe_store(s)
        i += 1
    return nnz


# ---------------------------------------------------------------- int8 run
@always_inline
def i8_rows4(w: I8P, n: Int, x: I8P, y: I32P):
    var r0 = w
    var r1 = w.unsafe_offset(n)
    var r2 = w.unsafe_offset(2 * n)
    var r3 = w.unsafe_offset(3 * n)
    var a0 = A4(0)
    var a1 = A4(0)
    var a2 = A4(0)
    var a3 = A4(0)
    var b0 = A4(0)
    var b1 = A4(0)
    var b2 = A4(0)
    var b3 = A4(0)
    var j = 0
    while j + 64 <= n:
        var x0 = ld16(x, j)
        var x1 = ld16(x, j + 16)
        var x2 = ld16(x, j + 32)
        var x3 = ld16(x, j + 48)
        a0 = sdot(a0, ld16(r0, j), x0)
        a1 = sdot(a1, ld16(r1, j), x0)
        a2 = sdot(a2, ld16(r2, j), x0)
        a3 = sdot(a3, ld16(r3, j), x0)
        b0 = sdot(b0, ld16(r0, j + 16), x1)
        b1 = sdot(b1, ld16(r1, j + 16), x1)
        b2 = sdot(b2, ld16(r2, j + 16), x1)
        b3 = sdot(b3, ld16(r3, j + 16), x1)
        a0 = sdot(a0, ld16(r0, j + 32), x2)
        a1 = sdot(a1, ld16(r1, j + 32), x2)
        a2 = sdot(a2, ld16(r2, j + 32), x2)
        a3 = sdot(a3, ld16(r3, j + 32), x2)
        b0 = sdot(b0, ld16(r0, j + 48), x3)
        b1 = sdot(b1, ld16(r1, j + 48), x3)
        b2 = sdot(b2, ld16(r2, j + 48), x3)
        b3 = sdot(b3, ld16(r3, j + 48), x3)
        j += 64
    while j + 16 <= n:
        var x0 = ld16(x, j)
        a0 = sdot(a0, ld16(r0, j), x0)
        a1 = sdot(a1, ld16(r1, j), x0)
        a2 = sdot(a2, ld16(r2, j), x0)
        a3 = sdot(a3, ld16(r3, j), x0)
        j += 16
    y.unsafe_store((a0 + b0).reduce_add() + tail_dot(r0, x, j, n))
    y.unsafe_offset(1).unsafe_store((a1 + b1).reduce_add() + tail_dot(r1, x, j, n))
    y.unsafe_offset(2).unsafe_store((a2 + b2).reduce_add() + tail_dot(r2, x, j, n))
    y.unsafe_offset(3).unsafe_store((a3 + b3).reduce_add() + tail_dot(r3, x, j, n))


@always_inline
def i8_row1(r: I8P, n: Int, x: I8P) -> Int32:
    var a0 = A4(0)
    var a1 = A4(0)
    var a2 = A4(0)
    var a3 = A4(0)
    var j = 0
    while j + 64 <= n:
        a0 = sdot(a0, ld16(r, j), ld16(x, j))
        a1 = sdot(a1, ld16(r, j + 16), ld16(x, j + 16))
        a2 = sdot(a2, ld16(r, j + 32), ld16(x, j + 32))
        a3 = sdot(a3, ld16(r, j + 48), ld16(x, j + 48))
        j += 64
    while j + 16 <= n:
        a0 = sdot(a0, ld16(r, j), ld16(x, j))
        j += 16
    return ((a0 + a1) + (a2 + a3)).reduce_add() + tail_dot(r, x, j, n)


@export("omx_mj_i8_run")
def omx_mj_i8_run(w: I8P, m: Int, n: Int, x: I8P, y: I32P) abi("C") -> Int32:
    var i = 0
    while i + 4 <= m:
        i8_rows4(w.unsafe_offset(i * n), n, x, y.unsafe_offset(i))
        i += 4
    while i < m:
        y.unsafe_offset(i).unsafe_store(i8_row1(w.unsafe_offset(i * n), n, x))
        i += 1
    return 0


# ---------------------------------------------------------------- crumb pack
# dst must hold m * ceil(n/64) * 16 zeroed bytes. Returns nnz or E_TRIT.
@export("omx_mj_crumb_pack")
def omx_mj_crumb_pack(w: I8P, m: Int, n: Int, dst: U8P) abi("C") -> Int64:
    var chunks = (n + 63) // 64
    var row_bytes = chunks * 16
    var nnz: Int64 = 0
    for i in range(m):
        var row = w.unsafe_offset(i * n)
        for c in range(chunks):
            for j in range(16):
                var byte: UInt8 = 0
                for k in range(4):
                    var col = c * 64 + 16 * k + j
                    if col < n:
                        var v = row.unsafe_offset(col).unsafe_load()
                        if v < -1 or v > 1:
                            return E_TRIT
                        if v != 0:
                            nnz += 1
                        byte |= (UInt8(v) & 3) << UInt8(2 * k)
                dst.unsafe_offset(i * row_bytes + c * 16 + j).unsafe_store(byte)
    return nnz


# ---------------------------------------------------------------- crumb run
@always_inline
def load_x64(x: I8P, n: Int, c0: Int) -> SIMD[DType.int8, 64]:
    if c0 + 64 <= n:
        return x.unsafe_offset(c0).unsafe_load[width=64]()
    var t = SIMD[DType.int8, 64](0)
    for k in range(n - c0):
        t[k] = x.unsafe_offset(c0 + k).unsafe_load()
    return t


@always_inline
def crumb_row(bv: V16, xv: SIMD[DType.int8, 64], mut a: A4, mut b: A4):
    a = sdot(a, (bv << 6) >> 6, xv.slice[16, offset=0]())
    b = sdot(b, (bv << 4) >> 6, xv.slice[16, offset=16]())
    a = sdot(a, (bv << 2) >> 6, xv.slice[16, offset=32]())
    b = sdot(b, bv >> 6, xv.slice[16, offset=48]())


@export("omx_mj_crumb_run")
def omx_mj_crumb_run(w: I8P, m: Int, n: Int, x: I8P, y: I32P) abi("C") -> Int32:
    var chunks = (n + 63) // 64
    var row_bytes = chunks * 16
    var i = 0
    while i + 4 <= m:
        var r0 = w.unsafe_offset(i * row_bytes)
        var a0 = A4(0)
        var a1 = A4(0)
        var a2 = A4(0)
        var a3 = A4(0)
        var b0 = A4(0)
        var b1 = A4(0)
        var b2 = A4(0)
        var b3 = A4(0)
        for c in range(chunks):
            var xv = load_x64(x, n, c * 64)
            var o = c * 16
            crumb_row(ld16(r0, o), xv, a0, b0)
            crumb_row(ld16(r0, row_bytes + o), xv, a1, b1)
            crumb_row(ld16(r0, 2 * row_bytes + o), xv, a2, b2)
            crumb_row(ld16(r0, 3 * row_bytes + o), xv, a3, b3)
        var yp = y.unsafe_offset(i)
        yp.unsafe_store((a0 + b0).reduce_add())
        yp.unsafe_offset(1).unsafe_store((a1 + b1).reduce_add())
        yp.unsafe_offset(2).unsafe_store((a2 + b2).reduce_add())
        yp.unsafe_offset(3).unsafe_store((a3 + b3).reduce_add())
        i += 4
    while i < m:
        var r0 = w.unsafe_offset(i * row_bytes)
        var a0 = A4(0)
        var b0 = A4(0)
        for c in range(chunks):
            crumb_row(ld16(r0, c * 16), load_x64(x, n, c * 64), a0, b0)
        y.unsafe_offset(i).unsafe_store((a0 + b0).reduce_add())
        i += 1
    return 0
