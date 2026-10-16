# tools/gen/abstract (GEN-3, aien-spark/omega#478): GEN record schema v1 (RECORD_SCHEMA.md), the strict
# validator, the abstraction extractor and its corpus (tests/one_layer/generalization/corpus).
# Picked up by `-include mk/*.mk`; not part of `all` or `test`. CPU only, no model, no GPU.
#
# gen-abstract         build/gen_abstract/recheck
# test-gen-abstract    tools/gen/abstract/test_gen_abstract.sh: recheck fixtures and mutants
ifndef GEN_ABSTRACT_MK
GEN_ABSTRACT_MK := 1
.PHONY: gen-abstract test-gen-abstract
GENABS_DIR = $(OUT_DIR)/gen_abstract
GENABS_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc

$(GENABS_DIR)/recheck: tools/gen/abstract/recheck.c src/sha256.c src/sha256.h
	@mkdir -p $(GENABS_DIR)
	$(CC) $(GENABS_CFLAGS) -o $@ tools/gen/abstract/recheck.c src/sha256.c

gen-abstract: $(GENABS_DIR)/recheck

test-gen-abstract: gen-abstract
	bash tools/gen/abstract/test_gen_abstract.sh $(abspath $(OUT_DIR))
endif
