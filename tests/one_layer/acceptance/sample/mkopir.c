/*
 * mkopir.c -- writes the OmegaProgram IR bytes of the PUBLIC SAMPLE corpus cases (not the sealed
 * corpus). Built from the repo's own program API so the bytes are what omega_program_ir_decode reads:
 *   mkopir <out.opir> <width> <op> <imm> [<op> <imm>]...      op: add sub mul and or
 * Prints "program_id=<hex>". Uses omega_program_build_unary_op for the first step, appends the
 * rest, sets the declared width, recomputes the id (omega_program_compute_id), then
 * omega_program_ir_encode.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omega_program.h"
#include "omega_program_ir.h"

static int opc(const char *s)
{
    return !strcmp(s, "add") ? OP_ADD : !strcmp(s, "sub") ? OP_SUB : !strcmp(s, "mul") ? OP_MUL
         : !strcmp(s, "and") ? OP_AND : !strcmp(s, "or") ? OP_OR : -1;
}

int main(int argc, char **argv)
{
    if (argc < 5 || (argc - 3) % 2) { fprintf(stderr, "usage: mkopir out width op imm [op imm]...\n"); return 2; }
    static OmegaProgram p;
    int op = opc(argv[3]);
    if (op < 0 || omega_program_build_unary_op(&p, "sample", (OpCode)op, strtoull(argv[4], NULL, 0))) return 1;
    for (int i = 5; i + 1 < argc; i += 2) {
        if ((op = opc(argv[i])) < 0 || p.body.step_count >= OMEGA_PROGRAM_MAX_STEPS) return 1;
        p.body.steps[p.body.step_count].op = (uint8_t)op;
        p.body.steps[p.body.step_count++].imm = strtoull(argv[i + 1], NULL, 0);
    }
    uint16_t w = (uint16_t)atoi(argv[2]);
    p.contract.input_width = p.contract.output_width = w;
    if (omega_program_compute_id(&p)) return 1;
    uint8_t *b = NULL;
    size_t n = 0;
    if (omega_program_ir_encode(&p, &b, &n)) return 1;
    FILE *f = fopen(argv[1], "wb");
    if (!f || fwrite(b, 1, n, f) != n || fclose(f)) return 1;
    printf("program_id=");
    for (int i = 0; i < 32; i++) printf("%02x", p.program_id.bytes[i]);
    printf("\n");
    free(b);
    return 0;
}
