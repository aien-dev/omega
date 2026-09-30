# EST-2/EST-3 replay, fit and evaluation tools (protocol v1). Not part of
# `all` or `test`. Tools may open files and print; est_types.o and est_kf.o
# stay pure (est-purity, unchanged).
ifndef ESTIMATION_TOOLS_MK
ESTIMATION_TOOLS_MK := 1
include mk/estimation.mk
.PHONY: test-est-tools est-tools-purity est-tools
ESTT_DIR = $(OUT_DIR)/est-tools
ESTT_CFLAGS = $(EST_CFLAGS) -Itools/estimation
ESTT_LIB = tools/estimation/est_replay.c $(EST_KF_SRCS)
ESTT_HDRS = tools/estimation/est_replay.h $(EST_HDRS)
ESTT_FORBIDDEN = 'rx_|aienos_|argus_|forge_|aegis|mmap|mprotect|fork|exec|dlopen|socket|system'

$(ESTT_DIR)/est_replay.o: tools/estimation/est_replay.c $(ESTT_HDRS)
	@mkdir -p $(ESTT_DIR)
	$(CC) $(ESTT_CFLAGS) -c -o $@ $<
$(ESTT_DIR)/est_fit.o: tools/estimation/est_fit.c $(ESTT_HDRS)
	@mkdir -p $(ESTT_DIR)
	$(CC) $(ESTT_CFLAGS) -c -o $@ $<
$(ESTT_DIR)/est_eval.o: tools/estimation/est_eval.c $(ESTT_HDRS)
	@mkdir -p $(ESTT_DIR)
	$(CC) $(ESTT_CFLAGS) -c -o $@ $<
$(ESTT_DIR)/est_fit: $(ESTT_DIR)/est_fit.o $(ESTT_LIB) $(ESTT_HDRS)
	$(CC) $(ESTT_CFLAGS) -o $@ $(ESTT_DIR)/est_fit.o $(ESTT_LIB) -lm
$(ESTT_DIR)/est_eval: $(ESTT_DIR)/est_eval.o $(ESTT_LIB) $(ESTT_HDRS)
	$(CC) $(ESTT_CFLAGS) -o $@ $(ESTT_DIR)/est_eval.o $(ESTT_LIB) -lm
$(ESTT_DIR)/est_replay: $(ESTT_LIB) $(ESTT_HDRS)
	@mkdir -p $(ESTT_DIR)
	$(CC) $(ESTT_CFLAGS) -DEST_REPLAY_MAIN -o $@ $(ESTT_LIB) -lm
$(ESTT_DIR)/test_est_tools: tools/estimation/test_est_tools.c $(ESTT_LIB) $(ESTT_HDRS)
	@mkdir -p $(ESTT_DIR)
	$(CC) $(ESTT_CFLAGS) -o $@ tools/estimation/test_est_tools.c $(ESTT_LIB) -lm

est-tools: $(ESTT_DIR)/est_fit $(ESTT_DIR)/est_eval $(ESTT_DIR)/est_replay

est-tools-purity: est-purity $(ESTT_DIR)/est_replay.o $(ESTT_DIR)/est_fit.o $(ESTT_DIR)/est_eval.o
	@if nm -u $(ESTT_DIR)/est_replay.o $(ESTT_DIR)/est_fit.o $(ESTT_DIR)/est_eval.o | grep -E $(ESTT_FORBIDDEN) ; then \
		echo "estimation tool references an authority, World or process operation"; exit 1; fi
	@echo "est-tools-purity: tool objects reference no authority/process symbol; est_types.o and est_kf.o pure"

test-est-tools: est-tools-purity $(ESTT_DIR)/est_fit $(ESTT_DIR)/est_eval $(ESTT_DIR)/test_est_tools
	./$(ESTT_DIR)/test_est_tools ./$(ESTT_DIR)/est_fit ./$(ESTT_DIR)/est_eval $(ESTT_DIR)/work
	@echo "test-est-tools: replay, fit and evaluation tools pass on run A and synthetic data"
endif
