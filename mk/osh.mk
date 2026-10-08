# osh shell core (aien-architecture#158). Picked up by `-include mk/*.mk`; not part of `all` or `test`.
# src/osh/osh_lex.osc and osh_parse.osc are generated from the .osc.in templates and osh_layout.lst by osh_gen.sh
# (checked in with their output); the drivers tests/osh/test_osh_lex.c and test_osh_parse.c run them in the OSC
# interpreter and as native AArch64.
#   make osh-gen          regenerate src/osh/osh_lex.osc, osh_parse.osc and osh_layout.h
#   make osh-gen-check    regenerating gives byte-identical files
#   make test-osh-lex     check + lexer driver, plain and ASan/UBSan (exact-size buffers); OSH_FUZZ=N (default 100000), OSH_FUZZ_SEED=S
#   make osh-bash-verify  prove tests/osh/vectors/bash_vectors.tsv against real bash 5.2 (needs bash, not part of test-osh-lex)
#   make test-osh-parse   check + parser driver (lexer + parser units, reference tokenizer + parser), plain and ASan/UBSan
ifndef OSH_MK
OSH_MK := 1
.PHONY: osh-gen osh-gen-check test-osh-lex test-osh-parse test-osh-expand osh test-osh-e2e osh-bash-verify
OSH_DIR = $(OUT_DIR)/osh
OSH_FUZZ ?= 100000
OSH_GEN = src/osh/osh_gen.sh
OSH_GEN_ARGS = src/osh/osh_layout.lst src/osh/osh_lex.osc.in src/osh/osh_lex.osc src/osh/osh_layout.h
OSH_GEN_ARGS_P = src/osh/osh_layout.lst src/osh/osh_parse.osc.in src/osh/osh_parse.osc src/osh/osh_layout.h
OSH_GEN_ARGS_X = src/osh/osh_layout.lst src/osh/osh_expand.osc.in src/osh/osh_expand.osc src/osh/osh_layout.h
OSH_SRCS = tests/osh/test_osh_lex.c src/osh/osh_lex_ref.c $(OSC_LIB)
OSH_PSRCS = tests/osh/test_osh_parse.c src/osh/osh_lex_ref.c src/osh/osh_parse_ref.c $(OSC_LIB)
OSH_HDRS = src/osh/osh_layout.h src/osh/osh_lex_ref.h src/osh/osh_parse_ref.h $(OSC_HDRS)
OSH_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc -Isrc/compiler -Isrc/osh

osh-gen:
	sh $(OSH_GEN) gen $(OSH_GEN_ARGS)
	sh $(OSH_GEN) gen $(OSH_GEN_ARGS_P)
	sh $(OSH_GEN) gen $(OSH_GEN_ARGS_X)

osh-bash-verify:
	bash tests/osh/vectors/bash_verify.sh

osh-gen-check:
	sh $(OSH_GEN) check $(OSH_GEN_ARGS)
	sh $(OSH_GEN) check $(OSH_GEN_ARGS_P)
	sh $(OSH_GEN) check $(OSH_GEN_ARGS_X)

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

OSH_XSRCS = tests/osh/test_osh_expand.c src/osh/osh_lex_ref.c src/osh/osh_parse_ref.c src/osh/osh_expand_ref.c src/osh/host/osh_core.c src/osh/host/osh_codes.c src/osh/host/osh_req.c $(OSC_LIB)
OSH_XHDRS = $(OSH_HDRS) src/osh/osh_expand_ref.h src/osh/host/osh_core.h src/osh/host/osh_host.h
OSH_XFLAGS = $(OSH_FLAGS) -Isrc/osh/host -Isrc/runtime

$(OSH_DIR)/test_osh_expand: $(OSH_XSRCS) $(OSH_XHDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_XFLAGS) -O2 -o $@ $(OSH_XSRCS)

$(OSH_DIR)/test_osh_expand_asan: $(OSH_XSRCS) $(OSH_XHDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_XFLAGS) $(OSC_ASAN) -o $@ $(OSH_XSRCS)

test-osh-expand: osh-gen-check $(OSH_DIR)/test_osh_expand $(OSH_DIR)/test_osh_expand_asan
	$(abspath $(OSH_DIR)/test_osh_expand) src/osh/osh_lex.osc src/osh/osh_parse.osc src/osh/osh_expand.osc tests/osh/vectors $(OSH_FUZZ) > $(OSH_DIR)/expand.out
	@tail -16 $(OSH_DIR)/expand.out; tail -1 $(OSH_DIR)/expand.out | grep -q '^OSH_EXPAND_PASS$$'
	$(abspath $(OSH_DIR)/test_osh_expand_asan) src/osh/osh_lex.osc src/osh/osh_parse.osc src/osh/osh_expand.osc tests/osh/vectors $(OSH_FUZZ) > $(OSH_DIR)/expand_asan.out
	@tail -3 $(OSH_DIR)/expand_asan.out; tail -1 $(OSH_DIR)/expand_asan.out | grep -q '^OSH_EXPAND_PASS$$'
	@echo "test-osh-expand: PASS"

OSH_UNIT_SRCS = src/osh/osh_lex.osc src/osh/osh_parse.osc src/osh/osh_expand.osc
OSH_PROG_HOSTSRCS = $(wildcard src/osh/host/osh_*.c)
OSH_BIN_SRCS = $(OSH_DIR)/osh_units.c $(filter-out src/osh/host/osh_codes.c,$(OSH_PROG_HOSTSRCS)) src/osh/host/osh_codes.c $(OSC_LIB) src/runtime/rx_caproot.c

# the three unit sources become C arrays (od + sed, no scripting language)
$(OSH_DIR)/osh_units.c: $(OSH_UNIT_SRCS)
	@mkdir -p $(OSH_DIR)
	{ echo "#include <stddef.h>"; \
	  for u in lex parse expand; do \
	    echo "const unsigned char osh_unit_$$u[] = {"; \
	    od -An -v -tu1 src/osh/osh_$$u.osc | sed 's/^ *//; s/  */,/g; s/$$/,/'; \
	    echo "0};"; \
	    echo "const size_t osh_unit_$${u}_len = sizeof osh_unit_$$u - 1;"; \
	  done; } > $@

$(OSH_DIR)/osh: $(OSH_BIN_SRCS) $(OSH_XHDRS) src/osh/host/osh_shell.h src/osh/host/osh_priv.h
	$(CC) $(OSH_XFLAGS) -O2 -o $@ $(OSH_BIN_SRCS)

osh: $(OSH_DIR)/osh

$(OSH_DIR)/argprint: tests/osh/e2e/argprint.c
	@mkdir -p $(OSH_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -O2 -o $@ $<

$(OSH_DIR)/test_osh_e2e: tests/osh/e2e/test_osh_e2e.c
	@mkdir -p $(OSH_DIR)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -o $@ $<

test-osh-e2e: osh-gen-check $(OSH_DIR)/osh $(OSH_DIR)/argprint $(OSH_DIR)/test_osh_e2e
	$(abspath $(OSH_DIR)/test_osh_e2e) $(abspath $(OSH_DIR)/osh) $(abspath $(OSH_DIR)/argprint) > $(OSH_DIR)/e2e.out
	@tail -3 $(OSH_DIR)/e2e.out; tail -1 $(OSH_DIR)/e2e.out | grep -q '^OSH_E2E_PASS$$'
	@echo "test-osh-e2e: PASS"

# Core-only step trace (OSH-AIENOS-0): the lexer, parser and expander run over fixture scripts through osh_core_call, nothing
# is executed. `make osh-trace` runs every tests/osh/trace/*.sh native AND interpreted, requires the two traces to be
# identical, and requires them to equal the committed .trace file. `make osh-trace-gen` rewrites the .trace files.
# Optional per fixture: tNN.status (fake pipeline statuses) and tNN.args (positionals); see tests/osh/osh_trace.c.
.PHONY: osh-trace osh-trace-gen
OSH_TRSRCS = tests/osh/osh_trace.c src/osh/host/osh_core.c src/osh/host/osh_codes.c $(OSC_LIB)

$(OSH_DIR)/osh_trace: $(OSH_TRSRCS) $(OSH_XHDRS)
	@mkdir -p $(OSH_DIR)
	$(CC) $(OSH_XFLAGS) -O2 -o $@ $(OSH_TRSRCS)

# Each trace must end with "event exit=" (the driver prints it last): an empty or cut-off trace never passes and is
# never written. osh-trace-gen writes all traces to a scratch directory first and copies only when every script passed.
osh-trace-gen: osh-gen-check $(OSH_DIR)/osh_trace
	@g=$(OSH_DIR)/trace-gen; rm -rf $$g; mkdir -p $$g; \
	for f in tests/osh/trace/*.sh; do b=$$(basename $$f .sh); \
	  $(OSH_DIR)/osh_trace interp $(OSH_UNIT_SRCS) $$f > $$g/$$b.interp || exit 1; \
	  $(OSH_DIR)/osh_trace native $(OSH_UNIT_SRCS) $$f > $$g/$$b.native || exit 1; \
	  cmp $$g/$$b.interp $$g/$$b.native || { echo "osh-trace-gen: native and interpreter differ on $$f"; exit 1; }; \
	  tail -n 1 $$g/$$b.native | grep -q '^event exit=' || { echo "osh-trace-gen: empty or cut-off trace for $$f"; exit 1; }; \
	done; \
	for f in tests/osh/trace/*.sh; do b=$$(basename $$f .sh); cp $$g/$$b.native tests/osh/trace/$$b.trace || exit 1; done; \
	echo "osh-trace-gen: wrote traces"

osh-trace: osh-gen-check $(OSH_DIR)/osh_trace
	@n=0; for f in tests/osh/trace/*.sh; do b=$$(basename $$f .sh); \
	  $(OSH_DIR)/osh_trace interp $(OSH_UNIT_SRCS) $$f > $(OSH_DIR)/$$b.interp || exit 1; \
	  $(OSH_DIR)/osh_trace native $(OSH_UNIT_SRCS) $$f > $(OSH_DIR)/$$b.native || exit 1; \
	  cmp $(OSH_DIR)/$$b.interp $(OSH_DIR)/$$b.native || { echo "osh-trace: native and interpreter differ on $$f"; exit 1; }; \
	  tail -n 1 $(OSH_DIR)/$$b.native | grep -q '^event exit=' || { echo "osh-trace: empty or cut-off trace for $$f"; exit 1; }; \
	  cmp $(OSH_DIR)/$$b.native tests/osh/trace/$$b.trace || { echo "osh-trace: $$f differs from the committed trace"; exit 1; }; \
	  n=$$((n+1)); done; echo "osh-trace: PASS ($$n scripts, native == interpreter == committed trace)"
endif
