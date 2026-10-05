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
.PHONY: test-dual-types test-dual-bind test-dual-update dual-purity test-dual
DUAL_CFLAGS = $(EST_CFLAGS) -Isrc/dual -Itests/dual
DUAL_DIR = $(OUT_DIR)/tests-dual
DUAL_CORE_SRCS = src/dual/rx_dual.c src/sha256.c
DUAL_SRCS = src/dual/rx_dual.c src/dual/rx_dual_bind.c src/dual/rx_dual_update.c $(EST_TYPES_SRCS)
DUAL_HDRS = src/dual/rx_dual.h src/dual/rx_dual_bind.h src/dual/rx_dual_update.h $(EST_HDRS)
# A price is information, not authority: no capability, promotion, generation
# administration, World, ARGUS, AEGIS, memory-mapping or process symbol.
DUAL_FORBIDDEN = 'aienos_|rx_gen_|rx_cap|rx_world|rx_js|argus_|forge_|aegis|mmap|mprotect|fork|exec|dlopen|system|socket|fopen|open$$'

$(DUAL_DIR)/%.o: src/dual/%.c $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) -c -o $@ $<

dual-purity: $(DUAL_DIR)/rx_dual.o $(DUAL_DIR)/rx_dual_bind.o $(DUAL_DIR)/rx_dual_update.o
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

test-dual: dual-purity test-dual-types test-dual-bind test-dual-update
	@echo "test-dual: DUAL standalone module passes"
endif

# DUAL-0b binding and DUAL-1a controller (appended; same fragment)
ifndef DUAL_MK_2
DUAL_MK_2 := 1
$(DUAL_DIR)/test_dual_bind: tests/dual/test_dual_bind.c tests/dual/dual_fixtures.h $(DUAL_SRCS) $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) -o $@ tests/dual/test_dual_bind.c $(DUAL_SRCS) -lm
$(DUAL_DIR)/test_dual_bind_asan: tests/dual/test_dual_bind.c tests/dual/dual_fixtures.h $(DUAL_SRCS) $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) $(EST_ASAN) -o $@ tests/dual/test_dual_bind.c $(DUAL_SRCS) -lm
test-dual-bind: $(DUAL_DIR)/test_dual_bind $(DUAL_DIR)/test_dual_bind_asan
	./$(DUAL_DIR)/test_dual_bind
	./$(DUAL_DIR)/test_dual_bind_asan
	@echo "test-dual-bind: DUAL-0b binding and staleness checks pass in plain and ASan/UBSan builds"

DUAL_REF_SRCS = tests/dual/dual_ref.c
$(DUAL_DIR)/test_dual_update: tests/dual/test_dual_update.c tests/dual/dual_fixtures.h tests/dual/dual_ref.h $(DUAL_REF_SRCS) $(DUAL_SRCS) $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) -o $@ tests/dual/test_dual_update.c $(DUAL_REF_SRCS) $(DUAL_SRCS) -lm
$(DUAL_DIR)/test_dual_update_asan: tests/dual/test_dual_update.c tests/dual/dual_fixtures.h tests/dual/dual_ref.h $(DUAL_REF_SRCS) $(DUAL_SRCS) $(DUAL_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) $(EST_ASAN) -DPROP_ROUNDS=300 -o $@ tests/dual/test_dual_update.c $(DUAL_REF_SRCS) $(DUAL_SRCS) -lm
test-dual-update: $(DUAL_DIR)/test_dual_update $(DUAL_DIR)/test_dual_update_asan
	./$(DUAL_DIR)/test_dual_update
	./$(DUAL_DIR)/test_dual_update_asan
	@echo "test-dual-update: DUAL-1a reference update passes analytic, property and refusal checks in plain and ASan/UBSan builds"
endif

# DUAL-1b replay (appended; same fragment)
ifndef DUAL_MK_3
DUAL_MK_3 := 1
.PHONY: test-dual-replay
DUAL_REPLAY_SRCS = $(DUAL_SRCS) src/dual/rx_dual_replay.c tests/dual/dual_traces.c
DUAL_REPLAY_HDRS = $(DUAL_HDRS) src/dual/rx_dual_replay.h tests/dual/dual_traces.h tests/dual/dual_fixtures.h
$(DUAL_DIR)/test_dual_replay: tests/dual/test_dual_replay.c $(DUAL_REPLAY_SRCS) $(DUAL_REPLAY_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) -o $@ tests/dual/test_dual_replay.c $(DUAL_REPLAY_SRCS) -lm
$(DUAL_DIR)/test_dual_replay_asan: tests/dual/test_dual_replay.c $(DUAL_REPLAY_SRCS) $(DUAL_REPLAY_HDRS)
	@mkdir -p $(DUAL_DIR)
	$(CC) $(DUAL_CFLAGS) $(EST_ASAN) -o $@ tests/dual/test_dual_replay.c $(DUAL_REPLAY_SRCS) -lm
# Run from the repository root: the recorded traces are evidence/R15/raw/*/{machine,preflight}-perf.csv.
test-dual-replay: $(DUAL_DIR)/test_dual_replay $(DUAL_DIR)/test_dual_replay_asan
	./$(DUAL_DIR)/test_dual_replay
	./$(DUAL_DIR)/test_dual_replay_asan > /dev/null
	@echo "test-dual-replay: DUAL-1b replay, pre-registered measures and negative control pass in plain and ASan/UBSan builds (all inputs UNCALIBRATED)"
dual-purity: $(DUAL_DIR)/rx_dual_replay.o
test-dual: test-dual-replay
endif
