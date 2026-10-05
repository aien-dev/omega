# ESTIMATION v5 (docs/estimation/protocols/est-v5.md): the v4 families (src/estimation/est_v4.c,
# unchanged) with the est5 tool (the est4 tool with v5 paths, tags and identities) and an
# enforced collection window. Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# test-est5-tools     tool tests on synthetic data (held-out refusal, open trace, binding refusals)
# test-est5-window    collector and window-check tests (no data, no load)
# test-estimation-v5  v4 library tests (reused, unchanged) + the two above
# est5                the binding tool (identity compiled in)
ifndef ESTIMATION_V5_MK
ESTIMATION_V5_MK := 1
include mk/estimation_v4.mk
.PHONY: test-est5-tools test-est5-window test-estimation-v5 est5 est5-force
ESTV5_DIR = $(OUT_DIR)/estimation-v5
ESTV5_CFLAGS = $(EST_CFLAGS) -Itools/estimation
ESTV5_TOOL_SRCS = tools/estimation/est5.c tools/estimation/est3c_common.c tools/estimation/est_replay.c \
	src/estimation/est_v4.c src/estimation/est_pred.c src/estimation/est_mix.c $(EST_KF_SRCS)
ESTV5_TOOL_HDRS = tools/estimation/est3c_common.h tools/estimation/est_replay.h $(ESTV4_HDRS)
# identity baked in: commit, dirty flag, protocol SHA, D1 identity
# (receipts/est-v5/d1.sha256), D2 identity (d2.sha256), params SHA, repo root
ESTV5_COMMIT := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
ESTV5_DIRTY := $(shell if [ -n "$$(git status --porcelain 2>/dev/null)" ]; then echo 1; else echo 0; fi)
ESTV5_PROTO := $(shell sha256sum docs/estimation/protocols/est-v5.md 2>/dev/null | cut -c1-64)
ESTV5_RC = docs/estimation/receipts/est-v5
ESTV5_SUM = $(shell awk '$$2 == "$(2)" && length($$1) == 64 { print $$1 }' $(ESTV5_RC)/$(1) 2>/dev/null | head -1)
ESTV5_D1RAW := $(call ESTV5_SUM,d1.sha256,machine-state.ndjson)
ESTV5_D1MARKS := $(call ESTV5_SUM,d1.sha256,machine-state-marks.txt)
ESTV5_D2RAW := $(call ESTV5_SUM,d2.sha256,machine-state.ndjson)
ESTV5_D2MARKS := $(call ESTV5_SUM,d2.sha256,machine-state-marks.txt)
ESTV5_D2SCHED := $(call ESTV5_SUM,d2.sha256,schedule.txt)
ESTV5_PARAMS := $(shell sha256sum $(ESTV5_RC)/params.txt 2>/dev/null | cut -c1-64)
ESTV5_ROOT := $(shell git rev-parse --show-toplevel 2>/dev/null || echo unknown)
ESTV5_OR = $(if $(1),$(1),$(2))
ESTV5_ID = -DTOOL_COMMIT='"$(ESTV5_COMMIT)"' -DTOOL_DIRTY=$(ESTV5_DIRTY) \
	-DE5_PROTOCOL_SHA='"$(call ESTV5_OR,$(ESTV5_PROTO),unknown)"' \
	-DE5_D1_RAW_SHA='"$(call ESTV5_OR,$(ESTV5_D1RAW),absent)"' -DE5_D1_MARKS_SHA='"$(call ESTV5_OR,$(ESTV5_D1MARKS),absent)"' \
	-DE5_D2_RAW_SHA='"$(call ESTV5_OR,$(ESTV5_D2RAW),absent)"' -DE5_D2_MARKS_SHA='"$(call ESTV5_OR,$(ESTV5_D2MARKS),absent)"' \
	-DE5_D2_SCHED_SHA='"$(call ESTV5_OR,$(ESTV5_D2SCHED),absent)"' -DE5_PARAMS_SHA='"$(call ESTV5_OR,$(ESTV5_PARAMS),absent)"' \
	-DE5_REPO_ROOT='"$(ESTV5_ROOT)"'

$(ESTV5_DIR)/toolid.stamp: est5-force
	@mkdir -p $(ESTV5_DIR)
	@echo '$(ESTV5_ID)' > $(ESTV5_DIR)/toolid.new; \
	cmp -s $(ESTV5_DIR)/toolid.new $(ESTV5_DIR)/toolid.stamp || mv $(ESTV5_DIR)/toolid.new $(ESTV5_DIR)/toolid.stamp; rm -f $(ESTV5_DIR)/toolid.new
est5-force: ;

$(ESTV5_DIR)/est5: $(ESTV5_TOOL_SRCS) $(ESTV5_TOOL_HDRS) $(ESTV5_DIR)/toolid.stamp
	$(CC) $(ESTV5_CFLAGS) $(ESTV5_ID) -o $@ $(ESTV5_TOOL_SRCS) -lm
est5: $(ESTV5_DIR)/est5
# test build: identities from E5_TEST_* environment variables
$(ESTV5_DIR)/est5_test: $(ESTV5_TOOL_SRCS) $(ESTV5_TOOL_HDRS)
	@mkdir -p $(ESTV5_DIR)
	$(CC) $(ESTV5_CFLAGS) -DE5_TEST_BUILD -DTOOL_COMMIT='"test-build"' -DTOOL_DIRTY=1 -o $@ $(ESTV5_TOOL_SRCS) -lm
# -Wno-format-truncation: est3c_common.c (v3, unchanged) trips a fortify false positive at -O1
$(ESTV5_DIR)/est5_test_asan: $(ESTV5_TOOL_SRCS) $(ESTV5_TOOL_HDRS)
	@mkdir -p $(ESTV5_DIR)
	$(CC) $(ESTV5_CFLAGS) $(EST_ASAN) -Wno-format-truncation -DE5_TEST_BUILD -DTOOL_COMMIT='"test-build"' -DTOOL_DIRTY=1 -o $@ $(ESTV5_TOOL_SRCS) -lm
test-est5-tools: $(ESTV5_DIR)/est5 $(ESTV5_DIR)/est5_test $(ESTV5_DIR)/est5_test_asan
	sh tools/estimation/test_est5_tools.sh ./$(ESTV5_DIR)/est5_test $(ESTV5_DIR)/work
	sh tools/estimation/test_est5_tools.sh ./$(ESTV5_DIR)/est5_test_asan $(ESTV5_DIR)/work-asan
	@echo "test-est5-tools: fit, held-out refusal, open-trace purity, binding refusals and receipt path pass (synthetic data)"

test-est5-window:
	sh tools/estimation/test_est5_window.sh

test-estimation-v5: est-v4-purity test-est-v4 test-est5-tools test-est5-window
	@echo "test-estimation-v5: ESTIMATION v5 passes"
endif
