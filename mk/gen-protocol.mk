# GEN-1: pre-registered generalization protocol tools (aien-spark/omega#476, docs/one_layer/GEN_PROTOCOL_V1.md).
# Picked up by `-include mk/*.mk`; not part of `all` or `test`. CPU only.
#
# gen-protocol-tools   build/gen_protocol/{famsig,gencheck,genstats,genjac}
# test-gen-protocol    genstats selftest, genjac known answers + byte-identical stats_vectors.tsv, famsig known answers and mutants,
#                      gencheck on tests/one_layer/generalization/protocol.json and its mutants (manifest,
#                      tree, synthetic held-out split), dev pack reproducibility + oracle negative control.
#                      GEN_TASKS_CHECK=1 also runs ola.sh tasks-check on the dev pack (clean tree needed).
ifndef GEN_PROTOCOL_MK
GEN_PROTOCOL_MK := 1
.PHONY: gen-protocol-tools test-gen-protocol
GENP_SRC = tools/gen/protocol
GENP_DIR = $(OUT_DIR)/gen_protocol
GENP_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -O2 -Isrc

$(GENP_DIR)/famsig: $(GENP_SRC)/famsig.c $(GENP_SRC)/famsig_lib.h src/sha256.c src/sha256.h
	@mkdir -p $(GENP_DIR)
	$(CC) $(GENP_CFLAGS) -o $@ $(GENP_SRC)/famsig.c src/sha256.c

$(GENP_DIR)/gencheck: $(GENP_SRC)/gencheck.c $(GENP_SRC)/famsig_lib.h src/sha256.c src/sha256.h
	@mkdir -p $(GENP_DIR)
	$(CC) $(GENP_CFLAGS) -o $@ $(GENP_SRC)/gencheck.c src/sha256.c

$(GENP_DIR)/genstats: $(GENP_SRC)/genstats.c
	@mkdir -p $(GENP_DIR)
	$(CC) $(GENP_CFLAGS) -o $@ $(GENP_SRC)/genstats.c -lm

$(GENP_DIR)/genjac: $(GENP_SRC)/genjac.c
	@mkdir -p $(GENP_DIR)
	$(CC) $(GENP_CFLAGS) -o $@ $(GENP_SRC)/genjac.c

gen-protocol-tools: $(GENP_DIR)/famsig $(GENP_DIR)/gencheck $(GENP_DIR)/genstats $(GENP_DIR)/genjac

test-gen-protocol:
	CC="$(CC)" bash $(GENP_SRC)/test_gen_protocol.sh $(abspath $(GENP_DIR))/test
endif
