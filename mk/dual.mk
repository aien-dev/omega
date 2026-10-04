# DUAL, constraint pricing for soft resource budgets (ADR 0031 / ARCH-0031).
# Standalone: new files only under src/dual/, tests/dual/, docs/dual/. Picked up
# by `-include mk/*.mk`; not part of `all` or `test`. Nothing in src/runtime/
# includes or links it. DUAL is scarcity information: it never authorizes,
# never promotes, never decides validity.
#
# test-dual-types    DUAL-0a contract: validation, refusal, canonical encoding, digests, properties
# dual-purity        nm -u on every dual object against authority / promotion / process symbols
# test-dual          everything above, plain + ASan/UBSan
ifndef DUAL_MK
DUAL_MK := 1
include mk/estimation.mk
.PHONY: test-dual-types dual-purity test-dual
DUAL_CFLAGS = $(EST_CFLAGS) -Isrc/dual -Itests/dual
DUAL_DIR = $(OUT_DIR)/tests-dual
DUAL_CORE_SRCS = src/dual/rx_dual.c src/sha256.c
DUAL_HDRS = src/dual/rx_dual.h
# A price is information, not authority: no capability, promotion, generation
# administration, World, ARGUS, AEGIS, memory-mapping or process symbol.
DUAL_FORBIDDEN = 'aienos_|rx_gen_|rx_cap|rx_world|rx_js|argus_|forge_|aegis|mmap|mprotect|fork|exec|dlopen|system|socket|fopen|open$$'

$(DUAL_DIR)/%.o: src/dual/%.c $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) -c -o $@ $<

dual-purity: $(DUAL_DIR)/rx_dual.o
	@if nm -u $^ | grep -E $(DUAL_FORBIDDEN) ; then \
		echo "dual object references an operation a pricing module must not have"; exit 1; fi
	@echo "dual-purity: $(notdir $^) reference no authority, promotion or process symbol"

$(DUAL_DIR)/test_dual_types: tests/dual/test_dual_types.c $(DUAL_CORE_SRCS) $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) -o $@ tests/dual/test_dual_types.c $(DUAL_CORE_SRCS) -lm
$(DUAL_DIR)/test_dual_types_asan: tests/dual/test_dual_types.c $(DUAL_CORE_SRCS) $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) $(EST_ASAN) -DPROP_ROUNDS=200 -o $@ tests/dual/test_dual_types.c $(DUAL_CORE_SRCS) -lm

test-dual-types: $(DUAL_DIR)/test_dual_types $(DUAL_DIR)/test_dual_types_asan
	./$(DUAL_DIR)/test_dual_types
	./$(DUAL_DIR)/test_dual_types_asan
	@echo "test-dual-types: DUAL-0a contract checks pass in plain and ASan/UBSan builds"

test-dual: dual-purity test-dual-types
	@echo "test-dual: DUAL standalone module passes"
endif
