/*
 * aienos_cap_abi_test.c -- OMEGA_AIENOS_CAP_ABI: src/runtime/aienos_cap.h, the
 * hand-kept copy the runtime compiles against, must match the pinned AIENOS
 * header aienos_capability.h (aienos.lock), the one libaienos_capability.a is
 * built from. Built and run only by tests/runtime/aienos_cap_abi.sh
 * (make test-aienos-cap-abi), which writes, for the copy under test:
 *   omega_view.h     the copy, unchanged
 *   omega_protos.h   the copy without its struct definitions and include guard
 *   aca_list.h       X-lists of its struct typedefs, their fields, its functions
 *   aca_exist.h      #ifndef/#error for each valued macro it defines
 *   aca_rename.h     O_ names for its struct and function-pointer typedefs,
 *   aca_unrename.h   o_ names for its functions (and the #undefs)
 *
 * Every check is a compile-time error; the run repeats the layout rows only to
 * count them for the GATE lines:
 *   1. existence: each function and valued macro of the copy is declared by
 *      the pinned header;
 *   2. layout: each struct typedef has the same sizeof and _Alignof, and each
 *      field the same offsetof, sizeof and kind (integer type and signedness,
 *      pointer or array element type, or which struct), so an equal-size
 *      reordered or retyped struct is refused;
 *   3. constants: the copy's macros redefine the pinned ones; a different body
 *      is a redefinition error (-Werror);
 *   4. prototypes: the copy's function and function-pointer declarations are
 *      redeclared over the pinned types; a different parameter or return type
 *      is a conflicting-types error.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "aienos_capability.h"
#include "aca_exist.h"
#include "aca_list.h"

/* 1. functions: taken from the pinned declarations (the copy is not in yet). */
#define ACA_EXISTS(f) (void)&f; n++;
static int pinned_functions(void) {
    int n = 0;
    ACA_FUNCS(ACA_EXISTS)
    return n;
}

/* The copy, under O_/o_ names; its macros redefine the pinned ones (check 3). */
#include "aca_rename.h"
#include "omega_view.h"
#include "aca_unrename.h"

/* 2. layout. */
#define ACA_INT_KINDS(X)                                                                   \
    X(_Bool, 1) X(char, 2) X(signed char, 3) X(unsigned char, 4) X(short, 5)               \
    X(unsigned short, 6) X(int, 7) X(unsigned int, 8) X(long, 9) X(unsigned long, 10)      \
    X(long long, 11) X(unsigned long long, 12) X(float, 13) X(double, 14) X(long double, 15)
#define ACA_KIND_INT(T, k) T: k, T *: 100 + k, const T *: 200 + k,
#define ACA_KIND_STRUCT(T, k)                                                              \
    T: 1000 + k, O_##T: 1000 + k, T *: 1100 + k, O_##T *: 1100 + k,                        \
    const T *: 1200 + k, const O_##T *: 1200 + k,
#define ACA_KIND(e)                                                                        \
    _Generic((e), ACA_INT_KINDS(ACA_KIND_INT) ACA_STRUCTS(ACA_KIND_STRUCT) default: 0)
#define ACA_M(T, f) (((T *)0)->f)

#define ACA_STRUCT_ASSERT(T, k)                                                            \
    _Static_assert(sizeof(O_##T) == sizeof(T), "sizeof " #T);                              \
    _Static_assert(_Alignof(O_##T) == _Alignof(T), "_Alignof " #T);
#define ACA_FIELD_ASSERT(T, f)                                                             \
    _Static_assert(offsetof(O_##T, f) == offsetof(T, f), "offsetof " #T "." #f);           \
    _Static_assert(sizeof ACA_M(O_##T, f) == sizeof ACA_M(T, f), "sizeof " #T "." #f);     \
    _Static_assert(ACA_KIND(ACA_M(O_##T, f)) == ACA_KIND(ACA_M(T, f)), "kind " #T "." #f);
ACA_STRUCTS(ACA_STRUCT_ASSERT)
ACA_FIELDS(ACA_FIELD_ASSERT)

typedef struct {
    const char *what;
    size_t copy[3], pinned[3]; /* offset or alignment, size, kind */
} AcaRow;
#define ACA_STRUCT_ROW(T, k)                                                               \
    {#T, {_Alignof(O_##T), sizeof(O_##T), 1000 + k}, {_Alignof(T), sizeof(T), 1000 + k}},
#define ACA_FIELD_ROW(T, f)                                                                \
    {#T "." #f,                                                                            \
     {offsetof(O_##T, f), sizeof ACA_M(O_##T, f), (size_t)ACA_KIND(ACA_M(O_##T, f))},      \
     {offsetof(T, f), sizeof ACA_M(T, f), (size_t)ACA_KIND(ACA_M(T, f))}},
static const AcaRow ROWS[] = {ACA_STRUCTS(ACA_STRUCT_ROW) ACA_FIELDS(ACA_FIELD_ROW)};

/* 3 and 4: the copy's declarations again, now over the pinned types. */
#include "omega_protos.h"

int main(void) {
    int rows = (int)(sizeof ROWS / sizeof ROWS[0]), ok_rows = 0;
    for (int i = 0; i < rows; i++) {
        const AcaRow *r = &ROWS[i];
        if (r->copy[0] == r->pinned[0] && r->copy[1] == r->pinned[1] &&
            r->copy[2] == r->pinned[2])
            ok_rows++;
        else
            fprintf(stderr, "FAIL %s: copy %zu/%zu/%zu pinned %zu/%zu/%zu\n", r->what,
                    r->copy[0], r->copy[1], r->copy[2], r->pinned[0], r->pinned[1],
                    r->pinned[2]);
    }
    int fns = pinned_functions();
    printf("GATE OMEGA_AIENOS_CAP_ABI_LAYOUT_PASS %s %d/%d\n", ok_rows == rows ? "PASS" : "FAIL",
           ok_rows, rows);
    printf("GATE OMEGA_AIENOS_CAP_ABI_DECLS_PASS %s %d/%d\n", fns == ACA_FUNC_COUNT ? "PASS" : "FAIL",
           fns + ACA_MACRO_COUNT,
           ACA_FUNC_COUNT + ACA_MACRO_COUNT);
    return ok_rows == rows && fns == ACA_FUNC_COUNT ? 0 : 1;
}
