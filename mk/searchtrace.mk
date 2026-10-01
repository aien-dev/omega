# M23 G1 search-trace corpus + G3 sealed-holdout commitment format (lane 7).
# New files only: src/searchtrace/*, tests/searchtrace/*, spec/searchtrace/*. Picked up by
# `-include mk/*.mk`; no Makefile edit; redefines nothing that exists. Links only the
# learner-side Omega core (no physics, no gate suites, no runtime).
#   make searchtrace        build/searchtrace/searchtrace
#   make test-searchtrace   unit + refusal tests (plain and ASan/UBSan) and hookcheck on the frozen set
ifndef SEARCHTRACE_MK
SEARCHTRACE_MK := 1
.PHONY: searchtrace test-searchtrace
ST_DIR = $(OUT_DIR)/searchtrace
ST_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc
ST_ASAN = -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
ST_CORE = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_program.c src/omega_synthesis.c \
	src/omega_library.c
ST_LIB = src/searchtrace/st_corpus.c src/searchtrace/st_holdout.c
ST_HDRS = $(wildcard src/searchtrace/*.h) src/omega_synthesis.h src/omega_realize_synth.h src/omega_program.h

$(ST_DIR)/searchtrace: src/searchtrace/st_tool.c $(ST_LIB) $(ST_CORE) $(ST_HDRS)
	@mkdir -p $(ST_DIR)
	$(CC) $(ST_CFLAGS) -o $@ src/searchtrace/st_tool.c $(ST_LIB) $(ST_CORE)

$(ST_DIR)/test_st_corpus: tests/searchtrace/test_st_corpus.c $(ST_LIB) $(ST_CORE) $(ST_HDRS)
	@mkdir -p $(ST_DIR)
	$(CC) $(ST_CFLAGS) -o $@ tests/searchtrace/test_st_corpus.c $(ST_LIB) $(ST_CORE)

$(ST_DIR)/test_st_corpus_asan: tests/searchtrace/test_st_corpus.c $(ST_LIB) $(ST_CORE) $(ST_HDRS)
	@mkdir -p $(ST_DIR)
	$(CC) $(ST_CFLAGS) $(ST_ASAN) -o $@ tests/searchtrace/test_st_corpus.c $(ST_LIB) $(ST_CORE)

$(ST_DIR)/test_st_holdout: tests/searchtrace/test_st_holdout.c src/searchtrace/st_holdout.c src/searchtrace/st_holdout.h src/sha256.c
	@mkdir -p $(ST_DIR)
	$(CC) $(ST_CFLAGS) -o $@ tests/searchtrace/test_st_holdout.c src/searchtrace/st_holdout.c src/sha256.c

$(ST_DIR)/test_st_holdout_asan: tests/searchtrace/test_st_holdout.c src/searchtrace/st_holdout.c src/searchtrace/st_holdout.h src/sha256.c
	@mkdir -p $(ST_DIR)
	$(CC) $(ST_CFLAGS) $(ST_ASAN) -o $@ tests/searchtrace/test_st_holdout.c src/searchtrace/st_holdout.c src/sha256.c

searchtrace: $(ST_DIR)/searchtrace

test-searchtrace: $(ST_DIR)/searchtrace $(ST_DIR)/test_st_corpus $(ST_DIR)/test_st_corpus_asan $(ST_DIR)/test_st_holdout $(ST_DIR)/test_st_holdout_asan
	./$(ST_DIR)/test_st_corpus
	./$(ST_DIR)/test_st_corpus_asan
	./$(ST_DIR)/test_st_holdout
	./$(ST_DIR)/test_st_holdout_asan
	./$(ST_DIR)/searchtrace hookcheck tests/searchtrace/frozen_taskset_v1.txt
	rm -f $(ST_DIR)/corpus-a.txt
	./$(ST_DIR)/searchtrace run tests/searchtrace/frozen_taskset_v1.txt $(ST_DIR)/corpus-a.txt
	./$(ST_DIR)/searchtrace replay tests/searchtrace/frozen_taskset_v1.txt $(ST_DIR)/corpus-a.txt
	@echo "test-searchtrace: corpus determinism, refusals, hook equivalence and G3 holdout format pass"
endif
