# EST-3c protocol v3 tools: est3c_fit, est3c_eval and their tests
# (docs/estimation/EST3C_PROTOCOL_V3.md). Not part of `all` or `test`.
# est_pred.c is compiled by its own rule here into $(ESTV3T_DIR), separate from
# mk/estimation_v3.mk's objects.
ifndef ESTIMATION_V3_TOOLS_MK
ESTIMATION_V3_TOOLS_MK := 1
include mk/estimation.mk
.PHONY: est3c-fit est3c-eval test-est3c-tools est3c-tools-stamp
ESTV3T_DIR = $(OUT_DIR)/est3c-tools
ESTV3T_CFLAGS = $(EST_CFLAGS) -Itools/estimation
ESTV3T_HDRS = tools/estimation/est3c_common.h tools/estimation/est_replay.h src/estimation/est_pred.h src/estimation/est_mix.h $(EST_HDRS)
ESTV3T_LIB = $(ESTV3T_DIR)/est3c_common.o $(ESTV3T_DIR)/est_pred.o tools/estimation/est_replay.c src/estimation/est_mix.c $(EST_KF_SRCS)
# Tool identity baked in at build time: commit, dirty flag, protocol SHA and,
# once they are committed, the D2 identity (ndjson, marks, schedule.txt) from
# receipts/est3c-v3/d2.sha256, the SHA-256 of receipts/est3c-v3/params.txt and the repo root
# ("absent" before then: est3c_eval --recorded refuses).
ESTV3T_COMMIT := $(shell git rev-parse HEAD 2>/dev/null || echo unknown)
ESTV3T_DIRTY := $(shell if [ -n "$$(git status --porcelain 2>/dev/null)" ]; then echo 1; else echo 0; fi)
ESTV3T_PROTO := $(shell sha256sum docs/estimation/EST3C_PROTOCOL_V3.md 2>/dev/null | cut -c1-64)
ESTV3T_D2FILE = docs/estimation/receipts/est3c-v3/d2.sha256
ESTV3T_D2RAW := $(shell awk '$$2 == "machine-state.ndjson" && length($$1) == 64 { print $$1 }' $(ESTV3T_D2FILE) 2>/dev/null | head -1)
ESTV3T_D2MARKS := $(shell awk '$$2 == "machine-state-marks.txt" && length($$1) == 64 { print $$1 }' $(ESTV3T_D2FILE) 2>/dev/null | head -1)
ESTV3T_D2SCHED := $(shell awk '$$2 == "schedule.txt" && length($$1) == 64 { print $$1 }' $(ESTV3T_D2FILE) 2>/dev/null | head -1)
# SHA-256 of the committed binding params ("absent" before then: est3c_eval --recorded refuses)
ESTV3T_PARAMS := $(shell sha256sum docs/estimation/receipts/est3c-v3/params.txt 2>/dev/null | cut -c1-64)
ESTV3T_ROOT := $(shell git rev-parse --show-toplevel 2>/dev/null || echo unknown)
ESTV3T_PID = -DC3_PROTOCOL_SHA='"$(if $(ESTV3T_PROTO),$(ESTV3T_PROTO),unknown)"'
ESTV3T_ID = -DTOOL_COMMIT='"$(ESTV3T_COMMIT)"' -DTOOL_DIRTY=$(ESTV3T_DIRTY) $(ESTV3T_PID) \
	-DC3_D2_RAW_SHA='"$(if $(ESTV3T_D2RAW),$(ESTV3T_D2RAW),absent)"' -DC3_D2_MARKS_SHA='"$(if $(ESTV3T_D2MARKS),$(ESTV3T_D2MARKS),absent)"' \
	-DC3_D2_SCHED_SHA='"$(if $(ESTV3T_D2SCHED),$(ESTV3T_D2SCHED),absent)"' -DC3_PARAMS_SHA='"$(if $(ESTV3T_PARAMS),$(ESTV3T_PARAMS),absent)"' \
	-DC3_REPO_ROOT='"$(ESTV3T_ROOT)"'

est3c-tools-stamp:
	@mkdir -p $(ESTV3T_DIR)
	@echo "$(ESTV3T_COMMIT) $(ESTV3T_DIRTY) $(ESTV3T_PROTO) $(ESTV3T_D2RAW) $(ESTV3T_D2MARKS) $(ESTV3T_D2SCHED) $(ESTV3T_PARAMS) $(ESTV3T_ROOT)" > $(ESTV3T_DIR)/toolid.new; \
	cmp -s $(ESTV3T_DIR)/toolid.new $(ESTV3T_DIR)/toolid.stamp || mv $(ESTV3T_DIR)/toolid.new $(ESTV3T_DIR)/toolid.stamp; rm -f $(ESTV3T_DIR)/toolid.new
$(ESTV3T_DIR)/toolid.stamp: | est3c-tools-stamp

$(ESTV3T_DIR)/est_pred.o: src/estimation/est_pred.c $(ESTV3T_HDRS)
	@mkdir -p $(ESTV3T_DIR)
	$(CC) $(ESTV3T_CFLAGS) -c -o $@ $<
$(ESTV3T_DIR)/est3c_common.o: tools/estimation/est3c_common.c $(ESTV3T_HDRS)
	@mkdir -p $(ESTV3T_DIR)
	$(CC) $(ESTV3T_CFLAGS) -c -o $@ $<
$(ESTV3T_DIR)/est3c_fit: tools/estimation/est3c_fit.c $(ESTV3T_LIB) $(ESTV3T_HDRS) $(ESTV3T_DIR)/toolid.stamp
	$(CC) $(ESTV3T_CFLAGS) $(ESTV3T_ID) -o $@ $< $(ESTV3T_LIB) -lm
$(ESTV3T_DIR)/est3c_eval: tools/estimation/est3c_eval.c $(ESTV3T_LIB) $(ESTV3T_HDRS) $(ESTV3T_DIR)/toolid.stamp
	$(CC) $(ESTV3T_CFLAGS) $(ESTV3T_ID) -o $@ $< $(ESTV3T_LIB) -lm
# test build: both tools linked in-process; dirty flag, D1 folder and D2 identity from the environment
ESTV3T_TFLAGS = $(ESTV3T_CFLAGS) -DC3_AS_LIB -DC3_TEST_BUILD -DTOOL_COMMIT='"test-build"' -DTOOL_DIRTY=1 $(ESTV3T_PID)
$(ESTV3T_DIR)/t_fit.o: tools/estimation/est3c_fit.c $(ESTV3T_HDRS) $(ESTV3T_DIR)/toolid.stamp
	$(CC) $(ESTV3T_TFLAGS) -c -o $@ $<
$(ESTV3T_DIR)/t_eval.o: tools/estimation/est3c_eval.c $(ESTV3T_HDRS) $(ESTV3T_DIR)/toolid.stamp
	$(CC) $(ESTV3T_TFLAGS) -c -o $@ $<
$(ESTV3T_DIR)/test_est3c_tools: tools/estimation/test_est3c_tools.c $(ESTV3T_DIR)/t_fit.o $(ESTV3T_DIR)/t_eval.o $(ESTV3T_LIB) $(ESTV3T_HDRS)
	$(CC) $(ESTV3T_TFLAGS) -o $@ $< $(ESTV3T_DIR)/t_fit.o $(ESTV3T_DIR)/t_eval.o $(ESTV3T_LIB) -lm

est3c-fit: $(ESTV3T_DIR)/est3c_fit
est3c-eval: $(ESTV3T_DIR)/est3c_eval

test-est3c-tools: $(ESTV3T_DIR)/est3c_fit $(ESTV3T_DIR)/est3c_eval $(ESTV3T_DIR)/test_est3c_tools
	rm -rf $(ESTV3T_DIR)/work
	./$(ESTV3T_DIR)/test_est3c_tools $(ESTV3T_DIR)/work
	@echo "test-est3c-tools: tick mapping, pre-check triggers, marks pairs, selection, E0, receipt refusals pass (synthetic data)"
endif
