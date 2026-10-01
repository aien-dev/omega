/*
 * osc_ir.h -- OSC-1 typed IR ("Flow IR" v0 slice). The contract between the
 * OSC-1 front end (src/compiler/osc_front.*), the reference interpreter
 * (src/compiler/osc_interp.*) and the AArch64 back end (src/compiler/osc_cg.*).
 *
 * OSC-1 slice; not a general Omega compiler; no self-hosting.
 * Design and semantics: docs/osc/OSC-1-DESIGN.md (normative). This header
 * states the data layout; the design doc states the meaning.
 *
 * Shape: a unit is an ordered list of functions. A function is a CFG of basic
 * blocks over typed virtual registers (vregs). Every vreg has exactly one
 * OscType. Blocks end in exactly one terminator. Calls may only target a
 * function with a smaller index (defined earlier in the unit), so there is no
 * recursion. All loops carry a static trip-count bound enforced by an explicit
 * counter + OSC_I_TRAP emitted by the front end (the IR itself has no
 * unbounded construct the back end needs to know about).
 *
 * Value representation (both interpreter and native code, RuntimeLayout):
 *  - every scalar vreg holds a 64-bit canonical value: unsigned types are
 *    zero-extended, signed types sign-extended, bool is 0 or 1;
 *  - a REF vreg (owned array handle or borrow) holds the address of element 0;
 *    elements are stored as 8-byte canonical values regardless of element width.
 */
#ifndef OSC_IR_H
#define OSC_IR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define OSC_MAX_FUNCS     32
#define OSC_MAX_PARAMS     6     /* x0..x5; x7 carries the hidden runtime ctx */
#define OSC_MAX_VREGS    480     /* stack frame must fit SUB SP imm12 */
#define OSC_MAX_INSNS   4096     /* per function */
#define OSC_MAX_BLOCKS   512     /* per function */
#define OSC_MAX_ARRAY_LEN 64     /* elements per unique allocation */
#define OSC_NAME_MAX      64
#define OSC_CLAUSE_MAX   256     /* requires/ensures source text (recorded; enforced by the front end, OSC-2) */
#define OSC_MAX_STRUCTS   16     /* OSC-2 structs (docs/osc/OSC-2-DESIGN.md section 2) */
#define OSC_MAX_FIELDS    16     /* fields per struct */

/* ---- types ------------------------------------------------------------ */
typedef enum {
    OSC_T_VOID = 0,
    OSC_T_BOOL,
    OSC_T_U8, OSC_T_U16, OSC_T_U32, OSC_T_U64,     /* wrap modulo 2^w (V0 compatible) */
    OSC_T_I8, OSC_T_I16, OSC_T_I32, OSC_T_I64,     /* checked: overflow traps (III.3) */
    OSC_T_REF                                      /* pointer to array element 0 */
} OscScalar;

typedef enum {
    OSC_REF_NONE = 0,
    OSC_REF_OWN,       /* unique owner of an allocation: own [T; N]   */
    OSC_REF_SHARED,    /* shared borrow:                &[T; N]       */
    OSC_REF_MUT        /* mutable borrow:               &mut [T; N]   */
} OscRefKind;

typedef struct {
    OscScalar s;          /* for OSC_T_REF: the elements are `elem`        */
    OscRefKind ref;       /* OSC_REF_NONE unless s == OSC_T_REF            */
    OscScalar elem;       /* element scalar type (integer or bool)         */
    uint16_t len;         /* static array length N, 1..OSC_MAX_ARRAY_LEN;  */
                          /*   for a struct ref: the struct's cell count  */
    uint8_t sid;          /* OSC-2 structs: 0 = array ref (or scalar);     */
                          /*   k+1 = ref to OscUnit.structs[k] (elem VOID) */
} OscType;

/* ---- instructions ----------------------------------------------------- */
typedef enum {
    OSC_I_CONST = 1, /* dst = imm (already canonical for dst type)                   */
    OSC_I_MOV,       /* dst = a  (same type)                                         */
    OSC_I_BIN,       /* dst = a <bop> b ; type of a,b,dst identical (except shifts:  */
                     /*   b may be any integer type; amount is a runtime value that  */
                     /*   must be < width else TRAP_SHIFT)                           */
    OSC_I_UN,        /* dst = <uop> a                                                */
    OSC_I_CMP,       /* dst(bool) = a <cc> b ; a,b same integer or bool type          */
    OSC_I_CAST,      /* dst = (T)a ; checked: value must be representable in T        */
                     /*   else TRAP_CAST. bool<->int not allowed (front end refuses) */
    OSC_I_ALLOC,     /* dst(REF OWN, elem, len) = unique allocation, every element   */
                     /*   initialised to vreg a (elem type). OOM -> TRAP_OOM          */
    OSC_I_RELEASE,   /* destroy owner a: zero the elements, return them to the pool  */
    OSC_I_LOAD,      /* dst(elem) = a[b] ; b any integer type; if b<0 or b>=len:      */
                     /*   TRAP_BOUNDS                                                */
    OSC_I_STORE,     /* a[b] = c ; same bounds rule; a is REF OWN or REF MUT         */
    OSC_I_CALL,      /* dst = func[callee](args[0..nargs)) ; dst may be -1 for void  */
    OSC_I_TRAP,      /* deterministic trap with code imm (unconditional)             */
    /* terminators */
    OSC_I_BR,        /* goto blk_t                                                   */
    OSC_I_CBR,       /* if a(bool) goto blk_t else goto blk_f                        */
    OSC_I_RET,       /* return a (or nothing if a == -1 and function is void)        */
    /* appended for OSC-2 structs (docs/osc/OSC-2-DESIGN.md section 2) */
    OSC_I_FLOAD,     /* dst = field imm of struct ref a; for an array field, element */
                     /*   b (any integer type, else -1); b out of range: TRAP_BOUNDS */
    OSC_I_FSTORE     /* field imm of struct ref a (element b, as FLOAD) = c ;        */
                     /*   a is REF OWN or REF MUT                                    */
} OscOp;

typedef enum {
    OSC_B_ADD = 1, OSC_B_SUB, OSC_B_MUL,
    OSC_B_DIV,       /* b == 0 -> TRAP_DIV0; signed MIN/-1 -> TRAP_OVERFLOW (truncating) */
    OSC_B_REM,       /* b == 0 -> TRAP_DIV0; sign follows dividend (C semantics)         */
    OSC_B_AND, OSC_B_OR, OSC_B_XOR,
    OSC_B_SHL,       /* unsigned: wrap; signed: result not representable -> TRAP_OVERFLOW */
    OSC_B_SHR        /* unsigned: logical; signed: arithmetic                           */
} OscBinOp;

typedef enum {
    OSC_U_NEG = 1,   /* signed only; MIN -> TRAP_OVERFLOW. unsigned: wrap (0 - a)  */
    OSC_U_BNOT,      /* bitwise not, then canonicalised to the type                 */
    OSC_U_LNOT       /* bool only                                                   */
} OscUnOp;

typedef enum {
    OSC_C_EQ = 1, OSC_C_NE, OSC_C_LT, OSC_C_LE, OSC_C_GT, OSC_C_GE
    /* signedness comes from the operand type */
} OscCmp;

/* Trap codes are part of the observable semantics: interpreter and native
 * code must report the same code for the same input. */
typedef enum {
    OSC_TRAP_NONE = 0,
    OSC_TRAP_OVERFLOW = 1,   /* checked signed arithmetic out of range           */
    OSC_TRAP_DIV0 = 2,
    OSC_TRAP_BOUNDS = 3,     /* dynamic out-of-bounds index                      */
    OSC_TRAP_LOOP_BOUND = 4, /* a loop exceeded its static trip-count bound      */
    OSC_TRAP_CAST = 5,       /* checked conversion out of range                  */
    OSC_TRAP_OOM = 6,        /* allocation pool exhausted                        */
    OSC_TRAP_SHIFT = 7,      /* shift amount >= width or negative                */
    OSC_TRAP_RUNTIME = 8,    /* runtime invariant broken (double release, bad    */
                             /*   handle): must never happen for checked code    */
    /* Appended for OSC-2 contracts (docs/osc/OSC-2-DESIGN.md section 1).
     * Existing codes are never renumbered. */
    OSC_TRAP_REQUIRES = 9,   /* a `requires` clause evaluated to false at entry  */
    OSC_TRAP_ENSURES = 10    /* an `ensures` clause evaluated to false at return */
} OscTrap;

/* Highest trap code; arrays indexed by trap code have OSC_TRAP_MAX + 1 entries. */
#define OSC_TRAP_MAX OSC_TRAP_ENSURES

typedef struct {
    uint8_t op;          /* OscOp */
    uint8_t sub;         /* OscBinOp / OscUnOp / OscCmp */
    int16_t dst;         /* vreg or -1 */
    int16_t a, b, c;     /* vregs or -1 */
    int16_t blk_t, blk_f;/* BR/CBR targets */
    int16_t callee;      /* CALL */
    uint8_t nargs;       /* CALL */
    int16_t args[OSC_MAX_PARAMS];
    uint64_t imm;        /* CONST value (canonical) / TRAP code */
    uint32_t line;       /* source line for diagnostics/debug, 0 if synthetic */
} OscInsn;

typedef struct {
    uint32_t first;      /* index into OscFunc.insns */
    uint32_t count;      /* last insn is the terminator */
} OscBlock;

typedef struct {
    char name[OSC_NAME_MAX];
    uint8_t nparams;     /* params are vregs 0..nparams-1 */
    OscType ret;         /* scalar or VOID; OSC-1 never returns a REF */
    OscType vtype[OSC_MAX_VREGS];
    uint16_t nvregs;
    OscInsn insns[OSC_MAX_INSNS];
    uint32_t ninsns;
    OscBlock blocks[OSC_MAX_BLOCKS];
    uint16_t nblocks;    /* block 0 is the entry */
    char requires_text[OSC_CLAUSE_MAX];  /* source text; checks are lowered into the CFG (OSC-2) */
    char ensures_text[OSC_CLAUSE_MAX];
} OscFunc;

/* OSC-2 structs: fixed layout, no padding. Field k occupies cells
 * [off, off + max(alen, 1)) of the struct's allocation; every cell is one
 * 8-byte canonical value; offsets are the prefix sums of the field cell
 * counts in declaration order; ncells = the total (1..OSC_MAX_ARRAY_LEN). */
typedef struct {
    char name[OSC_NAME_MAX];
    OscScalar s;         /* integer scalar or bool */
    uint16_t alen;       /* 0 = scalar field; N = inline array [s; N] */
    uint16_t off;        /* first cell */
} OscField;

typedef struct {
    char name[OSC_NAME_MAX];
    uint8_t nfields;     /* 1..OSC_MAX_FIELDS */
    uint16_t ncells;     /* size in bytes = 8 * ncells */
    OscField fields[OSC_MAX_FIELDS];
} OscStruct;

typedef struct {
    OscFunc funcs[OSC_MAX_FUNCS];
    uint16_t nfuncs;
    OscStruct structs[OSC_MAX_STRUCTS];  /* OSC-2; 0 structs = OSC-1 encoding */
    uint8_t nstructs;
} OscUnit;

/* Structural validation (types agree, vregs in range, every block ends in one
 * terminator and has no terminator before its end, branch targets valid,
 * calls only to earlier functions with matching arity/types, REF kinds legal
 * for LOAD/STORE/RELEASE). 0 ok, -1 invalid with message in err. Both the
 * interpreter and the back end refuse a unit that does not validate. */
int osc_ir_validate(const OscUnit *u, char *err, size_t n);

/* Canonical encoding of the IR (WireLayout, declared little-endian, no
 * padding, no pointers, no text except function names and contract text) and
 * its SHA-256 digest. Identity of a compiled unit = this digest. */
int osc_ir_encode(const OscUnit *u, uint8_t *buf, size_t cap, size_t *len);
int osc_ir_digest(const OscUnit *u, uint8_t out[32]);

/* Width in bits of an integer scalar (8..64); 1 for bool; 0 otherwise. */
unsigned osc_scalar_width(OscScalar s);
bool osc_scalar_signed(OscScalar s);
bool osc_scalar_is_int(OscScalar s);
const char *osc_scalar_name(OscScalar s);

#endif /* OSC_IR_H */
