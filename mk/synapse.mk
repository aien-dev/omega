# Synapse binary codec v2 in Omega (aien-dev/synapse ADR 0003 section 9 step 33, J1). Picked up by
# `-include mk/*.mk`; not part of `all` or `test`; host only (interpreter + native AArch64, no GPU).
# src/synapse/syn_codec.osc and syn_layout.h are generated from syn_codec.osc.in and syn_layout.lst by
# syn_gen.sh (checked in with their output). tests/synapse/j1-corpus/corpus.bin is the corpus the Rust codec
# sealed (provenance in tests/synapse/j1-corpus/PROVENANCE).
#   make syn-gen              regenerate src/synapse/syn_codec.osc and syn_layout.h
#   make syn-gen-check        regenerating gives byte-identical files
#   make test-synapse-codec   check + cross-check driver, plain and ASan/UBSan (exact-size buffers);
#                             SYN_FUZZ=N (default 20000), SYN_FUZZ_SEED=S
ifndef SYNAPSE_MK
SYNAPSE_MK := 1
.PHONY: syn-gen syn-gen-check test-synapse-codec
SYN_DIR = $(OUT_DIR)/synapse
SYN_FUZZ ?= 20000
SYN_GEN = src/synapse/syn_gen.sh
SYN_GEN_ARGS = src/synapse/syn_layout.lst src/synapse/syn_codec.osc.in src/synapse/syn_codec.osc src/synapse/syn_layout.h
SYN_SRCS = tests/synapse/test_syn_codec.c $(OSC_LIB)
SYN_HDRS = src/synapse/syn_layout.h src/sha256.h $(OSC_HDRS)
SYN_FLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc -Isrc/compiler -Isrc/synapse
SYN_CORPUS = tests/synapse/j1-corpus/corpus.bin

syn-gen:
	sh $(SYN_GEN) gen $(SYN_GEN_ARGS)

syn-gen-check:
	sh $(SYN_GEN) check $(SYN_GEN_ARGS)

$(SYN_DIR)/test_syn_codec: $(SYN_SRCS) $(SYN_HDRS)
	@mkdir -p $(SYN_DIR)
	$(CC) $(SYN_FLAGS) -O2 -o $@ $(SYN_SRCS)

$(SYN_DIR)/test_syn_codec_asan: $(SYN_SRCS) $(SYN_HDRS)
	@mkdir -p $(SYN_DIR)
	$(CC) $(SYN_FLAGS) $(OSC_ASAN) -o $@ $(SYN_SRCS)

test-synapse-codec: syn-gen-check $(SYN_DIR)/test_syn_codec $(SYN_DIR)/test_syn_codec_asan
	$(abspath $(SYN_DIR)/test_syn_codec) src/synapse/syn_codec.osc $(SYN_CORPUS) $(SYN_FUZZ) > $(SYN_DIR)/codec.out
	@tail -12 $(SYN_DIR)/codec.out; tail -1 $(SYN_DIR)/codec.out | grep -q '^SYN_J1_PASS$$'
	$(abspath $(SYN_DIR)/test_syn_codec_asan) src/synapse/syn_codec.osc $(SYN_CORPUS) $(SYN_FUZZ) > $(SYN_DIR)/codec_asan.out
	@tail -3 $(SYN_DIR)/codec_asan.out; tail -1 $(SYN_DIR)/codec_asan.out | grep -q '^SYN_J1_PASS$$'
	@echo "test-synapse-codec: PASS (Rust corpus both directions + A1 malformed corpus + encode refusals + fuzz; interp + native; plain and ASan/UBSan)"
endif
