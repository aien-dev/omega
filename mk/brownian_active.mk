# BRW-ACT-DEV0: active measurement choice, development demonstration (not EXP-003).
# New files only: tools/brownian/brw_active.*, tests/brownian/{test_brw_active,brw_active_dev0}.c.
# Picked up by `-include mk/*.mk`. Spec: docs/turing/BRW_ACT_DEV0_PROFILE.md.
#
# test-brownian-active: unit tests plain + ASan/UBSan, and the library purity check.
# brownian-active-dev0: builds the runner, runs the `dev` worlds twice, fails if the table digests differ.
# The `hold` worlds are run by hand once: build/brownian-active/brw_active_dev0 hold <fresh-dir> <commit>
ifndef BROWNIAN_ACTIVE_MK
BROWNIAN_ACTIVE_MK := 1
.PHONY: test-brownian-active brownian-active-dev0 brwa-purity
BRWA_FLAGS = -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L -Isrc -Itools -ffp-contract=off -fno-fast-math
BRWA_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
BRWA_LIB = tools/brownian/brw_active.c src/turing/ty_prd2.c src/turing/ty_qcont.c src/turing/ty_prd.c src/turing/ty_math.c
BRWA_HDRS = tools/brownian/brw_active.h src/turing/ty_prd2.h src/turing/ty_qcont.h src/turing/ty_prd.h src/turing/ty_math.h src/sha256.h
BRWA_DIR = $(OUT_DIR)/brownian-active
BRW_DEVIATIONS ?= none
BRWA_COMMIT = $(shell git rev-parse HEAD 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD -- tools tests mk src docs 2>/dev/null || echo -dirty)
# the candidate library must not name a world, a truth, or a generator
BRWA_FORBIDDEN = 'world|truth|oracle|splitmix|xorshift|poisson|box_muller|rand|generator|hidden|heldout'

$(BRWA_DIR)/test_brw_active: tests/brownian/test_brw_active.c $(BRWA_LIB) $(BRWA_HDRS)
	@mkdir -p $(BRWA_DIR)
	$(CC) $(BRWA_FLAGS) -o $@ tests/brownian/test_brw_active.c $(BRWA_LIB) -lm
$(BRWA_DIR)/test_brw_active_asan: tests/brownian/test_brw_active.c $(BRWA_LIB) $(BRWA_HDRS)
	@mkdir -p $(BRWA_DIR)
	$(CC) $(BRWA_FLAGS) $(BRWA_ASAN) -o $@ tests/brownian/test_brw_active.c $(BRWA_LIB) -lm
$(BRWA_DIR)/brw_active_dev0: tests/brownian/brw_active_dev0.c $(BRWA_LIB) src/sha256.c $(BRWA_HDRS)
	@mkdir -p $(BRWA_DIR)
	$(CC) $(BRWA_FLAGS) -DBRW_FLAGS='"$(BRWA_FLAGS)"' -o $@ tests/brownian/brw_active_dev0.c $(BRWA_LIB) src/sha256.c -lm

brwa-purity:
	@mkdir -p $(BRWA_DIR)/purity
	@$(CC) $(BRWA_FLAGS) -c -o $(BRWA_DIR)/purity/brw_active.o tools/brownian/brw_active.c
	@if grep -n -i -E $(BRWA_FORBIDDEN) tools/brownian/brw_active.c tools/brownian/brw_active.h; then echo "brw_active names a world, truth or generator"; exit 1; fi
	@if nm $(BRWA_DIR)/purity/brw_active.o | grep -i -E $(BRWA_FORBIDDEN); then echo "brw_active object references a generator or world symbol"; exit 1; fi
	@echo "brwa-purity: brw_active carries no world, truth or generator symbol"

test-brownian-active: brwa-purity $(BRWA_DIR)/test_brw_active $(BRWA_DIR)/test_brw_active_asan
	./$(BRWA_DIR)/test_brw_active
	./$(BRWA_DIR)/test_brw_active_asan
	@echo "test-brownian-active: candidate library unit tests pass in plain and ASan/UBSan builds; purity holds"

brownian-active-dev0: $(BRWA_DIR)/brw_active_dev0
	@rm -rf $(BRWA_DIR)/dev-a $(BRWA_DIR)/dev-b
	./$(BRWA_DIR)/brw_active_dev0 dev $(BRWA_DIR)/dev-a $(BRWA_COMMIT) "$(BRW_DEVIATIONS)" | tee $(BRWA_DIR)/dev-a.out | tail -12
	./$(BRWA_DIR)/brw_active_dev0 dev $(BRWA_DIR)/dev-b $(BRWA_COMMIT) "$(BRW_DEVIATIONS)" > $(BRWA_DIR)/dev-b.out
	@a=$$(sha256sum < $(BRWA_DIR)/dev-a/table.tsv); b=$$(sha256sum < $(BRWA_DIR)/dev-b/table.tsv); \
	  if [ "$$a" != "$$b" ]; then echo "brownian-active-dev0: REPLAY MISMATCH ($$a vs $$b)"; exit 1; fi; \
	  echo "brownian-active-dev0: two dev runs produced identical tables ($$a)"
endif
