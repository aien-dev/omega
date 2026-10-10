# PD-1 development run (Physics-0 Direction 7, aien-dev/physics docs/PD1_MACHINE_LAW_BENCHMARK.md):
# the UNCHANGED PD-0 learner on the label-free PD1REC1 machine record stream.
# Not in all/test. Development evidence only (spec PD1 section 4.2); no discovery claim.
#
# physics0-pd1-run RECORDS=<pd1-records.bin> [SEED=1]   build + run; prints the receipt
# physics0-pd1-receipt [NOTE="..."]       same, and writes evidence/physics0/pd1/PD1_RESULT-dev-<commit>.txt (refuses overwrite, refuses dirty tree)
ifndef PHYSICS0_PD1_MK
PHYSICS0_PD1_MK := 1
include mk/physics0-learner.mk
.PHONY: physics0-pd1-run physics0-pd1-receipt
PD1_DIR = $(OUT_DIR)/physics0-pd1
PD1_SEED ?= 1
PD1_RECORDS ?= $(RECORDS)
$(PD1_DIR)/pd1-run: tests/physics0/pd1/pd1_run.c $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c $(P0L_HDRS)
	@mkdir -p $(PD1_DIR)
	$(CC) $(P0L_CFLAGS) -o $@ tests/physics0/pd1/pd1_run.c $(P0L_LEARNER_SRCS) $(P0L_VERIFIER_SRCS) src/sha256.c -lm
physics0-pd1-run: $(PD1_DIR)/pd1-run
	@test -n "$(PD1_RECORDS)" || { echo "set RECORDS=<pd1-records.bin> (tools/physics0/pd1-convert: make run OUT=...)"; exit 2; }
	@mkdir -p $(PD1_DIR)/out
	$(PD1_DIR)/pd1-run "$(PD1_RECORDS)" $(PD1_DIR)/out $(PD1_SEED) | tee $(PD1_DIR)/pd1-run.out
physics0-pd1-receipt: $(PD1_DIR)/pd1-run
	@test -n "$(PD1_RECORDS)" || { echo "set RECORDS=<pd1-records.bin>"; exit 2; }
	@test -z "$$(git status --porcelain)" || { echo "refusing: working tree dirty"; exit 2; }
	@mkdir -p $(PD1_DIR)/out evidence/physics0/pd1
	@c=$$(git rev-parse --short=12 HEAD); f=evidence/physics0/pd1/PD1_RESULT-dev-$$c.txt; test ! -e $$f || { echo "refusing: $$f exists"; exit 2; }; \
	{ echo "receipt_kind: PD1_RESULT development run (spec section 8 recommendation; not the blind run)"; echo "commit: $$c"; test -z "$(NOTE)" || echo "note: $(NOTE)"; echo "records_sha256: $$(sha256sum "$(PD1_RECORDS)" | cut -c1-64)"; echo "cc: $$($(CC) --version | head -1)"; echo "date_utc: $$(date -u +%Y-%m-%dT%H:%M:%SZ)"; \
	  $(PD1_DIR)/pd1-run "$(PD1_RECORDS)" $(PD1_DIR)/out $(PD1_SEED); echo "exit: $$?"; } > $$f; tail -1 $$f; echo "wrote $$f"
endif
