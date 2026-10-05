# omegatool must not embed the omega checkout or physics path (CAND-2 prep). The physics
# location is resolved at run time by src/omega_physics_dir.h.
.PHONY: test-omegatool-path
test-omegatool-path:
	$(CC) -std=gnu11 -Wall -Wextra -Werror -D_GNU_SOURCE -O2 -Isrc -o $(OUT_DIR)/omegatool_path_test tests/omegatool_path/physics_dir_test.c
	$(OUT_DIR)/omegatool_path_test
	PHYSICS_DIR=$(PHYSICS_DIR) tests/omegatool_path/no_embedded_path_test.sh
