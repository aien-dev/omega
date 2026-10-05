# AIEN Prime Drag Race: native GB10 implementation (bench/prime_race/gb10_native.c) on Omega's
# own Blackwell codegen + session (libomega_gpu.a, no CUDA). Picked up by `-include mk/*.mk`;
# not part of `all` or `test`. Needs PHYSICS_DIR at the physics.lock commit (libomega_gpu).
#
# prime-race-gb10            build bench/prime_race/build/gb10_native (+ _mutant, the chip test's
#                            negative control) and refuse them if nm finds a CUDA symbol
# prime-race-gb10-host-test  golden words, nvdisasm listing, host IR simulator sweep (no device)
# The chip test is bench/prime_race/tests/run_gb10_chip.sh (holds /tmp/aien-gb10.lock).
ifndef PRIME_RACE_GB10_MK
PRIME_RACE_GB10_MK := 1
.PHONY: prime-race-gb10 prime-race-gb10-host-test
GB_DIR = bench/prime_race
GB_BUILD = $(GB_DIR)/build
GB_OPT = -O3 -mcpu=native
GB_SRC_COMMIT := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
GB_DEFS = -DPR_SOURCE_COMMIT='"$(GB_SRC_COMMIT)"' -DPR_CC='"$(CC)"' \
	-DPR_CFLAGS='"$(GB_OPT) -I$(GB_DIR) + omega CFLAGS; libomega_gpu.a built with omega CFLAGS"'
GB_DEPS = $(GB_DIR)/gb10_native.c $(GB_DIR)/prime_race_impl.c $(GB_DIR)/prime_race_impl.h \
	src/omega_gpu_elementwise_api.h src/omega_gpu_session.h $(OUT_DIR)/libomega_gpu.a
# Same pattern as tools/chip_run.sh REFUSAL:cuda
GB_NM_CHECK = if nm -u $@ | grep -Eiq 'cuda|cuInit|cuLaunch|nvrtc|cublas'; then echo "CUDA symbols in $@"; rm -f $@; exit 1; fi

$(GB_BUILD)/gb10_native: $(GB_DEPS)
	@mkdir -p $(GB_BUILD)
	$(CC) $(CFLAGS) $(GB_OPT) -I$(GB_DIR) $(GB_DEFS) -o $@ $(GB_DIR)/gb10_native.c $(GB_DIR)/prime_race_impl.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
	@$(GB_NM_CHECK)
$(GB_BUILD)/gb10_native_mutant: $(GB_DEPS)
	@mkdir -p $(GB_BUILD)
	$(CC) $(CFLAGS) $(GB_OPT) -I$(GB_DIR) $(GB_DEFS) -DPR_GB10_MUTANT=1 -o $@ $(GB_DIR)/gb10_native.c $(GB_DIR)/prime_race_impl.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
	@$(GB_NM_CHECK)
prime-race-gb10: $(GB_BUILD)/gb10_native $(GB_BUILD)/gb10_native_mutant

$(GB_BUILD)/gb10_sieve_host_test: $(GB_DIR)/tests/gb10_sieve_host_test.c $(GB_DIR)/prime_race_impl.h src/omega_blackwell_codegen.h src/omega_gpu_elementwise_api.h $(OUT_DIR)/libomega_gpu.a
	@mkdir -p $(GB_BUILD)
	$(CC) $(CFLAGS) -I$(GB_DIR) -o $@ $(GB_DIR)/tests/gb10_sieve_host_test.c $(OUT_DIR)/libomega_gpu.a -lpthread -lm
prime-race-gb10-host-test: $(GB_BUILD)/gb10_sieve_host_test
	./$(GB_BUILD)/gb10_sieve_host_test
endif
