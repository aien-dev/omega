# AT-0 Agent 4 independent evaluator (research/atemporal/at0/evaluator).
# Standalone: not part of omega `all` or `test`; not mk/at0.mk (Agent 5 owns that).
# Links exactly one omega source: src/sha256.c (charter section 3).
OMEGA_ROOT ?= ../../../..
OUT ?= build
CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2 -D_POSIX_C_SOURCE=200809L
ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
SRCS = src/at0e_big.c src/at0e_text.c src/at0e_case.c src/at0e_shadow.c src/at0e_result.c src/at0e_synth.c src/at0e_main.c
INC = -Isrc -I$(OMEGA_ROOT)/src

.PHONY: all asan clean selftest
all: $(OUT)/at0-eval
$(OUT)/at0-eval: $(SRCS) src/at0e.h $(OMEGA_ROOT)/src/sha256.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) -o $@ $(SRCS) $(OMEGA_ROOT)/src/sha256.c -lm
asan: $(OUT)/at0-eval-asan
$(OUT)/at0-eval-asan: $(SRCS) src/at0e.h $(OMEGA_ROOT)/src/sha256.c
	@mkdir -p $(OUT)
	$(CC) -std=c11 -Wall -Wextra -Werror -pedantic $(ASAN) -D_POSIX_C_SOURCE=200809L $(INC) -o $@ $(SRCS) $(OMEGA_ROOT)/src/sha256.c -lm
selftest: all
	sh run.sh --self-only
clean:
	rm -rf $(OUT)
