# BRW-ACT-DEV0: active measurement choice, development demonstration (not EXP-003).
# New files only: tools/brownian/brw_active.*, tests/brownian/{test_brw_active,brw_active_dev0}.c.
# Picked up by `-include mk/*.mk`. Spec: docs/turing/BRW_ACT_DEV0_PROFILE.md.
#
# test-brownian-active: unit tests plain + ASan/UBSan, and the library purity check.
# brownian-active-dev0: builds the runner, runs the `dev` worlds twice, fails if the table digests differ.
# The `hold` worlds are run by hand once: build/brownian-active/brw_active_dev0 hold <fresh-dir> <commit>
ifndef BROWNIAN_ACTIVE_MK
BROWNIAN_ACTIVE_MK := 1
.PHONY: brownian-active-reanalyse test-brownian-active brownian-active-dev0 brwa-purity brwa-cli brownian-active-dev1-power brownian-active-dev1 brownian-active-dev0-regress
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

# runner argument contract: DEV1 needs an explicit budget, unknown profile ids are refused
brwa-cli: $(BRWA_DIR)/brw_active_dev0
	@$(BRWA_DIR)/brw_active_dev0 dev1 $(BRWA_DIR)/cli-x t none >/dev/null 2>&1; [ $$? -eq 64 ] || { echo "dev1 without B= must exit 64"; exit 1; }
	@$(BRWA_DIR)/brw_active_dev0 hold1 $(BRWA_DIR)/cli-x t B=2 >/dev/null 2>&1; [ $$? -eq 64 ] || { echo "B below 3 must exit 64"; exit 1; }
	@$(BRWA_DIR)/brw_active_dev0 bogus $(BRWA_DIR)/cli-x t >/dev/null 2>&1; [ $$? -eq 64 ] || { echo "unknown profile id must exit 64"; exit 1; }
	@echo "brwa-cli: runner argument contract holds"

$(BRWA_DIR)/test_brw_act_report: tests/brownian/test_brw_act_report.c tests/brownian/brw_act_report.h
	@mkdir -p $(BRWA_DIR)
	$(CC) $(BRWA_FLAGS) $(BRWA_ASAN) -o $@ tests/brownian/test_brw_act_report.c -lm
$(BRWA_DIR)/brw_act_reanalyse: tests/brownian/brw_act_reanalyse.c tests/brownian/brw_act_report.h
	@mkdir -p $(BRWA_DIR)
	$(CC) $(BRWA_FLAGS) -o $@ tests/brownian/brw_act_reanalyse.c -lm

test-brownian-active: brwa-purity brwa-cli $(BRWA_DIR)/test_brw_active $(BRWA_DIR)/test_brw_active_asan $(BRWA_DIR)/test_brw_act_report
	./$(BRWA_DIR)/test_brw_act_report
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

# ---- BRW-ACT-DEV1 (docs/turing/BRW_ACT_DEV1_PROFILE.md): same runner, profile ids dev1/hold1 and the power sub-command.
# Built into its own directory. The power receipt (evidence/BRW-ACT-DEV1/power/receipt.txt, never overwritten) fixes B.
BRWA1_DIR = $(OUT_DIR)/brownian-active-dev1
BRWA1_POWER = evidence/BRW-ACT-DEV1/power
$(BRWA1_DIR)/brw_active_dev0: tests/brownian/brw_active_dev0.c $(BRWA_LIB) src/sha256.c $(BRWA_HDRS)
	@mkdir -p $(BRWA1_DIR)
	$(CC) $(BRWA_FLAGS) -DBRW_FLAGS='"$(BRWA_FLAGS)"' -o $@ tests/brownian/brw_active_dev0.c $(BRWA_LIB) src/sha256.c -lm

# DEV0 must stay bit-identical under the parametrised runner: the table digest of the committed DEV0 dev run.
brownian-active-dev0-regress: $(BRWA1_DIR)/brw_active_dev0
	@rm -rf $(BRWA1_DIR)/regress0
	./$(BRWA1_DIR)/brw_active_dev0 dev $(BRWA1_DIR)/regress0 $(BRWA_COMMIT) none > /dev/null
	@a=$$(sha256sum < $(BRWA1_DIR)/regress0/table.tsv); b=$$(sha256sum < evidence/BRW-ACT-DEV0/dev/table.tsv); \
	  if [ "$$a" != "$$b" ]; then echo "DEV0 regress: table differs ($$a vs $$b)"; exit 1; fi; echo "DEV0 regress: table identical to evidence/BRW-ACT-DEV0/dev ($$a)"

brownian-active-dev1-power: $(BRWA1_DIR)/brw_active_dev0
	@mkdir -p $(dir $(BRWA1_POWER))
	./$(BRWA1_DIR)/brw_active_dev0 power $(BRWA1_POWER) $(BRWA_COMMIT) "$(BRW_DEVIATIONS)"

brownian-active-dev1: $(BRWA1_DIR)/brw_active_dev0
	@test -f $(BRWA1_POWER)/receipt.txt || { echo "run brownian-active-dev1-power first and commit its evidence"; exit 1; }
	@B=$$(sed -n 's/^chosen_B: //p' $(BRWA1_POWER)/receipt.txt); test -n "$$B" || { echo "no chosen_B in power receipt"; exit 1; }; \
	  rm -rf $(BRWA1_DIR)/dev1-a $(BRWA1_DIR)/dev1-b; \
	  echo "B=$$B"; \
	  ./$(BRWA1_DIR)/brw_active_dev0 dev1 $(BRWA1_DIR)/dev1-a $(BRWA_COMMIT) B=$$B "$(BRW_DEVIATIONS)" | tee $(BRWA1_DIR)/dev1-a.out | tail -14; \
	  ./$(BRWA1_DIR)/brw_active_dev0 dev1 $(BRWA1_DIR)/dev1-b $(BRWA_COMMIT) B=$$B "$(BRW_DEVIATIONS)" > $(BRWA1_DIR)/dev1-b.out; \
	  a=$$(sha256sum < $(BRWA1_DIR)/dev1-a/table.tsv); b=$$(sha256sum < $(BRWA1_DIR)/dev1-b/table.tsv); \
	  if [ "$$a" != "$$b" ]; then echo "brownian-active-dev1: REPLAY MISMATCH"; exit 1; fi; \
	  echo "brownian-active-dev1: two dev1 runs produced identical tables ($$a)"
# Re-analysis of the committed held-out tables (reads table.tsv only, runs no world). Writes a new evidence file,
# never over an existing one: evidence/BRW-ACT-DEV<n>/<run>-reanalysis/reanalysis.txt.
brownian-active-reanalyse: $(BRWA_DIR)/brw_act_reanalyse
	@for r in DEV0:hold:dev0 DEV1:hold1:dev1; do \
	  d=$${r%%:*}; rest=$${r#*:}; run=$${rest%%:*}; p=$${rest#*:}; t=evidence/BRW-ACT-$$d/$$run/table.tsv; \
	  o=evidence/BRW-ACT-$$d/$$run-reanalysis; test -e $$o/reanalysis.txt && { echo "$$o/reanalysis.txt exists, not overwritten"; continue; }; \
	  mkdir -p $$o; { echo "source commit: $(BRWA_COMMIT)"; echo "input: $$t sha256 $$(sha256sum < $$t | cut -c1-64)"; \
	    echo "tool: tests/brownian/brw_act_reanalyse.c sha256 $$(sha256sum < tests/brownian/brw_act_reanalyse.c | cut -c1-64)"; \
	    echo "flags: $(BRWA_FLAGS)"; ./$(BRWA_DIR)/brw_act_reanalyse $$p $$t; } > $$o/reanalysis.txt || exit 1; \
	  echo "wrote $$o/reanalysis.txt"; done

endif
