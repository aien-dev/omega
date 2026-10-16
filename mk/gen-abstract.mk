# tools/gen/abstract (GEN-3, aien-spark/omega#478): GEN record schema v1 (RECORD_SCHEMA.md), the strict
# validator, the abstraction extractor and its corpus (tests/one_layer/generalization/corpus).
# Picked up by `-include mk/*.mk`; not part of `all` or `test`. CPU only, no model, no GPU.
#
# gen-abstract         build/gen_abstract/{recheck,gabs,oracle,famsig}
# test-gen-abstract    tools/gen/abstract/test_gen_abstract.sh: recheck, corpus, extract, verify, probes, mutants
#
# famsig is GEN-1's tool (tools/gen/protocol, FAMSIG v1, aien-spark/omega#476). Until that branch is merged the
# rule builds it from the pinned git commit 7eb8534c (gen/protocol, FAMSIG v1 with R6/R7) (read with git show, nothing is copied into this tree).
ifndef GEN_ABSTRACT_MK
GEN_ABSTRACT_MK := 1
.PHONY: gen-abstract test-gen-abstract
GENABS_DIR = $(OUT_DIR)/gen_abstract
GENABS_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc
GENABS_FAMSIG_COMMIT = 7eb8534c42304505640013f64753d9c9db40dcf3

$(GENABS_DIR)/recheck: tools/gen/abstract/recheck.c src/sha256.c src/sha256.h
	@mkdir -p $(GENABS_DIR)
	$(CC) $(GENABS_CFLAGS) -o $@ tools/gen/abstract/recheck.c src/sha256.c

$(GENABS_DIR)/gabs: tools/gen/abstract/gabs.c
	@mkdir -p $(GENABS_DIR)
	$(CC) $(GENABS_CFLAGS) -o $@ tools/gen/abstract/gabs.c

$(GENABS_DIR)/oracle: tests/one_layer/generalization/corpus/oracle.c
	@mkdir -p $(GENABS_DIR)
	$(CC) $(GENABS_CFLAGS) -o $@ tests/one_layer/generalization/corpus/oracle.c

$(GENABS_DIR)/famsig: src/sha256.c src/sha256.h mk/gen-abstract.mk
	@mkdir -p $(GENABS_DIR)/famsig-src/tools/gen/protocol
	@if [ -f tools/gen/protocol/famsig.c ]; then \
	  cp tools/gen/protocol/famsig.c tools/gen/protocol/famsig_lib.h $(GENABS_DIR)/famsig-src/tools/gen/protocol/; \
	else \
	  git show $(GENABS_FAMSIG_COMMIT):tools/gen/protocol/famsig.c > $(GENABS_DIR)/famsig-src/tools/gen/protocol/famsig.c && \
	  git show $(GENABS_FAMSIG_COMMIT):tools/gen/protocol/famsig_lib.h > $(GENABS_DIR)/famsig-src/tools/gen/protocol/famsig_lib.h; \
	fi
	$(CC) -std=gnu11 -Wall -Wextra -Werror -O2 -Isrc -o $@ $(GENABS_DIR)/famsig-src/tools/gen/protocol/famsig.c src/sha256.c

gen-abstract: $(GENABS_DIR)/recheck $(GENABS_DIR)/gabs $(GENABS_DIR)/oracle $(GENABS_DIR)/famsig

test-gen-abstract: gen-abstract omegese omega-emit checker2 one-layer-acceptance-tools $(OUT_DIR)/compiler/oscc
	bash tools/gen/abstract/test_gen_abstract.sh $(abspath $(OUT_DIR))
endif
