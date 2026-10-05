# ANS, Inertial Alignment v1 (docs/ans/INERTIAL_ALIGNMENT_SPEC_V1.md, ARCH-0032).
# Standalone: new files only under src/ans/, tests/ans/, docs/ans/. Links the
# EST-1 filter (est_kf.c); does not reimplement it. Picked up by
# `-include mk/*.mk`; not part of `all` or `test`. Nothing in src/runtime/
# includes or links it. ANS is advisory: it never authorizes or promotes.
#
# test-ans               unit suite (10 tests), plain + ASan/UBSan
# ans-purity             nm -u on every ans object against EST_FORBIDDEN and rx_gen_promote
# test-inertial-alignment  both
ifndef ANS_MK
ANS_MK := 1
include mk/estimation.mk
.PHONY: test-ans ans-purity test-inertial-alignment
ANS_CFLAGS = $(EST_CFLAGS) -Isrc/ans
ANS_DIR = $(OUT_DIR)/tests-ans
ANS_SRCS = src/ans/ans.c src/ans/ans_encode.c $(EST_KF_SRCS)
ANS_HDRS = src/ans/ans.h $(EST_HDRS)

$(ANS_DIR)/%.o: src/ans/%.c $(ANS_HDRS)
	@mkdir -p $(ANS_DIR)
	$(CC) $(ANS_CFLAGS) -c -o $@ $<

ans-purity: $(ANS_DIR)/ans.o $(ANS_DIR)/ans_encode.o
	@if nm -u $^ | grep -E $(EST_FORBIDDEN) || nm -u $^ | grep rx_gen_promote ; then \
		echo "ans object references an operation an advisory module must not have"; exit 1; fi
	@echo "ans-purity: ans.o and ans_encode.o reference no forbidden symbol and no rx_gen_promote"

$(ANS_DIR)/test_ans: tests/ans/test_ans.c $(ANS_SRCS) $(ANS_HDRS)
	@mkdir -p $(ANS_DIR)
	$(CC) $(ANS_CFLAGS) -o $@ tests/ans/test_ans.c $(ANS_SRCS) -lm
$(ANS_DIR)/test_ans_asan: tests/ans/test_ans.c $(ANS_SRCS) $(ANS_HDRS)
	@mkdir -p $(ANS_DIR)
	$(CC) $(ANS_CFLAGS) $(EST_ASAN) -o $@ tests/ans/test_ans.c $(ANS_SRCS) -lm

test-ans: $(ANS_DIR)/test_ans $(ANS_DIR)/test_ans_asan
	./$(ANS_DIR)/test_ans
	./$(ANS_DIR)/test_ans_asan
	@echo "test-ans: ANS checks pass in plain and ASan/UBSan builds"

test-inertial-alignment: ans-purity test-ans
	@echo "test-inertial-alignment: Inertial Alignment v1 (ANS) passes"
endif
