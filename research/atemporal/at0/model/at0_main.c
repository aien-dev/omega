/* at0-model: run one AT0_CASE_V1 case through the candidate engine.
 *   at0-model <case-file> [--reversed]
 * stdout: OMEGA-AT0-ENGINE v1 text. Refused case: stderr "AT0_CASE_REFUSED <code>",
 * exit 2 (AT0_RESULT_V1 section 5). Other failures: stderr "AT0_ENGINE_ERROR <code>", exit 1. */
#include "at0_model.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "--reversed") != 0)) {
        fprintf(stderr, "usage: at0-model <case-file> [--reversed]\n");
        return 1;
    }
    at0_case *c = calloc(1, sizeof *c);
    at0_engine_result *r = calloc(1, sizeof *r);
    char *buf = malloc(1 << 20);
    if (!c || !r || !buf) { fprintf(stderr, "AT0_ENGINE_ERROR ERR_INTERNAL\n"); return 1; }
    at0_status st = at0_case_read_file(argv[1], c);
    if (at0_status_is_refusal(st)) { fprintf(stderr, "AT0_CASE_REFUSED %s\n", at0_status_name(st)); free(c); free(r); free(buf); return 2; }
    if (st != AT0_OK) { fprintf(stderr, "AT0_ENGINE_ERROR %s\n", at0_status_name(st)); free(c); free(r); free(buf); return 1; }
    st = argc == 3 ? at0_engine_run_reversed(c, r) : at0_engine_run(c, r);
    if (st != AT0_OK) { fprintf(stderr, "AT0_ENGINE_ERROR %s\n", at0_status_name(st)); free(c); free(r); free(buf); return 1; }
    long n = at0_engine_emit(c, r, buf, 1 << 20);
    if (n < 0) { fprintf(stderr, "AT0_ENGINE_ERROR ERR_INTERNAL\n"); free(c); free(r); free(buf); return 1; }
    int rc = fwrite(buf, 1, (size_t)n, stdout) == (size_t)n && fflush(stdout) == 0 ? 0 : 1;
    if (rc) fprintf(stderr, "AT0_ENGINE_ERROR ERR_IO\n");
    free(c); free(r); free(buf);
    return rc;
}
