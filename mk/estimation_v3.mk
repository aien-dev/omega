# ESTIMATION-2 protocol v3: discrete predictive layer (src/estimation/est_pred.c).
# Picked up by `-include mk/*.mk`; not part of `all` or `test`.
#
# test-est-pred       hostile suite, plain + ASan/UBSan
# est-v3-purity       nm -u on est_pred.o against EST_FORBIDDEN
# test-estimation-v3  both
ifndef ESTIMATION_V3_MK
ESTIMATION_V3_MK := 1
include mk/estimation.mk
.PHONY: test-est-pred est-v3-purity test-estimation-v3
ESTV3_DIR = $(OUT_DIR)/tests-estimation-v3
ESTV3_SRCS = src/estimation/est_pred.c src/estimation/est_mix.c $(EST_TYPES_SRCS)
ESTV3_HDRS = src/estimation/est_pred.h $(EST_HDRS)

$(ESTV3_DIR)/est_pred.o: src/estimation/est_pred.c $(ESTV3_HDRS)
	@mkdir -p $(ESTV3_DIR)
	$(CC) $(EST_CFLAGS) -c -o $@ $<

est-v3-purity: $(ESTV3_DIR)/est_pred.o
	@if nm -u $^ | grep -E $(EST_FORBIDDEN) ; then \
		echo "est_pred.o references an operation an estimator must not have"; exit 1; fi
	@echo "est-v3-purity: est_pred.o references no forbidden symbol"

$(ESTV3_DIR)/test_est_pred: tests/estimation/test_est_pred.c $(ESTV3_SRCS) $(ESTV3_HDRS)
	@mkdir -p $(ESTV3_DIR)
	$(CC) $(EST_CFLAGS) -o $@ tests/estimation/test_est_pred.c $(ESTV3_SRCS) -lm
$(ESTV3_DIR)/test_est_pred_asan: tests/estimation/test_est_pred.c $(ESTV3_SRCS) $(ESTV3_HDRS)
	@mkdir -p $(ESTV3_DIR)
	$(CC) $(EST_CFLAGS) $(EST_ASAN) -o $@ tests/estimation/test_est_pred.c $(ESTV3_SRCS) -lm

test-est-pred: $(ESTV3_DIR)/test_est_pred $(ESTV3_DIR)/test_est_pred_asan
	./$(ESTV3_DIR)/test_est_pred
	./$(ESTV3_DIR)/test_est_pred_asan
	@echo "test-est-pred: est_pred contract, numerics and calibration scenarios pass in plain and ASan/UBSan builds"

test-estimation-v3: est-v3-purity test-est-pred
	@echo "test-estimation-v3: ESTIMATION-2 v3 predictive layer passes"
endif
