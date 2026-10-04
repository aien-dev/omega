# DUAL-3a: read-only observation tap at decision site "jspace.residency"
# (ADR 0031 section 7.3; src/runtime/rx_jspace.c js_forge_enforce).
# Not part of `all` or `test`. Host only; no GPU.
#
# test-dual-jspace-tap          parity suite, plain + ASan/UBSan builds
# test-dual-jspace-tap-hostile  negative control: a hostile observer mutates the
#                               space through its ctx; the parity suite must FAIL
# dual-jspace-tap-purity        nm -u on rx_jspace.o: no aienos_cap_*, rx_gen_*, aegis
# dual-jspace-tap-hygiene       the hostile seam exists in tests only (grep src/)
# bench-dual-jspace-tap         observer NULL vs recording observer vs the base
#                               commit's rx_jspace.c (DJT_BASE_SHA), same workload
# test-dual-3a                  purity + hygiene + parity + hostile
ifndef DUAL_JSPACE_TAP_MK
DUAL_JSPACE_TAP_MK := 1
.PHONY: test-dual-jspace-tap test-dual-jspace-tap-hostile dual-jspace-tap-purity \
	dual-jspace-tap-hygiene bench-dual-jspace-tap test-dual-3a
DJT_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -pthread
DJT_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
DJT_DIR = $(OUT_DIR)/tests-dual-jspace-tap
DJT_BASE_SHA ?= cb7cc216147664549484fcca8e8ceadbd6f6cdbf
DJT_JSPACE = src/runtime/rx_jspace.c src/runtime/rx_jspace.h src/runtime/aien_machine_id.h src/sha256.h
DJT_TEST = tests/runtime/rx_dual_jspace_tap.c
DJT_BENCH = tests/runtime/rx_dual_jspace_tap_bench.c

# The parity test includes rx_jspace.c (reference oracle needs its statics).
$(DJT_DIR)/rx_dual_jspace_tap: $(DJT_TEST) $(DJT_JSPACE) src/sha256.c
	@mkdir -p $(DJT_DIR)
	$(CC) $(DJT_CFLAGS) -o $@ $(DJT_TEST) src/sha256.c -lm
$(DJT_DIR)/rx_dual_jspace_tap_asan: $(DJT_TEST) $(DJT_JSPACE) src/sha256.c
	@mkdir -p $(DJT_DIR)
	$(CC) $(DJT_CFLAGS) $(DJT_ASAN) -o $@ $(DJT_TEST) src/sha256.c -lm
$(DJT_DIR)/rx_dual_jspace_tap_hostile: $(DJT_TEST) $(DJT_JSPACE) src/sha256.c
	@mkdir -p $(DJT_DIR)
	$(CC) $(DJT_CFLAGS) -DDUAL_TAP_HOSTILE_TEST -o $@ $(DJT_TEST) src/sha256.c -lm

test-dual-jspace-tap: $(DJT_DIR)/rx_dual_jspace_tap $(DJT_DIR)/rx_dual_jspace_tap_asan
	./$(DJT_DIR)/rx_dual_jspace_tap
	./$(DJT_DIR)/rx_dual_jspace_tap_asan
	@echo "test-dual-jspace-tap: parity with and without the observer, plain and ASan/UBSan"

test-dual-jspace-tap-hostile: $(DJT_DIR)/rx_dual_jspace_tap_hostile
	@if ./$(DJT_DIR)/rx_dual_jspace_tap_hostile; then \
		echo "test-dual-jspace-tap-hostile: a hostile observer changed decisions and went UNDETECTED"; exit 1; fi
	@echo "test-dual-jspace-tap-hostile: hostile observer detected (parity suite failed as required)"

$(DJT_DIR)/rx_jspace.o: $(DJT_JSPACE)
	@mkdir -p $(DJT_DIR)
	$(CC) $(DJT_CFLAGS) -c -o $@ src/runtime/rx_jspace.c
dual-jspace-tap-purity: $(DJT_DIR)/rx_jspace.o
	@if nm -u $< | grep -E 'aienos_cap_|rx_gen_|aegis'; then \
		echo "rx_jspace.o references an authority or promotion symbol"; exit 1; fi
	@echo "dual-jspace-tap-purity: rx_jspace.o references no aienos_cap_*, rx_gen_* or aegis symbol"

dual-jspace-tap-hygiene:
	@if grep -rn 'DUAL_TAP_HOSTILE' src/ ; then \
		echo "the hostile test seam leaked into src/"; exit 1; fi
	@echo "dual-jspace-tap-hygiene: DUAL_TAP_HOSTILE appears nowhere under src/"

# Benchmark: the base commit's rx_jspace.c/.h are extracted next to each other
# so its own #include "rx_jspace.h" resolves to the old header.
DJT_BASE_DIR = $(DJT_DIR)/base-$(DJT_BASE_SHA)
$(DJT_BASE_DIR)/rx_jspace.c:
	@mkdir -p $(DJT_BASE_DIR)
	git show $(DJT_BASE_SHA):src/runtime/rx_jspace.c > $@
	git show $(DJT_BASE_SHA):src/runtime/rx_jspace.h > $(DJT_BASE_DIR)/rx_jspace.h
	git show $(DJT_BASE_SHA):src/runtime/aien_machine_id.h > $(DJT_BASE_DIR)/aien_machine_id.h
$(DJT_DIR)/rx_dual_jspace_tap_bench_base: $(DJT_BENCH) $(DJT_BASE_DIR)/rx_jspace.c src/sha256.c
	@mkdir -p $(DJT_DIR)
	$(CC) $(DJT_CFLAGS) -DDUAL_TAP_BENCH_BASE -I$(DJT_BASE_DIR) -o $@ $(DJT_BENCH) $(DJT_BASE_DIR)/rx_jspace.c src/sha256.c -lm
$(DJT_DIR)/rx_dual_jspace_tap_bench: $(DJT_BENCH) $(DJT_JSPACE) src/sha256.c
	@mkdir -p $(DJT_DIR)
	$(CC) $(DJT_CFLAGS) -o $@ $(DJT_BENCH) src/runtime/rx_jspace.c src/sha256.c -lm

bench-dual-jspace-tap: $(DJT_DIR)/rx_dual_jspace_tap_bench_base $(DJT_DIR)/rx_dual_jspace_tap_bench
	./$(DJT_DIR)/rx_dual_jspace_tap_bench_base
	./$(DJT_DIR)/rx_dual_jspace_tap_bench

test-dual-3a: dual-jspace-tap-purity dual-jspace-tap-hygiene test-dual-jspace-tap test-dual-jspace-tap-hostile
	@echo "test-dual-3a: DUAL-3a tap implemented; DUAL_ADVISORY_SCHEDULER not claimed"
endif
