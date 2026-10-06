# AIEN Prime Drag Race (bench/prime_race/): implementation-side protocol + conventional CPU baseline.
# Standalone: new files only under bench/prime_race/. Picked up by `-include mk/*.mk`; not part of `all`
# or `test`. No physics, no GPU, no libraries.
#
# prime-race-cpu        builds bench/prime_race/build/cpu_base (-O3 -mcpu=native; flags baked in via -D)
# prime-race-host-test  trial-division oracle test of cpu_base + protocol formatting/parsing
ifndef PRIME_RACE_MK
PRIME_RACE_MK := 1
.PHONY: prime-race-cpu prime-race-host-test
PR_DIR = bench/prime_race
PR_BUILD = $(PR_DIR)/build
PR_WARN = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE
PR_OPT = -O3 -mcpu=native
PR_SOURCE_COMMIT_VAL := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
PR_DEFS = -DPR_SOURCE_COMMIT='"$(PR_SOURCE_COMMIT_VAL)"' -DPR_CC='"$(CC)"' -DPR_CFLAGS='"$(PR_WARN) $(PR_OPT)"'
PR_HDRS = $(PR_DIR)/prime_race_impl.h

$(PR_BUILD)/cpu_base: $(PR_DIR)/cpu_base.c $(PR_DIR)/prime_race_impl.c $(PR_HDRS)
	@mkdir -p $(PR_BUILD)
	$(CC) $(PR_WARN) $(PR_OPT) $(PR_DEFS) -o $@ $(PR_DIR)/cpu_base.c $(PR_DIR)/prime_race_impl.c -lm
prime-race-cpu: $(PR_BUILD)/cpu_base

$(PR_BUILD)/prime_race_host_test: $(PR_DIR)/tests/prime_race_host_test.c $(PR_DIR)/cpu_base.c $(PR_DIR)/prime_race_impl.c $(PR_HDRS)
	@mkdir -p $(PR_BUILD)
	$(CC) $(PR_WARN) -O2 -DPR_NO_MAIN -o $@ $(PR_DIR)/tests/prime_race_host_test.c $(PR_DIR)/cpu_base.c $(PR_DIR)/prime_race_impl.c -lm
prime-race-host-test: $(PR_BUILD)/prime_race_host_test
	./$(PR_BUILD)/prime_race_host_test
endif
