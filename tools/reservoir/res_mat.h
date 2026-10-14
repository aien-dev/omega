/* Minimal reader for MATLAB MAT-file version 5 (little-endian), the format
 * of the 19 archived .mat files of Zenodo 10.5281/zenodo.17296788
 * (docs/reservoir/RESERVOIR_REFERENCE.md section 1). Reads what the
 * omega#440 Gate 2 tools need and refuses the rest by name:
 *   - top-level elements miMATRIX (14) or miCOMPRESSED (15, decoded by the
 *     in-house res_zlib_inflate with its Adler-32 check);
 *   - numeric classes double, single, int8..uint64 and logical, stored in any
 *     numeric MAT data type; values are converted to binary64 (exact for
 *     every stored type except 64-bit integers above 2^53, which refuse);
 *   - cell arrays (recursively);
 *   - char, struct, object, sparse and complex arrays are recorded with
 *     their class and dimensions but carry no data (RES_MAT_UNSUPPORTED if
 *     their values are requested).
 * Input is untrusted: every size is checked against the bytes present.
 * Data is column-major, as MATLAB stores it. */
#ifndef RES_MAT_H
#define RES_MAT_H

#include <stddef.h>
#include <stdint.h>

#define RES_MAT_MAX_DIMS 8u
#define RES_MAT_MAX_NAME 64u
#define RES_MAT_MAX_VARS 64u
#define RES_MAT_MAX_DEPTH 4

enum { /* mxClassID */
    RES_MX_CELL = 1, RES_MX_STRUCT = 2, RES_MX_OBJECT = 3, RES_MX_CHAR = 4, RES_MX_SPARSE = 5,
    RES_MX_DOUBLE = 6, RES_MX_SINGLE = 7, RES_MX_INT8 = 8, RES_MX_UINT8 = 9, RES_MX_INT16 = 10,
    RES_MX_UINT16 = 11, RES_MX_INT32 = 12, RES_MX_UINT32 = 13, RES_MX_INT64 = 14, RES_MX_UINT64 = 15,
    RES_MX_OPAQUE = 17 /* MATLAB class objects (MCOS); name only, no dims, no data */
};

typedef enum {
    RES_MAT_OK = 0,
    RES_MAT_ERR_IO = -1,
    RES_MAT_ERR_FORMAT = -2,    /* header, tag, size or type not as the format requires */
    RES_MAT_ERR_INFLATE = -3,   /* a compressed element failed to decode */
    RES_MAT_ERR_NOMEM = -4,
    RES_MAT_ERR_DIGEST = -5,    /* file sha256 differs from the pinned value */
    RES_MAT_ERR_NOTFOUND = -6,
    RES_MAT_UNSUPPORTED = -7,
    RES_MAT_ERR_SHAPE = -8
} ResMatStatus;

typedef struct ResMatArray {
    char name[RES_MAT_MAX_NAME];
    int cls;                 /* RES_MX_* */
    int is_complex, is_logical, has_data;
    uint32_t ndims;
    uint32_t dims[RES_MAT_MAX_DIMS];
    size_t numel;
    double *re;              /* numeric: numel values, column-major (else NULL) */
    struct ResMatArray *cell; /* cell: numel arrays, column-major (else NULL) */
} ResMatArray;

typedef struct {
    char header_text[117];
    uint8_t sha256[32];      /* of the whole file as read */
    size_t file_bytes;
    uint32_t var_count;
    ResMatArray var[RES_MAT_MAX_VARS];
} ResMatFile;

/* Reads path. If want_sha256 is not NULL, the file's SHA-256 must equal it
 * (RES_MAT_ERR_DIGEST otherwise, nothing parsed). */
ResMatStatus res_mat_load(const char *path, const uint8_t *want_sha256, ResMatFile *out);
/* Same, from bytes already in memory. */
ResMatStatus res_mat_parse(const uint8_t *bytes, size_t n, ResMatFile *out);
void res_mat_free(ResMatFile *f);
/* Nonzero (the default): a failed compressed element is described on stderr. */
extern int res_mat_verbose;

const ResMatArray *res_mat_find(const ResMatFile *f, const char *name);
/* Element of a cell array by 0-based column-major subscripts (count must
 * equal the cell's ndims). NULL if out of range or not a cell. */
const ResMatArray *res_mat_cell_at(const ResMatArray *c, const uint32_t *sub, uint32_t count);
/* A numeric array's values, refusing anything else (and shape mismatch when
 * want_numel != 0). */
ResMatStatus res_mat_values(const ResMatArray *a, size_t want_numel, const double **values);

const char *res_mat_strerror(ResMatStatus s);
const char *res_mat_class_name(int cls);

#endif /* RES_MAT_H */
