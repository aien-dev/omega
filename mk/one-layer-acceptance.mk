# One-layer stage 1: hostile acceptance harness (omega#443, tests/one_layer/acceptance/).
# Contract: tests/one_layer/acceptance/HARNESS_INTERFACE.md (v1, sha256 pinned in RULES).
# Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# one-layer-acceptance-tools  build/one_layer/{ola,ola_runner}
# test-one-layer-acceptance   selftest on the PUBLIC sample corpus (tests/one_layer/acceptance/sample/):
#                             expected verdict per sample case, every isolation probe blocked + its
#                             negative control escaping, two clean replays identical, tally gate
#                             logic, and mutants of the harness that each must be caught.
# Scoring the sealed corpus is NOT a make target: tests/one_layer/acceptance/ola.sh score <tar> <SEALED.sha256>
ifndef ONE_LAYER_ACCEPTANCE_MK
ONE_LAYER_ACCEPTANCE_MK := 1
.PHONY: one-layer-acceptance-tools test-one-layer-acceptance
OLA_SRC_DIR = tests/one_layer/acceptance
OLA_DIR = $(OUT_DIR)/one_layer
OLA_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/compiler
# the runner links the OSC compiler (oscc's library, mk/compiler.mk OSC_LIB) and the program IR
# stack (Makefile RESOLVE_PROGRAM_STACK: omega_program_ir_decode + realize + exec)
OLA_OSC_LIB = $(filter-out src/compiler/oscc_main.c,$(wildcard src/compiler/*.c))
OLA_PROG_STACK = src/omega_program_ir.c src/omega_program.c src/omega_canonical.c src/omega_validate.c \
	src/omega_core.c src/omega_codec.c src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c \
	src/omega_exec.c src/omega_verify.c src/omega_realize_synth.c src/omega_machine.c
OLA_DEPS = $(wildcard src/compiler/*.h) $(wildcard src/*.h)

$(OLA_DIR)/ola: $(OLA_SRC_DIR)/ola.c src/sha256.c src/sha256.h
	@mkdir -p $(OLA_DIR)
	$(CC) $(OLA_CFLAGS) -o $@ $(OLA_SRC_DIR)/ola.c src/sha256.c

$(OLA_DIR)/ola_runner: $(OLA_SRC_DIR)/ola_runner.c $(OLA_OSC_LIB) $(OLA_PROG_STACK) src/sha256.c $(OLA_DEPS)
	@mkdir -p $(OLA_DIR)
	$(CC) $(OLA_CFLAGS) -o $@ $(OLA_SRC_DIR)/ola_runner.c $(OLA_OSC_LIB) $(OLA_PROG_STACK) src/sha256.c

one-layer-acceptance-tools: $(OLA_DIR)/ola $(OLA_DIR)/ola_runner

test-one-layer-acceptance: one-layer-acceptance-tools
	sh $(OLA_SRC_DIR)/selftest.sh $(abspath $(OLA_DIR))
endif
