/* at1-model: command-line tool of the AT-1 candidate engine.
 *
 *   at1-model result <case> <oracle-record|none> <provenance> [--reversed]
 *       writes the complete AT1_RESULT_V1 file to standard output. The reference_* lines are
 *       spliced verbatim from the oracle record (charter reading c); "none", or an oracle record
 *       that cannot be read, writes an ERROR
 *       result with error_code ORACLE_UNAVAILABLE. The provenance file holds the provenance
 *       lines (source_repo .. run_finished_utc, then optional artifact lines) so that the
 *       engine itself never reads a clock.
 *   at1-model values <case> [--reversed]
 *       writes the engine's own values lines only (component output, not a contract file).
 *
 * Exit status: 0 written; 2 case refused (one line "AT1_CASE_REFUSED <code>" on stderr);
 * 1 engine error (one line "AT1_ENGINE_ERROR <reason>" on stderr), nothing on stdout. */
#include "at1_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int usage(void)
{
    fputs("usage: at1-model result <case> <oracle-record|none> <provenance> [--reversed]\n"
          "       at1-model values <case> [--reversed]\n", stderr);
    return 1;
}

static int engine_error(const char *what)
{
    fprintf(stderr, "AT1_ENGINE_ERROR %s\n", what);
    return 1;
}

static int emit(char *out, size_t len)
{
    size_t w = fwrite(out, 1, len, stdout);
    free(out);
    if (w != len || fflush(stdout) != 0) return engine_error("write failed");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) return usage();
    int result_mode = strcmp(argv[1], "result") == 0;
    if (!result_mode && strcmp(argv[1], "values") != 0) return usage();
    int fixed = result_mode ? 5 : 3, reversed = 0;
    if (argc == fixed + 1 && strcmp(argv[fixed], "--reversed") == 0) reversed = 1;
    else if (argc != fixed) return usage();

    uint8_t *case_bytes = NULL, *oracle = NULL, *prov = NULL;
    size_t case_len = 0, oracle_len = 0, prov_len = 0;
    at1_status st = at1_read_file(argv[2], &case_bytes, &case_len);
    if (st == AT1_ERR_RESOURCE) return engine_error("RESOURCE_LIMIT case file larger than 1 MiB");
    if (st != AT1_OK) return engine_error("cannot read case file");
    /* the case is validated before any other input is read, so a refusal is never masked */
    at1_case c;
    st = at1_case_parse(case_bytes, case_len, &c);
    free(case_bytes);
    if (st >= AT1_CASE_PARSE_ERROR && st <= AT1_CASE_ID_MISMATCH) {
        fprintf(stderr, "AT1_CASE_REFUSED %s\n", at1_status_name(st));
        return 2;
    }
    if (st != AT1_OK) return engine_error(st == AT1_ERR_RESOURCE ? "RESOURCE_LIMIT engine limit in case file" : "INTERNAL_ERROR case parse");

    /* an oracle record that cannot be read is a run error (ORACLE_UNAVAILABLE), not a tool failure;
     * the provenance is required to write any result at all */
    if (result_mode) {
        st = at1_read_file(argv[4], &prov, &prov_len);
        if (st != AT1_OK) { at1_case_free(&c); return engine_error("cannot read provenance file"); }
        if (strcmp(argv[3], "none") != 0 && at1_read_file(argv[3], &oracle, &oracle_len) != AT1_OK) {
            free(oracle); oracle = NULL; oracle_len = 0;     /* written as ERROR ORACLE_UNAVAILABLE */
        }
    }

    at1_kernel k;
    at1_values *v = at1_xcalloc(1, sizeof *v);
    const char *run_error = NULL;
    st = at1_kernel_build(&c, &k);
    if (st == AT1_OK) {
        st = at1_compute(&c, &k, reversed, v);
        at1_kernel_free(&k);
    }
    if (st == AT1_ERR_RESOURCE) run_error = "RESOURCE_LIMIT";
    else if (st != AT1_OK) run_error = "INTERNAL_ERROR";

    char *out = NULL; size_t out_len = 0; char err[256];
    int rc;
    if (!result_mode) {
        if (run_error) rc = engine_error(run_error);
        else { at1_values_write(&c, v, &out, &out_len); rc = emit(out, out_len); }
    } else {
        if (run_error)
            st = at1_result_write_error(&c, run_error, (const char *)prov, prov_len, &out, &out_len, err, sizeof err);
        else
            st = at1_result_write(&c, v, (const char *)oracle, oracle_len, (const char *)prov, prov_len, &out, &out_len, err, sizeof err);
        if (st == AT1_OK) rc = emit(out, out_len);
        else if (st == AT1_ERR_ARGUMENT) rc = engine_error(err);
        else rc = engine_error("INTERNAL_ERROR result writer");
    }
    free(v); free(oracle); free(prov);
    at1_case_free(&c);
    return rc;
}
