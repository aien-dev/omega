# Omega language V0 (lane 3) test target. Build+run from the repo root so the
# golden file path tests/language/golden/v0.txt resolves.
.PHONY: test-language
test-language: $(OUT_DIR)/tests-language/test_language_v0
	./$(OUT_DIR)/tests-language/test_language_v0
