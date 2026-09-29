# Omega Visor V1 lane 1 (semantic query API) test target.
.PHONY: test-visor-semantic
test-visor-semantic: $(OUT_DIR)/tests-visor/test_visor_semantic
	./$(OUT_DIR)/tests-visor/test_visor_semantic
