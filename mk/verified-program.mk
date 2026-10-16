# tools/verified_program (omega#505): objective-bound Omegese -> OSC IR -> checker2 -> authorized
# execution with an independent receipt (docs/one_layer/VERIFIED_PROGRAM.md). Host CPU only.
# Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# verified-program        build/verified_program/{verified_program,vp_verify} and the two canary
#                         runners (IR or code bytes changed between the check and the execution;
#                         a forbidden system call inside the sandbox)
# test-verified-program   tools/verified_program/test_verified_program.sh: positives verified by the
#                         independent oracle, then every negative control with its named reason
#
# vp_verify links checker2's core (C2_CORE, from mk/checker2.mk) and src/sha256.c only, never
# src/compiler: it judges the runner, so it shares none of the runner's lowering, back end or runtime.
ifndef VERIFIED_PROGRAM_MK
VERIFIED_PROGRAM_MK := 1
.PHONY: verified-program test-verified-program
VP_DIR = $(OUT_DIR)/verified_program
VP_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/compiler
VP_OSC_LIB = $(filter-out src/compiler/oscc_main.c,$(wildcard src/compiler/*.c)) src/sha256.c
VPV_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -Isrc/checker2

$(VP_DIR)/verified_program: tools/verified_program/vp.c $(VP_OSC_LIB) $(wildcard src/compiler/*.h)
	@mkdir -p $(VP_DIR)
	$(CC) $(VP_CFLAGS) -o $@ tools/verified_program/vp.c $(VP_OSC_LIB)

$(VP_DIR)/verified_program_tamper%: tools/verified_program/vp.c $(VP_OSC_LIB) $(wildcard src/compiler/*.h)
	@mkdir -p $(VP_DIR)
	$(CC) $(VP_CFLAGS) -DVP_TAMPER=$* -o $@ tools/verified_program/vp.c $(VP_OSC_LIB)

$(VP_DIR)/vp_verify: tools/verified_program/vp_verify.c $(C2_CORE) $(wildcard src/checker2/*.h) src/sha256.c src/sha256.h
	@mkdir -p $(VP_DIR)
	$(CC) $(VPV_CFLAGS) -o $@ tools/verified_program/vp_verify.c $(C2_CORE) src/sha256.c

verified-program: $(VP_DIR)/verified_program $(VP_DIR)/vp_verify $(VP_DIR)/verified_program_tamper1 $(VP_DIR)/verified_program_tamper2 $(VP_DIR)/verified_program_tamper3

test-verified-program: verified-program omegese omega-emit checker2
	bash tools/verified_program/test_verified_program.sh $(abspath $(VP_DIR)) $(abspath $(OUT_DIR)/omegese/omegese) \
		$(abspath $(OUT_DIR)/omega_emit/omega_emit) $(abspath $(C2_DIR)/checker2)
endif
