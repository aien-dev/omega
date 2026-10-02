# Omega GPU Engine (docs/numeric/OMEGA_GPU_ENGINE.md). Picked up by
# `-include mk/*.mk`. Host only: opens no device; needs no physics headers except the two A3b1 targets, not
# part of `all` or `test`.
#   test-gpu-engine-compile  header + core compile, enum names, refusals, no-op backend (A2/A3a)
#   test-gpu-engine          the engine core against a fake backend (every state and failure)
#   test-gpu-engine-backend-compile  syntax-checks src/omega_blackwell_engine.c (A3b1): -c only, no link, no device
#   test-gpu-engine-prod-build       production build of build/omegatool at the physics pin: links only, opens no device
#   test-gpu-engine-mutants  removes each protection from src/omega_gpu_engine.c in a scratch
#                            copy; the host test must fail each one (exit nonzero on a survivor)
.PHONY: test-gpu-engine-compile test-gpu-engine test-gpu-engine-mutants test-gpu-engine-backend-compile test-gpu-engine-prod-build
build/test_omega_gpu_engine_compile: tests/test_omega_gpu_engine_compile.c src/omega_gpu_engine.c src/omega_gpu_engine.h
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc -DOMEGA_NUMERIC_CPU_ONLY -o $@ tests/test_omega_gpu_engine_compile.c src/omega_gpu_engine.c
test-gpu-engine-compile: build/test_omega_gpu_engine_compile
	./build/test_omega_gpu_engine_compile

build/test_omega_gpu_engine: tests/test_omega_gpu_engine.c src/omega_gpu_engine.c src/omega_gpu_engine.h
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -Isrc -DOMEGA_NUMERIC_CPU_ONLY -o $@ tests/test_omega_gpu_engine.c src/omega_gpu_engine.c
test-gpu-engine: build/test_omega_gpu_engine
	./build/test_omega_gpu_engine

test-gpu-engine-mutants:
	sh tools/gpu_engine_mutations.sh

# A3b1: the Blackwell backend includes physics headers (m16_native.h, nvrm.h), so it
# uses the same physics -I paths as the top-level Makefile CFLAGS (Makefile lines 9-14).
GPU_ENGINE_PHYS_INC = -I$(PHYSICS_DIR)/m16 -I$(PHYSICS_DIR)/nvrm \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/kernel-open/common/inc \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include

build/omega_blackwell_engine_syntax.o: src/omega_blackwell_engine.c src/omega_blackwell_engine.h src/omega_gpu_engine.h
	@mkdir -p build
	gcc -std=gnu11 -O2 -Wall -Wextra -Werror -D_GNU_SOURCE -Isrc $(GPU_ENGINE_PHYS_INC) -c -o $@ src/omega_blackwell_engine.c
test-gpu-engine-backend-compile: build/omega_blackwell_engine_syntax.o

# Links only, opens no device. Uses the real m16_native and nvrm sources at the physics pin.
test-gpu-engine-prod-build:
	$(MAKE) build/omegatool
