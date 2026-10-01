# M20 canonical machine identity (src/runtime/aien_machine_id.h). Host-only, no
# Physics or AIENOS sources needed. Run from the repo root.
AIEN_MID_TEST = $(OUT_DIR)/aien_machine_id_test

$(AIEN_MID_TEST): tests/runtime/aien_machine_id_test.c src/runtime/aien_machine_id.c \
		src/runtime/aien_machine_id.h src/sha256.c src/sha256.h
	mkdir -p $(dir $@)
	$(CC) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -o $@ \
		tests/runtime/aien_machine_id_test.c src/runtime/aien_machine_id.c src/sha256.c

.PHONY: test-machine-identity
test-machine-identity: $(AIEN_MID_TEST)
	$(abspath $(AIEN_MID_TEST))

# ARGUS adapter: with RX_ARGUS_MACHINE_ID set, ARGUS stamps the canonical id into
# its events (checked in the summary); a malformed one keeps ARGUS from starting.
# Needs the ARGUS/aienos sources (see ARGUS_REPO, AIENOS_LOCK_REPO); not in CI.
.PHONY: test-machine-identity-argus
test-machine-identity-argus: $(AIEN_MID_TEST) $(ARGUS_R7)
	@set -e; d=$$(mktemp -d); t=$$($(abspath $(AIEN_MID_TEST)) --kat-text); \
	RX_ARGUS_MACHINE_ID=$$t RX_ARGUS_CONSUMER=ingest RX_ARGUS_SUMMARY=$$d/ok.json ./$(ARGUS_R7) > $$d/ok.log 2>&1; \
	grep -q '"machine_id": "b3d2c32ee1a3ef5803e1cc604943dc8189fda2f5ea90bb5158eecc0acf5ff366"' $$d/ok.json; \
	RX_ARGUS_MACHINE_ID=$${t}0 RX_ARGUS_CONSUMER=ingest RX_ARGUS_SUMMARY=$$d/bad.json ./$(ARGUS_R7) > $$d/bad.log 2>&1 || true; \
	grep -q 'not a canonical machine identity' $$d/bad.log; test ! -e $$d/bad.json; \
	rm -rf $$d; echo M20_MACHINE_IDENTITY_ARGUS_PASS
