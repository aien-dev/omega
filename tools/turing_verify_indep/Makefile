# Lane D independent scorer (EXP-001). C only, no dependency outside libc (+libm for the selftest audit only).
CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror -pedantic -D_POSIX_C_SOURCE=200809L -Wno-error=pedantic -Wno-format-truncation
BUILD := build

all: $(BUILD)/indep-scorer $(BUILD)/indep-selftest

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/indep-scorer: src/main.c src/core.c src/sha256.c src/core.h src/sha256.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/main.c src/core.c src/sha256.c

$(BUILD)/indep-selftest: src/selftest.c src/core.c src/sha256.c src/core.h src/sha256.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/selftest.c src/core.c src/sha256.c -lm

test: $(BUILD)/indep-selftest
	cd $(BUILD) && ./indep-selftest ../../docs-only-v2 $(HOME)/aien-data/turing-cal/candidates

clean:
	rm -rf $(BUILD)

.PHONY: all test clean
