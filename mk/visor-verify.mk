# Omega Visor V1 - lane 4 (verification + evidence explorer) test targets.
# The generic $(OUT_DIR)/tests-visor/% rule in mk/00-visor-core.mk builds the binaries.
.PHONY: test-visor-verify test-visor-evidence

test-visor-verify: $(OUT_DIR)/tests-visor/test_visor_verify
	./$(OUT_DIR)/tests-visor/test_visor_verify

test-visor-evidence: $(OUT_DIR)/tests-visor/test_visor_evidence
	./$(OUT_DIR)/tests-visor/test_visor_evidence
