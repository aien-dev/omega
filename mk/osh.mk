# osh shell core (aien-architecture#158). Picked up by `-include mk/*.mk`; not part of `all` or `test`.
# src/osh/osh_lex.osc and osh_parse.osc are generated from the .osc.in templates and osh_layout.lst by osh_gen.sh
# (checked in with their output); the drivers tests/osh/test_osh_lex.c and test_osh_parse.c run them in the OSC
# interpreter and as native AArch64.
#   make osh-gen          regenerate src/osh/osh_lex.osc, osh_parse.osc and osh_layout.h
#   make osh-gen-check    regenerating gives byte-identical files
#   make test-osh-lex     check + lexer driver, plain and ASan/UBSan (exact-size buffers); OSH_FUZZ=N (default 100000)
#   make test-osh-parse   check + parser driver (lexer + parser units, reference tokenizer + parser), plain and ASan/UBSan
ifndef OSH_MK
OSH_MK := 1
.PHONY: osh-gen osh-gen-check test-osh-lex test-osh-parse
OSH_DIR = $(OUT_DIR)/osh
OSH_FUZZ ?= 100000
OSH_GEN = src/osh/osh_gen.sh
OSH_GEN_ARGS = src/osh/osh_layout.lst src/osh/osh_lex.osc.in src/osh/osh_lex.osc src/osh/osh_layout.h
OSH_GEN_ARGS_P = src/osh/osh_layout.lst src/osh/osh_parse.osc.in src/osh/osh_parse.osc src/osh/osh_layout.h
OSH_SRCS = tests/osh/test_osh_lex.c src/osh/osh_lex_ref.c $(OSC_LIB)
OSH_PSRCS = tests/osh/test_osh_parse.c src/osh/osh_lex_ref.c src/osh/osh_parse_ref.c $(OSC_LIB)
OSH_HDRS = src/osh/osh_layout.h src/osh/osh_lex_ref.h src/osh/osh_parse_ref.h $(OSC_HDRS)
OSH_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc -Isrc/compiler -Isrc/osh

osh-gen:
	sh $(OSH_GEN) gen $(OSH_GEN_ARGS)
	sh $(OSH_GEN) gen $(OSH_GEN_ARGS_P)

osh-gen-check:
	sh $(OSH_GEN) check $(OSH_GEN_ARGS)
	sh $(OSH_GEN) check $(OSH_GEN_ARGS_P)

$(OSH_DIR)/test_osh_lex: $(OSH_SRCS) $(OSH_HDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_FLAGS) -O2 -o $@ $(OSH_SRCS)

$(OSH_DIR)/test_osh_lex_asan: $(OSH_SRCS) $(OSH_HDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_FLAGS) $(OSC_ASAN) -o $@ $(OSH_SRCS)

$(OSH_DIR)/test_osh_parse: $(OSH_PSRCS) $(OSH_HDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_FLAGS) -O2 -o $@ $(OSH_PSRCS)

$(OSH_DIR)/test_osh_parse_asan: $(OSH_PSRCS) $(OSH_HDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_FLAGS) $(OSC_ASAN) -o $@ $(OSH_PSRCS)

test-osh-lex: osh-gen-check $(OSH_DIR)/test_osh_lex $(OSH_DIR)/test_osh_lex_asan
	$(abspath $(OSH_DIR)/test_osh_lex) src/osh/osh_lex.osc tests/osh/vectors $(OSH_FUZZ) > $(OSH_DIR)/lex.out
	@tail -12 $(OSH_DIR)/lex.out; tail -1 $(OSH_DIR)/lex.out | grep -q '^OSH_LEX_PASS$$'
	$(abspath $(OSH_DIR)/test_osh_lex_asan) src/osh/osh_lex.osc tests/osh/vectors $(OSH_FUZZ) > $(OSH_DIR)/lex_asan.out
	@tail -3 $(OSH_DIR)/lex_asan.out; tail -1 $(OSH_DIR)/lex_asan.out | grep -q '^OSH_LEX_PASS$$'
	@echo "test-osh-lex: PASS (lexer unit only; interp + native + C reference + chunked + vectors + fuzz; plain and ASan/UBSan)"

test-osh-parse: osh-gen-check $(OSH_DIR)/test_osh_parse $(OSH_DIR)/test_osh_parse_asan
	$(abspath $(OSH_DIR)/test_osh_parse) src/osh/osh_lex.osc src/osh/osh_parse.osc tests/osh/vectors tests/osh/parse_vectors $(OSH_FUZZ) > $(OSH_DIR)/parse.out
	@tail -14 $(OSH_DIR)/parse.out; tail -1 $(OSH_DIR)/parse.out | grep -q '^OSH_PARSE_PASS$$'
	$(abspath $(OSH_DIR)/test_osh_parse_asan) src/osh/osh_lex.osc src/osh/osh_parse.osc tests/osh/vectors tests/osh/parse_vectors $(OSH_FUZZ) > $(OSH_DIR)/parse_asan.out
	@tail -3 $(OSH_DIR)/parse_asan.out; tail -1 $(OSH_DIR)/parse_asan.out | grep -q '^OSH_PARSE_PASS$$'
	@echo "test-osh-parse: PASS (lexer + parser units; interp + native + C reference lexer and parser + chunked + vectors + fuzz; plain and ASan/UBSan)"
endif
