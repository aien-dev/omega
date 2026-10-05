# Proof layer, hardening lane LG: World lifecycle model, differential harness
# against rx_world, and a ThreadSanitizer run of rx_world. Host only (no
# graphics chip, no physics headers). Owned files: tests/model/, this file.
#
#   make test-model       exhaustive model check + mutants
#   make test-model-diff  model vs real rx_world (+ concurrent stress)
#   make test-model-tsan  the same harness built with -fsanitize=thread
#                         (NOT_RUN when the toolchain has no tsan)
#   make test-proof       all three, then build/proof/LG-receipt.json
#
# Every target refuses to start while ~/workspace/.spark-quiet exists and
# says NOT_RUN instead. Knobs: PROOF_DEPTH (default 6), PROOF_TSAN_DEPTH (3).

PROOF_OUT        ?= $(OUT_DIR)/proof
PROOF_DEPTH      ?= 6
PROOF_TSAN_DEPTH ?= 3
PROOF_QUIET      ?= $(HOME)/workspace/.spark-quiet
PROOF_CFLAGS     ?= -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc -Itests/model -pthread
PROOF_MODEL_SRCS = tests/model/world_model.c tests/model/world_explore.c
PROOF_MODEL_HDRS = tests/model/world_model.h tests/model/world_explore.h
PROOF_RX_SRCS    = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c src/sha256.c
PROOF_RX_HDRS    = src/runtime/rx_world.h src/runtime/rx_caproot.h src/runtime/rx_caller.h \
                   src/runtime/omega_shared_world_abi.h

PROOF_MODEL_BIN  = $(PROOF_OUT)/model_check
PROOF_DIFF_BIN   = $(PROOF_OUT)/world_diff
PROOF_TSAN_BIN   = $(PROOF_OUT)/world_diff_tsan

define proof_quiet_guard
	@if [ -e "$(PROOF_QUIET)" ]; then echo "$(1): NOT_RUN (quiet flag $(PROOF_QUIET) is set)"; exit 1; fi
endef

$(PROOF_OUT):
	mkdir -p $@

$(PROOF_MODEL_BIN): tests/model/model_check.c $(PROOF_MODEL_SRCS) $(PROOF_MODEL_HDRS) | $(PROOF_OUT)
	$(CC) $(PROOF_CFLAGS) -O2 -o $@ tests/model/model_check.c $(PROOF_MODEL_SRCS)

$(PROOF_DIFF_BIN): tests/model/world_diff.c $(PROOF_MODEL_SRCS) $(PROOF_MODEL_HDRS) \
		$(PROOF_RX_SRCS) $(PROOF_RX_HDRS) | $(PROOF_OUT)
	$(CC) $(PROOF_CFLAGS) -O2 -o $@ tests/model/world_diff.c $(PROOF_MODEL_SRCS) $(PROOF_RX_SRCS)

$(PROOF_TSAN_BIN): tests/model/world_diff.c $(PROOF_MODEL_SRCS) $(PROOF_MODEL_HDRS) \
		$(PROOF_RX_SRCS) $(PROOF_RX_HDRS) | $(PROOF_OUT)
	$(CC) $(PROOF_CFLAGS) -O1 -g -fsanitize=thread -o $@ tests/model/world_diff.c \
		$(PROOF_MODEL_SRCS) $(PROOF_RX_SRCS)

.PHONY: test-model test-model-diff test-model-tsan test-proof

test-model:
	$(call proof_quiet_guard,test-model)
	@$(MAKE) --no-print-directory $(PROOF_MODEL_BIN)
	./$(PROOF_MODEL_BIN) > $(PROOF_OUT)/model.log 2>&1; rc=$$?; cat $(PROOF_OUT)/model.log; exit $$rc

test-model-diff:
	$(call proof_quiet_guard,test-model-diff)
	@$(MAKE) --no-print-directory $(PROOF_DIFF_BIN)
	./$(PROOF_DIFF_BIN) --depth $(PROOF_DEPTH) --stress > $(PROOF_OUT)/diff.log 2>&1; rc=$$?; \
		cat $(PROOF_OUT)/diff.log; exit $$rc

# The tsan probe builds and runs a one-line program; a toolchain without
# libtsan (or a memory layout tsan rejects) is recorded as NOT_RUN.
test-model-tsan:
	$(call proof_quiet_guard,test-model-tsan)
	@mkdir -p $(PROOF_OUT)
	@printf 'int main(void){return 0;}\n' > $(PROOF_OUT)/tsan_probe.c
	@if ! $(CC) -fsanitize=thread -o $(PROOF_OUT)/tsan_probe $(PROOF_OUT)/tsan_probe.c 2>/dev/null || \
	    ! $(PROOF_OUT)/tsan_probe >/dev/null 2>&1; then \
		echo "RESULT tsan=NOT_RUN (toolchain has no working -fsanitize=thread)" | tee $(PROOF_OUT)/tsan.log; \
		exit 0; \
	fi; \
	$(MAKE) --no-print-directory $(PROOF_TSAN_BIN) || exit 1; \
	TSAN_OPTIONS="halt_on_error=0 exitcode=66 second_deadlock_stack=1" \
		./$(PROOF_TSAN_BIN) --depth $(PROOF_TSAN_DEPTH) --stress --stress-iters 400 \
		> $(PROOF_OUT)/tsan.log 2>&1; rc=$$?; \
	races=$$(grep -c 'WARNING: ThreadSanitizer' $(PROOF_OUT)/tsan.log); \
	echo "RESULT tsan.exit=$$rc" >> $(PROOF_OUT)/tsan.log; \
	echo "RESULT tsan.reports=$$races" >> $(PROOF_OUT)/tsan.log; \
	if [ "$$rc" = 0 ] && [ "$$races" = 0 ]; then v=PASS; else v=FAIL; fi; \
	echo "RESULT tsan=$$v" >> $(PROOF_OUT)/tsan.log; \
	grep '^RESULT tsan\|^WORLD DIFF' $(PROOF_OUT)/tsan.log; \
	grep -A12 'WARNING: ThreadSanitizer' $(PROOF_OUT)/tsan.log | head -60; \
	[ "$$v" = PASS ]

test-proof:
	@mkdir -p $(PROOF_OUT)
	@rm -f $(PROOF_OUT)/model.log $(PROOF_OUT)/diff.log $(PROOF_OUT)/tsan.log
	@if [ -e "$(PROOF_QUIET)" ]; then \
		echo "test-proof: NOT_RUN (quiet flag $(PROOF_QUIET) is set)"; \
		sh tests/model/receipt.sh $(PROOF_OUT) quiet; exit 1; fi
	-@$(MAKE) --no-print-directory test-model
	-@$(MAKE) --no-print-directory test-model-diff
	-@$(MAKE) --no-print-directory test-model-tsan
	sh tests/model/receipt.sh $(PROOF_OUT)
