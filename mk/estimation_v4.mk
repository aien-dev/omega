# ESTIMATION v4 (docs/estimation/protocols/est-v4.md): lag / AR-change /
# two-time-constant families (src/estimation/est_v4.c) and the est4 tool.
# Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# test-est-v4         unit suite (analytic multi-step propagation), plain + ASan/UBSan
# est-v4-purity       nm -u on est_v4.o against EST_FORBIDDEN
# test-est4-tools     tool tests on synthetic data (held-out refusal, open trace, binding refusals)
# test-estimation-v4  all three
# est4                the binding tool (identity compiled in)
ifndef ESTIMATION_V4_MK
ESTIMATION_V4_MK := 1
include mk/estimation.mk
.PHONY: test-est-v4 est-v4-purity test-est4-tools test-estimation-v4 est4 est4-force
ESTV4_DIR = $(OUT_DIR)/estimation-v4
ESTV4_CFLAGS = $(EST_CFLAGS) -Itools/estimation
ESTV4_SRCS = src/estimation/est_v4.c src/estimation/est_pred.c src/estimation/est_mix.c $(EST_TYPES_SRCS)
ESTV4_HDRS = src/estimation/est_v4.h src/estimation/est_pred.h $(EST_HDRS)
ESTV4_TOOL_SRCS = tools/estimation/est4.c tools/estimation/est3c_common.c tools/estimation/est_replay.c \
	src/estimation/est_v4.c src/estimation/est_pred.c src/estimation/est_mix.c $(EST_KF_SRCS)
ESTV4_TOOL_HDRS = tools/estimation/est3c_common.h tools/estimation/est_replay.h $(ESTV4_HDRS)
# identity baked in: commit, dirty flag, protocol SHA, D1 identity
# (receipts/est-v4/d1.sha256), D2 identity (d2.sha256), params SHA, repo root
ESTV4_COMMIT := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
ESTV4_DIRTY := $(shell if [ -n "$$(git status --porcelain 2>/dev/null)" ]; then echo 1; else echo 0; fi)
ESTV4_PROTO := $(shell sha256sum docs/estimation/protocols/est-v4.md 2>/dev/null | cut -c1-64)
ESTV4_RC = docs/estimation/receipts/est-v4
ESTV4_SUM = $(shell awk '$$2 == "$(2)" && length($$1) == 64 { print $$1 }' $(ESTV4_RC)/$(1) 2>/dev/null | head -1)
ESTV4_D1RAW := $(call ESTV4_SUM,d1.sha256,machine-state.ndjson)
ESTV4_D1MARKS := $(call ESTV4_SUM,d1.sha256,machine-state-marks.txt)
ESTV4_D2RAW := $(call ESTV4_SUM,d2.sha256,machine-state.ndjson)
ESTV4_D2MARKS := $(call ESTV4_SUM,d2.sha256,machine-state-marks.txt)
ESTV4_D2SCHED := $(call ESTV4_SUM,d2.sha256,schedule.txt)
ESTV4_PARAMS := $(shell sha256sum $(ESTV4_RC)/params.txt 2>/dev/null | cut -c1-64)
ESTV4_ROOT := $(shell git rev-parse --show-toplevel 2>/dev/null || echo unknown)
ESTV4_OR = $(if $(1),$(1),$(2))
ESTV4_ID = -DTOOL_COMMIT='"$(ESTV4_COMMIT)"' -DTOOL_DIRTY=$(ESTV4_DIRTY) \
	-DE4_PROTOCOL_SHA='"$(call ESTV4_OR,$(ESTV4_PROTO),unknown)"' \
	-DE4_D1_RAW_SHA='"$(call ESTV4_OR,$(ESTV4_D1RAW),absent)"' -DE4_D1_MARKS_SHA='"$(call ESTV4_OR,$(ESTV4_D1MARKS),absent)"' \
	-DE4_D2_RAW_SHA='"$(call ESTV4_OR,$(ESTV4_D2RAW),absent)"' -DE4_D2_MARKS_SHA='"$(call ESTV4_OR,$(ESTV4_D2MARKS),absent)"' \
	-DE4_D2_SCHED_SHA='"$(call ESTV4_OR,$(ESTV4_D2SCHED),absent)"' -DE4_PARAMS_SHA='"$(call ESTV4_OR,$(ESTV4_PARAMS),absent)"' \
	-DE4_REPO_ROOT='"$(ESTV4_ROOT)"'

$(ESTV4_DIR)/toolid.stamp: est4-force
	@mkdir -p $(ESTV4_DIR)
	@echo '$(ESTV4_ID)' > $(ESTV4_DIR)/toolid.new; \
	cmp -s $(ESTV4_DIR)/toolid.new $(ESTV4_DIR)/toolid.stamp || mv $(ESTV4_DIR)/toolid.new $(ESTV4_DIR)/toolid.stamp; rm -f $(ESTV4_DIR)/toolid.new
est4-force: ;

$(ESTV4_DIR)/est_v4.o: src/estimation/est_v4.c $(ESTV4_HDRS)
	@mkdir -p $(ESTV4_DIR)
	$(CC) $(EST_CFLAGS) -c -o $@ $<
est-v4-purity: $(ESTV4_DIR)/est_v4.o
	@if nm -u $^ | grep -E $(EST_FORBIDDEN) ; then \
		echo "est_v4.o references an operation an estimator must not have"; exit 1; fi
	@echo "est-v4-purity: est_v4.o references no forbidden symbol"

$(ESTV4_DIR)/test_est_v4: tests/estimation/test_est_v4.c $(ESTV4_SRCS) $(ESTV4_HDRS)
	@mkdir -p $(ESTV4_DIR)
	$(CC) $(EST_CFLAGS) -o $@ tests/estimation/test_est_v4.c $(ESTV4_SRCS) -lm
$(ESTV4_DIR)/test_est_v4_asan: tests/estimation/test_est_v4.c $(ESTV4_SRCS) $(ESTV4_HDRS)
	@mkdir -p $(ESTV4_DIR)
	$(CC) $(EST_CFLAGS) $(EST_ASAN) -o $@ tests/estimation/test_est_v4.c $(ESTV4_SRCS) -lm
test-est-v4: $(ESTV4_DIR)/test_est_v4 $(ESTV4_DIR)/test_est_v4_asan
	./$(ESTV4_DIR)/test_est_v4
	./$(ESTV4_DIR)/test_est_v4_asan
	@echo "test-est-v4: v4 families, multi-step propagation and pmf agree in plain and ASan/UBSan builds"

$(ESTV4_DIR)/est4: $(ESTV4_TOOL_SRCS) $(ESTV4_TOOL_HDRS) $(ESTV4_DIR)/toolid.stamp
	$(CC) $(ESTV4_CFLAGS) $(ESTV4_ID) -o $@ $(ESTV4_TOOL_SRCS) -lm
est4: $(ESTV4_DIR)/est4
# test build: identities from E4_TEST_* environment variables
$(ESTV4_DIR)/est4_test: $(ESTV4_TOOL_SRCS) $(ESTV4_TOOL_HDRS)
	@mkdir -p $(ESTV4_DIR)
	$(CC) $(ESTV4_CFLAGS) -DE4_TEST_BUILD -DTOOL_COMMIT='"test-build"' -DTOOL_DIRTY=1 -o $@ $(ESTV4_TOOL_SRCS) -lm
# -Wno-format-truncation: est3c_common.c (v3, unchanged) trips a fortify false positive at -O1
$(ESTV4_DIR)/est4_test_asan: $(ESTV4_TOOL_SRCS) $(ESTV4_TOOL_HDRS)
	@mkdir -p $(ESTV4_DIR)
	$(CC) $(ESTV4_CFLAGS) $(EST_ASAN) -Wno-format-truncation -DE4_TEST_BUILD -DTOOL_COMMIT='"test-build"' -DTOOL_DIRTY=1 -o $@ $(ESTV4_TOOL_SRCS) -lm
test-est4-tools: $(ESTV4_DIR)/est4 $(ESTV4_DIR)/est4_test $(ESTV4_DIR)/est4_test_asan
	sh tools/estimation/test_est4_tools.sh ./$(ESTV4_DIR)/est4_test $(ESTV4_DIR)/work
	sh tools/estimation/test_est4_tools.sh ./$(ESTV4_DIR)/est4_test_asan $(ESTV4_DIR)/work-asan
	@echo "test-est4-tools: fit, held-out refusal, open-trace purity, binding refusals and receipt path pass (synthetic data)"

test-estimation-v4: est-v4-purity test-est-v4 test-est4-tools
	@echo "test-estimation-v4: ESTIMATION v4 passes"
endif
