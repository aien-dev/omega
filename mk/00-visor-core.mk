# Omega Visor V1 (lead-owned). The `omega` binary is PHYSICS-FREE: it links only the
# core object set below + src/visor + src/language + tools/omega.c. It must build
# from a clean checkout with PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0.
VISOR_CORE = sha256 omega_canonical omega_validate omega_core omega_codec aarch64_encoder aarch64_decoder \
	omega_realize omega_realize_synth omega_machine omega_exec omega_verify omega_program omega_synthesis omega_library \
	omega_vector omega_blackwell_qmd omega_blackwell_encoder omega_blackwell_realize
VISOR_CORE_OBJS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,$(VISOR_CORE)))
VISOR_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -MMD -MP -D_GNU_SOURCE -O2 -Isrc -Isrc/visor -Isrc/language
VISOR_SRCS = $(wildcard src/visor/*.c)
LANG_SRCS = $(wildcard src/language/*.c)
VISOR_OBJS = $(patsubst src/visor/%.c,$(OUT_DIR)/visor/%.o,$(VISOR_SRCS))
LANG_OBJS = $(patsubst src/language/%.c,$(OUT_DIR)/language/%.o,$(LANG_SRCS))
VISOR_DEPS = $(VISOR_OBJS:.o=.d) $(LANG_OBJS:.o=.d) $(OUT_DIR)/omega_main.d
-include $(VISOR_DEPS)
OMEGA_BIN = $(OUT_DIR)/omega

$(OUT_DIR)/visor $(OUT_DIR)/language $(OUT_DIR)/tests-visor $(OUT_DIR)/tests-language:
	mkdir -p $@

$(OUT_DIR)/visor/%.o: src/visor/%.c | $(OUT_DIR)/visor
	$(CC) $(VISOR_CFLAGS) -c $< -o $@

$(OUT_DIR)/language/%.o: src/language/%.c | $(OUT_DIR)/language
	$(CC) $(VISOR_CFLAGS) -c $< -o $@

$(OUT_DIR)/omega_main.o: tools/omega.c | $(OUT_DIR)
	$(CC) $(VISOR_CFLAGS) -c $< -o $@

$(OMEGA_BIN): $(VISOR_CORE_OBJS) $(VISOR_OBJS) $(LANG_OBJS) $(OUT_DIR)/omega_main.o
	$(CC) $(VISOR_CFLAGS) -o $@ $^

.PHONY: omega
omega: $(OMEGA_BIN)
	cp $(OMEGA_BIN) ./omega

# Generic rule for lane tests: tests/visor/test_X.c -> build/tests-visor/test_X
$(OUT_DIR)/tests-visor/%: tests/visor/%.c $(VISOR_CORE_OBJS) $(VISOR_OBJS) $(LANG_OBJS) | $(OUT_DIR)/tests-visor
	$(CC) $(VISOR_CFLAGS) -Itests/visor -o $@ $< $(VISOR_CORE_OBJS) $(VISOR_OBJS) $(LANG_OBJS)

$(OUT_DIR)/tests-language/%: tests/language/%.c $(VISOR_CORE_OBJS) $(VISOR_OBJS) $(LANG_OBJS) | $(OUT_DIR)/tests-language
	$(CC) $(VISOR_CFLAGS) -Itests/language -o $@ $< $(VISOR_CORE_OBJS) $(VISOR_OBJS) $(LANG_OBJS)

# ---- aggregate Visor gates (lead-owned) --------------------------------------
# Physics-free lane tests (no PHYSICS_DIR needed):
.PHONY: test-visor-core visor-physics-free-check test-visor
test-visor-core: test-visor-semantic test-language test-visor-verify test-visor-evidence \
	test-visor-machine test-visor-realization test-visor-console visor-authority-check

# Proves the omega binary builds without any physics checkout at all.
visor-physics-free-check:
	rm -rf $(OUT_DIR)/pf-check
	$(MAKE) OUT_DIR=$(OUT_DIR)/pf-check PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 $(OUT_DIR)/pf-check/omega
	@echo OMEGA_VISOR_PHYSICS_FREE_BUILD_PASS

# Full Visor suite: physics-free gates + the two runtime-linked hostile/world tests
# (those need PHYSICS_DIR for omega_evidence.c's nvrm.h and VISOR_AUTH_CAP_LIB).
test-visor: test-visor-core visor-physics-free-check test-visor-world test-visor-authority
