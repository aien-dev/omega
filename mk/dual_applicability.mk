# DUAL applicability experiment (ARCH-0031 / ADR 0031, offline evidence lane):
# when is aggregate (macroscopic) load control predictive, and when must
# discrete per-item scheduling dominate? Standalone: new files only under
# src/dual_experiment/, tools/dual_experiment/, tests/dual_experiment/,
# docs/dual/, evidence/DUAL/applicability/. Picked up by `-include mk/*.mk`;
# not part of `all` or `test`. Nothing in src/runtime/ includes or links it.
# EXPERIMENT ONLY: no production gate, no production classifier.
#
# test-dual-applicability  unit suite (determinism, ledger, predictor, controls),
#                          plain + ASan/UBSan, plus the purity check
# dual-applic-purity       nm -u on every src/dual_experiment object
# dual-applic-run          run the campaign, write evidence/DUAL/applicability/
ifndef DUAL_APPLICABILITY_MK
DUAL_APPLICABILITY_MK := 1
include mk/estimation.mk
.PHONY: test-dual-applicability dual-applic-purity dual-applic-run
DAP_CFLAGS = $(EST_CFLAGS) -Isrc/dual_experiment
DAP_DIR = $(OUT_DIR)/dual-applicability
DAP_SRCS = src/dual_experiment/applic_sim.c src/dual_experiment/applic_predict.c src/dual_experiment/applic_metrics.c src/sha256.c
DAP_HDRS = src/dual_experiment/applic.h src/dual_experiment/applic_params.h src/sha256.h
DAP_EVIDENCE = evidence/DUAL/applicability

$(DAP_DIR)/%.o: src/dual_experiment/%.c $(DAP_HDRS)
	@mkdir -p $(DAP_DIR)
	$(CC) $(DAP_CFLAGS) -c -o $@ $<

# The experiment library is a calculator: no authority, World, generation,
# capability, memory-mapping, process or file operations.
dual-applic-purity: $(DAP_DIR)/applic_sim.o $(DAP_DIR)/applic_predict.o $(DAP_DIR)/applic_metrics.o
	@if nm -u $^ | grep -E $(EST_FORBIDDEN) ; then \
		echo "dual_experiment object references an operation an experiment library must not have"; exit 1; fi
	@echo "dual-applic-purity: applic_sim.o, applic_predict.o and applic_metrics.o reference no forbidden symbol"

$(DAP_DIR)/test_applic: tests/dual_experiment/test_applic.c $(DAP_SRCS) $(DAP_HDRS)
	@mkdir -p $(DAP_DIR)
	$(CC) $(DAP_CFLAGS) -o $@ tests/dual_experiment/test_applic.c $(DAP_SRCS) -lm
$(DAP_DIR)/test_applic_asan: tests/dual_experiment/test_applic.c $(DAP_SRCS) $(DAP_HDRS)
	@mkdir -p $(DAP_DIR)
	$(CC) $(DAP_CFLAGS) $(EST_ASAN) -o $@ tests/dual_experiment/test_applic.c $(DAP_SRCS) -lm

$(DAP_DIR)/applic_run: tools/dual_experiment/applic_run.c $(DAP_SRCS) $(DAP_HDRS)
	@mkdir -p $(DAP_DIR)
	$(CC) $(DAP_CFLAGS) -o $@ tools/dual_experiment/applic_run.c $(DAP_SRCS) -lm

test-dual-applicability: dual-applic-purity $(DAP_DIR)/test_applic $(DAP_DIR)/test_applic_asan
	./$(DAP_DIR)/test_applic
	./$(DAP_DIR)/test_applic_asan
	@echo "test-dual-applicability: determinism, ledger, predictor and negative-control checks pass in plain and ASan/UBSan builds"

dual-applic-run: $(DAP_DIR)/applic_run
	@mkdir -p $(DAP_EVIDENCE)
	./$(DAP_DIR)/applic_run $(DAP_EVIDENCE)
endif
