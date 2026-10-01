# M22 OMEGA_OPTIMIZER substrate + E5 training provenance (Lane 21, 2026-10-01).
# Transactional generation store over plain byte buffers (src/train/tg_store.*),
# SGD as the first updater (src/train/tg_sgd.*). No tensor/autodiff dependency
# (M20/M21 integration waits). New files only: src/train/, tests/train/,
# docs/train/, evidence/M22/. Picked up by `-include mk/*.mk`; not part of
# `all` or `test`. Host only, single core, no GPU, no quiet flag.
#
# test-train      purity check, then the exit tests plain and with ASan/UBSan;
#                 both builds must print the same replay digest
# train-purity    src/train includes only libc, sha256.h and src/train headers
# train-receipt   digest-named receipt under evidence/M22/receipts (clean tree only)
#
# The receipt says M22 NOT QUALIFIED: this is the substrate + SGD path only.
ifndef TRAIN_MK
TRAIN_MK := 1
.PHONY: test-train train-purity train-receipt
TRAIN_DIR = $(OUT_DIR)/train
TRAIN_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -ffp-contract=off -Isrc
TRAIN_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
TRAIN_SRCS = src/train/tg_store.c src/train/tg_sgd.c src/sha256.c
TRAIN_HDRS = src/train/tg_store.h src/train/tg_sgd.h src/sha256.h

$(TRAIN_DIR)/test_train: tests/train/test_train.c $(TRAIN_SRCS) $(TRAIN_HDRS)
	@mkdir -p $(TRAIN_DIR)
	$(CC) $(TRAIN_CFLAGS) -o $@ tests/train/test_train.c $(TRAIN_SRCS) -lm

$(TRAIN_DIR)/test_train_asan: tests/train/test_train.c $(TRAIN_SRCS) $(TRAIN_HDRS)
	@mkdir -p $(TRAIN_DIR)
	$(CC) $(TRAIN_CFLAGS) $(TRAIN_ASAN) -o $@ tests/train/test_train.c $(TRAIN_SRCS) -lm

train-purity:
	@bad=$$(grep -hE '^[[:space:]]*#[[:space:]]*include' src/train/*.c src/train/*.h | \
		grep -vE '#[[:space:]]*include[[:space:]]+(<[a-z/]+\.h>|"sha256\.h"|"train/tg_[a-z]+\.h")' || true); \
	if [ -n "$$bad" ]; then echo "train-purity: FAIL, unexpected includes:"; echo "$$bad"; exit 1; fi; \
	echo "train-purity: src/train depends only on libc, sha256 and itself"

test-train: train-purity $(TRAIN_DIR)/test_train $(TRAIN_DIR)/test_train_asan
	./$(TRAIN_DIR)/test_train $(TRAIN_DIR)/scratch > $(TRAIN_DIR)/plain.log 2>&1; rc=$$?; \
		grep -vE '^crash |^cost |^sgd_' $(TRAIN_DIR)/plain.log; [ $$rc -eq 0 ]
	./$(TRAIN_DIR)/test_train_asan $(TRAIN_DIR)/scratch-asan > $(TRAIN_DIR)/asan.log 2>&1; rc=$$?; \
		grep -vE '^crash |^cost |^sgd_' $(TRAIN_DIR)/asan.log; [ $$rc -eq 0 ]
	@a=$$(awk '$$1=="replay_digest"{print $$2}' $(TRAIN_DIR)/plain.log); \
	 b=$$(awk '$$1=="replay_digest"{print $$2}' $(TRAIN_DIR)/asan.log); \
	 if [ -z "$$a" ] || [ "$$a" != "$$b" ]; then echo "test-train: replay digest differs between builds"; exit 1; fi; \
	 echo "test-train: plain and ASan/UBSan PASS, replay digest equal across builds"

train-receipt:
	bash tests/train/train_receipt.sh
endif
