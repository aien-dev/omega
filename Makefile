CC ?= gcc
.DEFAULT_GOAL := all
PHYSICS_DIR ?= ../physics
OUT_DIR ?= build
PHYSICS_LOCK_CHECK ?= 1

CFLAGS ?= -std=gnu11 -Wall -Wextra -Werror -MMD -MP -D_GNU_SOURCE -O2 -Isrc \
	-I$(PHYSICS_DIR)/m16 -I$(PHYSICS_DIR)/nvrm \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/common/sdk/nvidia/inc \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/kernel-open/common/inc \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/kernel-open/nvidia-uvm \
	-I$(PHYSICS_DIR)/third_party/nvidia-open-580.173.02/src/nvidia/arch/nvalloc/unix/include \
	-DOMEGA_PHYSICS_DIR=\"$(PHYSICS_DIR)\"

SRCS = src/sha256.c src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_exec.c \
	src/omega_self_host.c src/omega_verify.c src/omega_program.c src/omega_synthesis.c \
	src/omega_library.c src/omega_discovery.c src/omega_machine.c src/omega_realize_synth.c \
	src/omega_matvec.c src/omega_accelerator.c src/omega_accelerator_world.c \
	src/omega_vector.c src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_realize.c src/omega_blackwell_submit.c src/omega_blackwell_gates.c src/omega_blackwell_matmul.c src/omega_blackwell_codegen.c src/omega_world_gates.c \
	src/omega_evidence.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	tools/omegatool.c

OBJS = $(patsubst %.c,$(OUT_DIR)/%.o,$(notdir $(SRCS)))
DEPS = $(OBJS:.o=.d)
-include $(DEPS)
TARGET = $(OUT_DIR)/omegatool

# Crumbline learner: a separate executable linked ONLY from learner-side code
# (visible reader, Omega program/realize/verify stack, synthesis vocabulary,
# OmegaLibrary, Crumbline search). It links no physics, no gate suites and no
# sealed-side code; the sealed side lives in the crumbs crate.
CL_SRCS = src/crumbline/cl_common.c src/crumbline/cl_crumb.c src/crumbline/cl_program.c src/crumbline/cl_search.c
LEARNER_CORE = sha256 omega_canonical omega_validate omega_core omega_codec aarch64_encoder aarch64_decoder \
	omega_realize omega_realize_synth omega_machine omega_exec omega_verify omega_program omega_synthesis omega_library
LEARNER_OBJS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,$(LEARNER_CORE))) \
	$(patsubst src/crumbline/%.c,$(OUT_DIR)/crumbline/%.o,$(CL_SRCS)) $(OUT_DIR)/crumbline_learner.o
LEARNER = $(OUT_DIR)/crumbline-learner

.PHONY: all clean check-physics-lock crumbline-learner test-crumbline test-m19 test test-m5 test-m6 test-m7 test-m8 test-m9 test-m10 test-m11 test-m12 test-m13 test-m14 test-m15 test-m17 test-r3

all: $(TARGET)

check-physics-lock:
	@if [ "$(PHYSICS_LOCK_CHECK)" != "0" ]; then \
		if [ ! -f physics.lock ]; then \
			echo "error: physics.lock not found at repo root" >&2; exit 1; \
		fi; \
		locked=$$(tr -d '[:space:]' < physics.lock); \
		actual=$$(git -C $(PHYSICS_DIR) rev-parse HEAD 2>/dev/null); \
		if [ -z "$$actual" ]; then \
			echo "error: could not read HEAD of PHYSICS_DIR=$(PHYSICS_DIR) (git -C $(PHYSICS_DIR) rev-parse HEAD failed)" >&2; \
			exit 1; \
		fi; \
		if [ "$$actual" != "$$locked" ]; then \
			echo "error: PHYSICS_DIR=$(PHYSICS_DIR) is at commit $$actual but physics.lock pins $$locked." >&2; \
			echo "       Checkout the pinned physics commit, or override with PHYSICS_LOCK_CHECK=0." >&2; \
			exit 1; \
		fi; \
	fi

$(OUT_DIR):
	mkdir -p $(OUT_DIR)

$(OUT_DIR)/%.o: src/%.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/m16_native.o: $(PHYSICS_DIR)/m16/m16_native.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/nvrm.o: $(PHYSICS_DIR)/nvrm/nvrm.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/crumbline/%.o: src/crumbline/%.c | $(OUT_DIR)
	mkdir -p $(OUT_DIR)/crumbline
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT_DIR)/crumbline_learner.o: tools/crumbline_learner.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

crumbline-learner: $(LEARNER)

test-crumbline: $(LEARNER)
	./tests/crumbline/run_conformance.sh $(LEARNER) tests/crumbline/vectors

$(LEARNER): $(LEARNER_OBJS)
	$(CC) $(CFLAGS) -o $@ $(LEARNER_OBJS)

$(OUT_DIR)/omegatool.o: tools/omegatool.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(TARGET): check-physics-lock $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

test: $(TARGET)
	./$(TARGET) --run-gates
	./$(TARGET) --demonstrate-arithmetic
	./$(TARGET) --demonstrate-physics

test-m5: $(TARGET)
	./$(TARGET) --run-m5-gates
	./$(TARGET) --demonstrate-realization

test-m6: $(TARGET)
	./$(TARGET) --run-m6-gates
	./$(TARGET) --demonstrate-self-host

test-m7: $(TARGET)
	./$(TARGET) --run-m7-gates
	./$(TARGET) --demonstrate-verify

test-m8: $(TARGET)
	./$(TARGET) --run-m8-gates
	./$(TARGET) --demonstrate-program

test-m9: $(TARGET)
	./$(TARGET) --run-m9-gates
	./$(TARGET) --demonstrate-synthesis

test-m10: $(TARGET)
	./$(TARGET) --run-m10-gates
	./$(TARGET) --demonstrate-library

test-m11: $(TARGET)
	./$(TARGET) --run-m11-gates
	./$(TARGET) --demonstrate-discovery

test-m12: $(TARGET)
	./$(TARGET) --run-m12-gates
	./$(TARGET) --demonstrate-living-matvec

test-m13: $(TARGET)
	./$(TARGET) --run-m13-gates
	./$(TARGET) --demonstrate-machine

test-m14: $(TARGET)
	./$(TARGET) --run-m14-gates
	./$(TARGET) --demonstrate-realization-synthesis

test-m15: $(TARGET)
	./$(TARGET) --run-m15-gates
	./$(TARGET) --demonstrate-accelerator

test-m17: $(TARGET)
	./$(TARGET) --run-m17-gates
	./$(TARGET) --demonstrate-blackwell-vector

clean:
	rm -rf $(OUT_DIR)

test-m19: $(TARGET)
	./$(TARGET) --run-m19-gates

# Resident reaction runtime heartbeat (ADR 0016, R3/R4 host reference).
# CPU only; links no PHYSICS/NVRM code (omega_evidence.c needs only the header).
RX_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/sha256.c src/omega_evidence.c tests/runtime/rx_heartbeat_test.c
RX_TEST = $(OUT_DIR)/rx_heartbeat_test

$(RX_TEST): $(RX_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SRCS)

test-r3: $(RX_TEST)
	./$(RX_TEST)

# R7: native AIENOS authority versus the Linux oracle, then the world view.
AIENOS_R7_DIR ?= ../aienos-r9
AIENOS_CAP_LIB ?= $(AIENOS_R7_DIR)/native/capability/out/libaienos_capability.a
RX_R7_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r7_native.c
RX_R7_TEST = $(OUT_DIR)/rx_r7_native_test

$(AIENOS_CAP_LIB):
	$(MAKE) -C $(AIENOS_R7_DIR)/native/capability

$(RX_R7_TEST): $(RX_R7_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/aienos_cap.h src/runtime/omega_shared_world_abi.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R7_SRCS) $(AIENOS_CAP_LIB) -lm

test-r7: $(RX_R7_TEST)
	./$(RX_R7_TEST)

# R9: generation barrier. The candidate is prepared beside the live world.
# A stop after each persistence step must recover one whole generation.
RX_R9_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r9_barrier.c
RX_R9_TEST = $(OUT_DIR)/rx_r9_barrier_test

$(RX_R9_TEST): $(RX_R9_SRCS) src/runtime/rx_generation.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R9_SRCS) $(AIENOS_CAP_LIB) -lm

test-r9: $(RX_R9_TEST)
	./$(RX_R9_TEST)

# R12 host rules against the native AIENOS authority. The graphics processor is
# not started. A pass here does not claim the resident-seat gate.
RX_R12_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r12_resident.c
RX_R12_TEST = $(OUT_DIR)/rx_r12_resident_test

$(RX_R12_TEST): $(RX_R12_SRCS) src/runtime/rx_caproot.h src/runtime/rx_world.h \
	src/runtime/aienos_cap.h src/runtime/omega_shared_world_abi.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R12_SRCS) $(AIENOS_CAP_LIB) -lm

test-r12: $(RX_R12_TEST)
	./$(RX_R12_TEST)

# R10: Omega as a resident realization faculty. Omega's native AArch64 matvec
# realizations become ready from cost evidence, are verified in a sandbox,
# measured, and recorded; production picks the record up. AArch64 hosts only.
RX_R10_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_omega.c src/sha256.c src/omega_evidence.c \
	src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_r10_omega.c
RX_R10_TEST = $(OUT_DIR)/rx_r10_omega_test

$(RX_R10_TEST): $(RX_R10_SRCS) src/runtime/rx_omega.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R10_SRCS) $(AIENOS_CAP_LIB) -lm

test-r10: $(RX_R10_TEST)
	./$(RX_R10_TEST)

# Physical graphics seat against the native AIENOS authority. Not part of
# GitHub checks. A pass on this machine is the only run that may set
# silicon_observed.
RX_R12_SILICON_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c \
	src/runtime/rx_coherent.c src/runtime/rx_native_bind.c \
	src/runtime/rx_resident_gpu.c src/sha256.c src/omega_evidence.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_qmd.c src/omega_blackwell_matmul.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	tests/runtime/rx_r12_silicon.c
RX_R12_SILICON = $(OUT_DIR)/rx_r12_silicon_test

$(RX_R12_SILICON): $(RX_R12_SILICON_SRCS) src/runtime/rx_world.h \
	src/runtime/rx_resident_gpu.h src/runtime/rx_caproot.h src/runtime/aienos_cap.h \
	src/omega_blackwell_codegen.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R12_SILICON_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

test-r12-silicon: $(RX_R12_SILICON)
	./$(RX_R12_SILICON)
