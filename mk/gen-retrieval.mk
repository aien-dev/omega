# tools/gen/retrieval (GEN-4, aien-spark/omega#479): genret, the file-backed retrieval provider of the GEN
# experiment (contract tools/gen/retrieval/CONTRACT.md, genret-cli-v3). Picked up by `-include mk/*.mk`; not
# part of `all` or `test`. CPU only, no model, no GPU, no network.
#
# gen-retrieval        build/gen_retrieval/genret, build/gen_retrieval/recheck and recheck.sha256 (the value
#                      callers pass as --recheck-sha256)
# test-gen-retrieval   tools/gen/retrieval/test_genret.sh twice: on the -O2 build, then on an ASan and UBSan
#                      build (leak report off: a one-shot tool exits holding its buffers).
#
# genret validates records with Agent D's recheck (tools/gen/abstract/recheck.c). Its source must hash to
# GENRET_RECHECK_SHA256; that is checked every time make runs a gen-retrieval target, whether the source is in
# this tree or taken from D's frozen schema v1 commit. The same value is compiled into genret and written to
# every receipt (recheck_source_sha256).
ifndef GEN_RETRIEVAL_MK
GEN_RETRIEVAL_MK := 1
.PHONY: gen-retrieval test-gen-retrieval gen-retrieval-recheck-source
GENRET_DIR = $(OUT_DIR)/gen_retrieval
GENRET_RECHECK_COMMIT = 937b033c78d087f1a0c64f28a29f4bab69f0a18a
GENRET_RECHECK_SHA256 = 05cec285f7b7b2be68b1db1287661513e7e4715da44909065bdd0829daac02b6
GENRET_CFLAGS = -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc \
  -DGENRET_RECHECK_SOURCE_SHA256='"$(GENRET_RECHECK_SHA256)"'
GENRET_SAN = -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
GENRET_SRCS = tools/gen/retrieval/genret.c src/sha256.c src/sha256.h mk/gen-retrieval.mk

ifneq ($(wildcard tools/gen/abstract/recheck.c),)
GENRET_RECHECK_SRC = tools/gen/abstract/recheck.c
else
# named by its pinned digest, so a changed pin extracts a fresh copy
GENRET_RECHECK_SRC = $(GENRET_DIR)/recheck_$(GENRET_RECHECK_SHA256).c
$(GENRET_RECHECK_SRC):
	@mkdir -p $(GENRET_DIR)
	@git show $(GENRET_RECHECK_COMMIT):tools/gen/abstract/recheck.c > $@.tmp || \
	  { rm -f $@.tmp; echo "gen-retrieval: commit $(GENRET_RECHECK_COMMIT) missing: git fetch origin gen/abstract"; exit 1; }
	@echo "$(GENRET_RECHECK_SHA256)  $@.tmp" | sha256sum -c --quiet || { rm -f $@.tmp; exit 1; }
	@mv $@.tmp $@
endif

# runs on every invocation of the targets below, not only when a file is first extracted
gen-retrieval-recheck-source: $(GENRET_RECHECK_SRC)
	@echo "$(GENRET_RECHECK_SHA256)  $(GENRET_RECHECK_SRC)" | sha256sum -c --quiet || \
	  { echo "gen-retrieval: $(GENRET_RECHECK_SRC) does not match GENRET_RECHECK_SHA256"; exit 1; }

$(GENRET_DIR)/genret: $(GENRET_SRCS)
	@mkdir -p $(GENRET_DIR)
	$(CC) $(GENRET_CFLAGS) -O2 -o $@ tools/gen/retrieval/genret.c src/sha256.c

$(GENRET_DIR)/genret_asan: $(GENRET_SRCS)
	@mkdir -p $(GENRET_DIR)
	$(CC) $(GENRET_CFLAGS) $(GENRET_SAN) -o $@ tools/gen/retrieval/genret.c src/sha256.c

$(GENRET_DIR)/recheck: $(GENRET_RECHECK_SRC) src/sha256.c src/sha256.h mk/gen-retrieval.mk
	@mkdir -p $(GENRET_DIR)
	@echo "$(GENRET_RECHECK_SHA256)  $(GENRET_RECHECK_SRC)" | sha256sum -c --quiet
	$(CC) $(GENRET_CFLAGS) -O2 -o $@ $(GENRET_RECHECK_SRC) src/sha256.c
	sha256sum $@ | cut -c1-64 > $@.sha256

gen-retrieval: gen-retrieval-recheck-source $(GENRET_DIR)/genret $(GENRET_DIR)/recheck

test-gen-retrieval: gen-retrieval-recheck-source $(GENRET_DIR)/genret $(GENRET_DIR)/genret_asan $(GENRET_DIR)/recheck
	sh tools/gen/retrieval/test_genret.sh $(abspath $(GENRET_DIR)/genret) $(abspath $(GENRET_DIR)/recheck) \
	  $(abspath $(GENRET_RECHECK_SRC))
	ASAN_OPTIONS=detect_leaks=0 sh tools/gen/retrieval/test_genret.sh $(abspath $(GENRET_DIR)/genret_asan) \
	  $(abspath $(GENRET_DIR)/recheck) $(abspath $(GENRET_RECHECK_SRC))
endif
