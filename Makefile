CC ?= gcc
.DEFAULT_GOAL := all
PHYSICS_DIR ?= ../physics
OUT_DIR ?= build
-include mk/*.mk
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

.PHONY: all clean check-physics-lock crumbline-learner test-crumbline test-m19 test test-m5 test-m6 test-m7 test-m8 test-m9 test-m10 test-m11 test-m12 test-m13 test-m14 test-m15 test-m17 test-r3 test-action-graph test-state-projection test-capability-query test-semantic-comm test-cognitive-routing test-sem-incremental test-branch-reuse test-plan-reuse

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

# Omega semantic variables and incremental recomputation
# (gate OMEGA_INCREMENTAL_SEMANTICS_PASS). CPU only, same links as R3.
RX_SEM_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_semantic.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_sem_incremental.c
RX_SEM_TEST = $(OUT_DIR)/rx_sem_incremental_test

$(RX_SEM_TEST): $(RX_SEM_SRCS) src/runtime/rx_semantic.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/omega_shared_world_abi.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SEM_SRCS)

test-sem-incremental: $(RX_SEM_TEST)
	./$(RX_SEM_TEST)

# R7: native AIENOS authority versus the Linux oracle, then the world view.
# Default: the authority pinned by aienos.lock, extracted with `git archive` from
# AIENOS_LOCK_REPO (a clone that has the commit) into $(OUT_DIR). An override must point
# at a tree with native/capability; the build stops if it does not (the old default
# ../aienos-r9 silently had none on the Spark).
AIENOS_LOCK_REPO ?= ../aienos-argus-cap
AIENOS_LOCK = $(shell head -n 1 aienos.lock)
AIENOS_R7_DEFAULT = $(OUT_DIR)/aienos-authority/$(shell echo $(AIENOS_LOCK) | cut -c1-7)
AIENOS_R7_DIR ?= $(AIENOS_R7_DEFAULT)
AIENOS_CAP_LIB ?= $(AIENOS_R7_DIR)/native/capability/out/libaienos_capability.a
RX_R7_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r7_native.c
RX_R7_TEST = $(OUT_DIR)/rx_r7_native_test

$(AIENOS_CAP_LIB):
	@if [ "$(AIENOS_R7_DIR)" = "$(AIENOS_R7_DEFAULT)" ] && [ ! -d "$(AIENOS_R7_DIR)/native/capability" ]; then \
		test -n "$(AIENOS_LOCK)" || { echo "aienos.lock is empty"; exit 1; }; \
		mkdir -p "$(AIENOS_R7_DIR)" && \
		git -C $(AIENOS_LOCK_REPO) archive $(AIENOS_LOCK) native/capability | tar -x -C "$(AIENOS_R7_DIR)"; \
	fi
	@test -f "$(AIENOS_R7_DIR)/native/capability/Makefile" || { \
		echo "AIENOS_R7_DIR=$(AIENOS_R7_DIR) has no native/capability:"; \
		echo "  set AIENOS_R7_DIR to an aienos tree at aienos.lock, or AIENOS_LOCK_REPO to a clone with it"; exit 1; }
	$(MAKE) -C $(AIENOS_R7_DIR)/native/capability

# OMEGA_EFFECT_CAP64 (spec/effect-cap64-migration.md): effect objects carry the
# full 64-bit AIENOS capability generation. Physics-free: the Omega core, the
# Visor effect-request adapter, and the pinned AIENOS header + library
# (aienos.lock) used directly. Prints OMEGA_EFFECT_CAP64_{ROUNDTRIP,IDENTITY,
# STALE_REJECT,AUTHORITY}_PASS gate lines.
EFFECT_CAP64_CAP_LIB ?= $(AIENOS_CAP_LIB)
EFFECT_CAP64_CAP_INC ?= $(patsubst %/,%,$(dir $(EFFECT_CAP64_CAP_LIB)))/..
EFFECT_CAP64_TEST = $(OUT_DIR)/tests-effect/test_effect_cap64
EFFECT_CAP64_OBJS = $(addprefix $(OUT_DIR)/,sha256.o omega_canonical.o omega_validate.o omega_core.o omega_codec.o)

$(EFFECT_CAP64_TEST): tests/effect/test_effect_cap64.c $(EFFECT_CAP64_OBJS) \
		$(OUT_DIR)/visor/visor_effect_request.o src/visor/visor_effect_request.h \
		src/omega_core.h src/omega_canonical.h src/omega_types.h $(EFFECT_CAP64_CAP_LIB)
	mkdir -p $(dir $@)
	$(CC) $(VISOR_CFLAGS) -I$(EFFECT_CAP64_CAP_INC) -pthread -o $@ tests/effect/test_effect_cap64.c \
		$(OUT_DIR)/visor/visor_effect_request.o $(EFFECT_CAP64_OBJS) $(EFFECT_CAP64_CAP_LIB) -lm

.PHONY: test-effect-cap64
test-effect-cap64: $(EFFECT_CAP64_TEST)
	./$(EFFECT_CAP64_TEST)

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

# R8: AEGIS resident authority. Capability slots in the world; AEGIS decides,
# only root.install mints, through the native AIENOS authority.
RX_R8_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_r8_aegis.c
RX_R8_TEST = $(OUT_DIR)/rx_r8_aegis_test

$(RX_R8_TEST): $(RX_R8_SRCS) src/runtime/rx_aegis.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R8_SRCS) $(AIENOS_CAP_LIB) -lm

test-r8: $(RX_R8_TEST)
	./$(RX_R8_TEST)

# R11: AIEN as a resident cognitive faculty. Part A drives AIEN alone on any
# host; part B is the living run with Omega, on hosts that have both Cortex-X925
# and Cortex-A725 cores. rx_aien.o is built alone first and must not reference
# any Omega symbol: AIEN reaches Omega only through world objects.
RX_R11_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_omega.c src/sha256.c src/omega_evidence.c \
	src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_r11_aien.c
RX_R11_TEST = $(OUT_DIR)/rx_r11_aien_test
RX_AIEN_OBJ = $(OUT_DIR)/rx_aien.o

$(RX_AIEN_OBJ): src/runtime/rx_aien.c src/runtime/rx_aien.h src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_aien.c
	@if nm -u $@ | grep -Ei 'omega' ; then \
		echo "rx_aien.o references Omega directly; AIEN must reach it only through the world"; \
		rm -f $@; exit 1; fi

$(RX_R11_TEST): $(RX_R11_SRCS) $(RX_AIEN_OBJ) src/runtime/rx_omega.h src/runtime/rx_caproot.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R11_SRCS) $(RX_AIEN_OBJ) $(AIENOS_CAP_LIB) -lm

test-r11: $(RX_R11_TEST)
	./$(RX_R11_TEST)

# OMEGA_STATE_PROJECTION: cognition gets a compiled state projection from
# Cortex, not everything Cortex knows. Part B drives the real R11 AIEN faculty
# under the native AIENOS authority. The projection objects are built alone
# first and may reference nothing but Cortex and SHA-256: no world, no faculty,
# no Omega realization code.
RX_SP_OBJS = $(OUT_DIR)/rx_cortex.o $(OUT_DIR)/rx_projection.o
RX_SP_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_sp_workloads.c tests/runtime/rx_state_projection.c
RX_SP_TEST = $(OUT_DIR)/rx_state_projection_test

$(OUT_DIR)/rx_cortex.o: src/runtime/rx_cortex.c src/runtime/rx_cortex.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_cortex.c

$(OUT_DIR)/rx_projection.o: src/runtime/rx_projection.c src/runtime/rx_projection.h \
	src/runtime/rx_cortex.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_projection.c
	@if nm -u $@ $(OUT_DIR)/rx_cortex.o | grep -E ' (rx_world|rx_aien|rx_omega|omega_)' ; then \
		echo "the projection references the world, a faculty or Omega realization code"; \
		rm -f $@; exit 1; fi

$(RX_SP_TEST): $(RX_SP_SRCS) $(RX_SP_OBJS) $(RX_AIEN_OBJ) tests/runtime/rx_sp_workloads.h \
	src/runtime/rx_world.h src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SP_SRCS) $(RX_SP_OBJS) $(RX_AIEN_OBJ) $(AIENOS_CAP_LIB) -lm

test-state-projection: $(RX_SP_TEST)
	./$(RX_SP_TEST)

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

# R13 host uses the R12 processor stand-in and cannot claim the silicon gate.
# R13 silicon runs the same world against the physical resident GB10 seat.
RX_R13_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c \
	src/runtime/rx_coherent.c src/runtime/rx_native_bind.c \
	src/runtime/rx_aegis.c src/runtime/rx_aien.c src/runtime/rx_omega.c \
	src/runtime/rx_generation.c src/runtime/rx_living.c \
	src/sha256.c src/omega_evidence.c src/omega_canonical.c \
	src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c \
	src/omega_realize_synth.c src/omega_machine.c src/omega_exec.c \
	src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_r13_living.c
RX_R13_HOST = $(OUT_DIR)/rx_r13_living_host
RX_R13_SILICON = $(OUT_DIR)/rx_r13_living_silicon

$(RX_R13_HOST): $(RX_R13_SRCS) src/runtime/rx_living.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R13_SRCS) $(AIENOS_CAP_LIB) -lm

$(RX_R13_SILICON): $(RX_R13_SRCS) src/runtime/rx_resident_gpu.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_qmd.c src/omega_blackwell_matmul.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR13_SILICON -pthread -o $@ $(RX_R13_SRCS) \
		src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
		src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
		src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c \
		$(PHYSICS_DIR)/nvrm/nvrm.c $(AIENOS_CAP_LIB) -ldl -lm

test-r13-host: $(RX_R13_HOST)
	./$(RX_R13_HOST)

test-r13-silicon: $(RX_R13_SILICON)
	./$(RX_R13_SILICON)

# OMEGA_BRANCH_STATE_REUSE: J-Space branches sharing one semantic prefix,
# shared-state realization versus independent recomputation, FORGE placement.
RX_BRANCH_REUSE_SRCS = src/runtime/rx_jspace.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_branch_reuse.c
RX_BRANCH_REUSE_TEST = $(OUT_DIR)/rx_branch_reuse

$(RX_BRANCH_REUSE_TEST): $(RX_BRANCH_REUSE_SRCS) src/runtime/rx_jspace.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ $(RX_BRANCH_REUSE_SRCS) -lm

test-branch-reuse: $(RX_BRANCH_REUSE_TEST)
	./$(RX_BRANCH_REUSE_TEST)

# R14: the R13 organism attacked while alive. Host uses the R12 processor
# stand-in and cannot claim the gate; silicon runs D and E on the GB10 seat.
RX_R14_SRCS = $(filter-out tests/runtime/rx_r13_living.c,$(RX_R13_SRCS)) \
	tests/runtime/rx_r14_recovery.c
RX_R14_HOST = $(OUT_DIR)/rx_r14_recovery_host
RX_R14_SILICON = $(OUT_DIR)/rx_r14_recovery_silicon

$(RX_R14_HOST): $(RX_R14_SRCS) src/runtime/rx_living.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R14_SRCS) $(AIENOS_CAP_LIB) -lm

$(RX_R14_SILICON): $(RX_R14_SRCS) src/runtime/rx_resident_gpu.c \
	src/omega_blackwell_codegen.c src/omega_blackwell_encoder.c \
	src/omega_blackwell_qmd.c src/omega_blackwell_matmul.c \
	$(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR14_SILICON -pthread -o $@ $(RX_R14_SRCS) \
		src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
		src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
		src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c \
		$(PHYSICS_DIR)/nvrm/nvrm.c $(AIENOS_CAP_LIB) -ldl -lm

test-r14-host: $(RX_R14_HOST)
	./$(RX_R14_HOST)

test-r14-silicon: $(RX_R14_SILICON)
	./$(RX_R14_SILICON)

# R15: instrumentation checks (host). Every counter the R15 harness reduces
# is checked against a case whose true value is known in advance.
RX_R15_INSTR_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	src/runtime/rx_seq_reference.c tests/runtime/rx_r15_instr.c
RX_R15_INSTR = $(OUT_DIR)/rx_r15_instr_test

$(RX_R15_INSTR): $(RX_R15_INSTR_SRCS) src/runtime/rx_world.h src/runtime/rx_generation.h \
	src/runtime/rx_caproot.h src/runtime/rx_seq_reference.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_INSTR_SRCS)

test-r15-instr: $(RX_R15_INSTR)
	./$(RX_R15_INSTR)

# R15: SEQ semantic parity gate (spec §16 C1 item 9). The R13 body is built
# by tests/runtime/rx_r15_rig.c for RES-1 and for the sequential reference.
RX_R15_RIG_SRCS = $(filter-out tests/runtime/rx_r13_living.c,$(RX_R13_SRCS)) \
	src/runtime/rx_seq_reference.c tests/runtime/rx_r15_rig.c
RX_R15_RIG_HDRS = src/runtime/rx_living.h src/runtime/rx_seq_reference.h \
	src/runtime/rx_world.h tests/runtime/rx_r15_rig.h
RX_R15_GPU_SRCS = src/runtime/rx_resident_gpu.c src/omega_blackwell_codegen.c \
	src/omega_blackwell_encoder.c src/omega_blackwell_qmd.c \
	src/omega_blackwell_matmul.c $(PHYSICS_DIR)/m16/m16_native.c $(PHYSICS_DIR)/nvrm/nvrm.c
RX_R15_PARITY_HOST = $(OUT_DIR)/rx_r15_parity_host
RX_R15_PARITY_SILICON = $(OUT_DIR)/rx_r15_parity_silicon

$(RX_R15_PARITY_HOST): $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_parity.c $(RX_R15_RIG_HDRS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_parity.c \
		$(AIENOS_CAP_LIB) -lm

$(RX_R15_PARITY_SILICON): $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_parity.c $(RX_R15_RIG_HDRS) \
	$(RX_R15_GPU_SRCS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR15_SILICON -pthread -o $@ $(RX_R15_RIG_SRCS) \
		tests/runtime/rx_r15_parity.c $(RX_R15_GPU_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

test-r15-parity-host: $(RX_R15_PARITY_HOST)
	./$(RX_R15_PARITY_HOST)

# R15 G7: production keeps its worker through a generation promotion; the R9
# store's physical work runs on the store's durable executor (host seat).
RX_R15_G7_HOST = $(OUT_DIR)/rx_r15_g7_host
$(RX_R15_G7_HOST): $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_g7.c $(RX_R15_RIG_HDRS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_RIG_SRCS) tests/runtime/rx_r15_g7.c \
		$(AIENOS_CAP_LIB) -lm
test-r15-g7-host: $(RX_R15_G7_HOST)
	./$(RX_R15_G7_HOST)

# R15 C2 sensor validation: bounded GB10 load through the resident seat.
R15_GPU_LOAD = $(OUT_DIR)/r15_gpu_load
R15_GPU_LOAD_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	tests/runtime/r15_gpu_load.c

$(R15_GPU_LOAD): $(R15_GPU_LOAD_SRCS) $(RX_R15_GPU_SRCS) src/runtime/rx_world.h \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(R15_GPU_LOAD_SRCS) $(RX_R15_GPU_SRCS) \
		$(AIENOS_CAP_LIB) -ldl -lm

r15-gpu-load: $(R15_GPU_LOAD)

test-r15-parity-silicon: $(RX_R15_PARITY_SILICON)
	./$(RX_R15_PARITY_SILICON)

# R15 harness (spec §4, §5, §12). Four binaries from the same sources and
# flags: production and the RES-1-NODIGEST measurement build (§3), host
# stand-in seat and GB10 silicon. tools/r15_qualify.sh drives them and
# tools/r15_reduce.c reduces their raw output.
RX_R15_PERF_SRCS = $(RX_R15_RIG_SRCS) tests/runtime/r15_measure.c tests/runtime/rx_r15_perf.c
RX_R15_PERF_HDRS = $(RX_R15_RIG_HDRS) tests/runtime/r15_measure.h
RX_R15_PERF_HOST = $(OUT_DIR)/rx_r15_perf_host
RX_R15_PERF_HOST_ND = $(OUT_DIR)/rx_r15_perf_host_nodigest
RX_R15_PERF_SILICON = $(OUT_DIR)/rx_r15_perf_silicon
RX_R15_PERF_SILICON_ND = $(OUT_DIR)/rx_r15_perf_silicon_nodigest
R15_REDUCE = $(OUT_DIR)/r15_reduce

$(RX_R15_PERF_HOST): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_R15_PERF_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

$(RX_R15_PERF_HOST_ND): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DRX_MEASURE_NO_CAUSAL_DIGEST -pthread -o $@ $(RX_R15_PERF_SRCS) \
		$(AIENOS_CAP_LIB) -ldl -lm

$(RX_R15_PERF_SILICON): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(RX_R15_GPU_SRCS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR15_SILICON -pthread -o $@ $(RX_R15_PERF_SRCS) $(RX_R15_GPU_SRCS) \
		$(AIENOS_CAP_LIB) -ldl -lm

$(RX_R15_PERF_SILICON_ND): $(RX_R15_PERF_SRCS) $(RX_R15_PERF_HDRS) $(RX_R15_GPU_SRCS) \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -DR15_SILICON -DRX_MEASURE_NO_CAUSAL_DIGEST -pthread -o $@ \
		$(RX_R15_PERF_SRCS) $(RX_R15_GPU_SRCS) $(AIENOS_CAP_LIB) -ldl -lm

$(R15_REDUCE): tools/r15_reduce.c src/sha256.c src/sha256.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tools/r15_reduce.c src/sha256.c -lm

r15-perf-host: $(RX_R15_PERF_HOST) $(RX_R15_PERF_HOST_ND) $(R15_REDUCE)
r15-perf-silicon: $(RX_R15_PERF_SILICON) $(RX_R15_PERF_SILICON_ND) $(R15_REDUCE)

# R15 receipt writer (spec §12/§13/§14): tools/r15_receipt.sh turns one run
# directory (summary.json + machine.json + SHA256SUMS) into
# evidence/R15/<sha256>.json. The host test runs it on attempt 1 (a FAIL).
r15-receipt:
	@test -n "$(RUN)" || { echo "usage: make r15-receipt RUN=evidence/R15/raw/<run-id> [CANDIDATE=<commit>] [RERUNS=<file>] [NOTES=<file>]"; exit 2; }
	tools/r15_receipt.sh $(RUN) evidence/R15 "$(CANDIDATE)" "$(RERUNS)" "$(NOTES)"

.PHONY: r15-receipt test-r15-receipt
test-r15-receipt:
	tests/r15_receipt_test.sh

# R16-G2 code-search gate (spec/r16-orchestrator-retirement.md). Host-only C tool,
# seconds, no network. Scans the five repos (paths from R16_REPO_OMEGA,
# R16_REPO_SOVEREIGN_CORE, R16_REPO_AEGIS_RUNTIME, R16_REPO_AIENOS,
# R16_REPO_PHYSICS; defaults: this tree and ~/workspace/r16-survey/<repo>) and
# joins every loop-shaped site with spec/r16-orchestrator-retirement-map.md.
# Exit 0 only when every site is classified A-F, no omega class-A site is
# still under a production name, and all five repos were scanned.
R16_INVENTORY = $(OUT_DIR)/r16_loop_inventory
$(R16_INVENTORY): tools/r16_loop_inventory.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tools/r16_loop_inventory.c

.PHONY: r16-inventory test-r16-inventory
r16-inventory: $(R16_INVENTORY)
	$(R16_INVENTORY) --map spec/r16-orchestrator-retirement-map.md --json $(OUT_DIR)/r16-inventory.json

test-r16-inventory: $(R16_INVENTORY)
	sh tests/r16_inventory/run.sh $(R16_INVENTORY)


# OMEGA_ACTION_GRAPH_IR: goals compile to typed action graphs that run as
# resident reactions by readiness alone. rx_graph.o is built alone first and
# must not reference any AIENOS admin operation: compilation can find that
# authority is missing, never create it.
RX_GRAPH_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_action_graph.c
RX_GRAPH_TEST = $(OUT_DIR)/rx_action_graph_test
RX_GRAPH_OBJ = $(OUT_DIR)/rx_graph.o

$(RX_GRAPH_OBJ): src/runtime/rx_graph.c src/runtime/rx_graph.h src/runtime/rx_world.h \
	src/runtime/rx_aegis.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_graph.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_graph.o references an authority admin operation; compilation must not mint"; \
		rm -f $@; exit 1; fi

$(RX_GRAPH_TEST): $(RX_GRAPH_SRCS) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_GRAPH_SRCS) $(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm

test-action-graph: $(RX_GRAPH_TEST)
	./$(RX_GRAPH_TEST)

# OMEGA_CAPABILITY_QUERY: AIEN states a CapabilityNeed; Omega compiles it into
# probes of the capability sources and returns a bounded list of candidates
# ranked on explicit dimensions. rx_capq.o is built alone first and must not
# reference any AIENOS admin operation: a query can learn that authority is
# held or missing, never create it.
RX_CAPQ_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_capability_query.c
RX_CAPQ_TEST = $(OUT_DIR)/rx_capability_query_test
RX_CAPQ_OBJ = $(OUT_DIR)/rx_capq.o

$(RX_CAPQ_OBJ): src/runtime/rx_capq.c src/runtime/rx_capq.h src/runtime/rx_graph.h \
	src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_capq.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_capq.o references an authority admin operation; a query must not mint"; \
		rm -f $@; exit 1; fi

$(RX_CAPQ_TEST): $(RX_CAPQ_SRCS) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_CAPQ_SRCS) $(RX_CAPQ_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm

test-capability-query: $(RX_CAPQ_TEST)
	./$(RX_CAPQ_TEST)

# OMEGA_PLAN_REUSE: plan IR and verified plan cache. rx_plan.o must not
# reference any AIENOS admin operation (the cache checks authority, never
# creates it); rx_plan_arrange.o (AIEN's planner) must only read the World.
RX_PLAN_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_plan_reuse.c
RX_PLAN_TEST = $(OUT_DIR)/rx_plan_reuse_test
RX_PLAN_OBJS = $(OUT_DIR)/rx_plan.o $(OUT_DIR)/rx_plan_arrange.o

$(OUT_DIR)/rx_plan.o: src/runtime/rx_plan.c src/runtime/rx_plan.h src/runtime/rx_graph.h \
	src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_plan.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_plan.o references an authority admin operation; the plan cache must not mint"; \
		rm -f $@; exit 1; fi

$(OUT_DIR)/rx_plan_arrange.o: src/runtime/rx_plan_arrange.c src/runtime/rx_plan_arrange.h \
	src/runtime/rx_plan.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_plan_arrange.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_|rx_world_publish|rx_graph_lower|rx_graph_start' ; then \
		echo "rx_plan_arrange.o must only read the World: the planner changes nothing"; \
		rm -f $@; exit 1; fi

$(RX_PLAN_TEST): $(RX_PLAN_SRCS) $(RX_PLAN_OBJS) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_PLAN_SRCS) $(RX_PLAN_OBJS) $(RX_GRAPH_OBJ) $(AIENOS_CAP_LIB) -lm

test-plan-reuse: $(RX_PLAN_TEST)
	./$(RX_PLAN_TEST)

# OMEGA_SEMANTIC_COMMUNICATION: branches and workers receive the semantic
# projection their InformationNeed selects, then only deltas, not full state.
# rx_semcomm.o is built alone first. It enforces capability boundaries, so it
# must not reference any authority admin operation, and it only reads the
# World, so it must not reference a World write either.
RX_SEMCOMM_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_semantic_comm.c
RX_SEMCOMM_TEST = $(OUT_DIR)/rx_semantic_comm_test
RX_SEMCOMM_OBJ = $(OUT_DIR)/rx_semcomm.o

$(RX_SEMCOMM_OBJ): src/runtime/rx_semcomm.c src/runtime/rx_semcomm.h src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_semcomm.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke|rx_world_publish|rx_world_create|rx_world_retire|rx_world_add_reaction' ; then \
		echo "rx_semcomm.o references an authority admin operation or a World write; projection must only read"; \
		rm -f $@; exit 1; fi

$(RX_SEMCOMM_TEST): $(RX_SEMCOMM_SRCS) $(RX_SEMCOMM_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_SEMCOMM_SRCS) $(RX_SEMCOMM_OBJ) $(AIENOS_CAP_LIB) -lm

test-semantic-comm: $(RX_SEMCOMM_TEST)
	./$(RX_SEMCOMM_TEST)

# OMEGA_COGNITIVE_ROUTING: each cognitive step goes to the cheapest
# realization the evidence says meets it, escalating only on insufficient
# verification or calibrated confidence. rx_route.o is built alone first and
# must not reference promotion or any AIENOS admin operation: routing profiles
# change only through a promoted generation.
RX_ROUTE_SRCS = src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	tests/runtime/rx_cog_engines.c tests/runtime/rx_cognitive_routing.c
RX_ROUTE_TEST = $(OUT_DIR)/rx_cognitive_routing_test
RX_ROUTE_OBJ = $(OUT_DIR)/rx_route.o

$(RX_ROUTE_OBJ): src/runtime/rx_route.c src/runtime/rx_route.h src/runtime/rx_generation.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_route.c
	@if nm -u $@ | grep -E 'rx_gen_promote|aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_route.o references promotion or an authority admin operation"; \
		rm -f $@; exit 1; fi

$(RX_ROUTE_TEST): $(RX_ROUTE_SRCS) $(RX_ROUTE_OBJ) tests/runtime/rx_cog_engines.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -Itests -o $@ $(RX_ROUTE_SRCS) $(RX_ROUTE_OBJ) $(AIENOS_CAP_LIB) -lm

test-cognitive-routing: $(RX_ROUTE_TEST)
	./$(RX_ROUTE_TEST)

# OMEGA_EMPIRICAL_OPTIMIZER: realization choice from a learned, calibrated cost
# model. rx_costmodel.o is built alone first and must not reference the
# generation store, any authority operation, or anything that maps or runs
# code: the model is a calculator; durability comes only through promotion.
.PHONY: test-costmodel test-empirical
RX_CM_OBJ = $(OUT_DIR)/rx_costmodel.o

$(RX_CM_OBJ): src/runtime/rx_costmodel.c src/runtime/rx_costmodel.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_costmodel.c
	@if nm -u $@ | grep -E 'rx_gen_|aienos_cap_|rx_caproot_|rx_capadmin_|rx_world_|mmap|mprotect|fork|exec|dlopen|system' ; then \
		echo "rx_costmodel.o references an operation a cost model must not have"; \
		rm -f $@; exit 1; fi

# Any host: synthetic measurements; fit, calibration, decisions, blob.
RX_CM_UNIT = $(OUT_DIR)/rx_costmodel_unit
$(RX_CM_UNIT): tests/runtime/rx_costmodel_unit.c $(RX_CM_OBJ) src/sha256.c | $(OUT_DIR)
	$(CC) $(CFLAGS) -o $@ tests/runtime/rx_costmodel_unit.c $(RX_CM_OBJ) src/sha256.c -lm

test-costmodel: $(RX_CM_UNIT)
	./$(RX_CM_UNIT)

# AArch64 hosts: the gate. Real realizations, training, promotion through the
# R9 barrier on the native authority, held-out workloads.
RX_EMP_SRCS = src/runtime/rx_generation.c src/sha256.c src/omega_evidence.c \
	src/omega_canonical.c src/omega_validate.c src/omega_core.c src/omega_codec.c \
	src/aarch64_encoder.c src/aarch64_decoder.c src/omega_realize.c src/omega_realize_synth.c \
	src/omega_machine.c src/omega_exec.c src/omega_verify.c src/omega_matvec.c src/omega_matvec_quad.c \
	tests/runtime/rx_empirical_optimizer.c
RX_EMP_TEST = $(OUT_DIR)/rx_empirical_optimizer_test

$(RX_EMP_TEST): $(RX_EMP_SRCS) $(RX_CM_OBJ) src/runtime/rx_generation.h src/runtime/aienos_cap.h \
	$(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_EMP_SRCS) $(RX_CM_OBJ) $(AIENOS_CAP_LIB) -lm

test-empirical: $(RX_EMP_TEST)
	./$(RX_EMP_TEST)

# OMEGA_WORKFLOW_FUSION: repeated verified action-graph fragments become
# MetaSkills; only a verified, measured, canaried, promoted (R9 barrier) and
# published one replaces the steps. rx_fusion.o, like rx_graph.o, must not
# reference any AIENOS admin operation.
.PHONY: test-workflow-fusion
RX_FUSION_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/runtime/rx_generation.c src/sha256.c \
	src/omega_evidence.c src/omega_core.c src/omega_canonical.c tests/runtime/rx_workflow_fusion.c
RX_FUSION_TEST = $(OUT_DIR)/rx_workflow_fusion_test
RX_FUSION_OBJ = $(OUT_DIR)/rx_fusion.o

$(RX_FUSION_OBJ): src/runtime/rx_fusion.c src/runtime/rx_fusion.h src/runtime/rx_graph.h \
	src/runtime/rx_generation.h src/runtime/rx_world.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_fusion.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_fusion.o references an authority admin operation; fusion must not mint"; \
		rm -f $@; exit 1; fi

$(RX_FUSION_TEST): $(RX_FUSION_SRCS) $(RX_FUSION_OBJ) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_FUSION_SRCS) $(RX_FUSION_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm

test-workflow-fusion: $(RX_FUSION_TEST)
	./$(RX_FUSION_TEST)

# OMEGA_TYPED_RESULT_CONSTRAINTS: structured results of cognition are checked
# against typed contracts; the publish gate is the only writer of what is
# published. rx_contract.o is built alone first and must not reference any
# AIENOS admin operation: a contract can find that authority is missing, never
# create it.
RX_TYPED_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c src/omega_evidence.c \
	src/omega_core.c src/omega_canonical.c tests/runtime/rx_typed_results.c
RX_TYPED_TEST = $(OUT_DIR)/rx_typed_results_test
RX_CONTRACT_OBJ = $(OUT_DIR)/rx_contract.o
TYPED_RESULTS_N ?= 400

$(RX_CONTRACT_OBJ): src/runtime/rx_contract.c src/runtime/rx_contract.h src/runtime/rx_graph.h \
	src/runtime/rx_world.h src/runtime/rx_aien.h | $(OUT_DIR)
	$(CC) $(CFLAGS) -c -o $@ src/runtime/rx_contract.c
	@if nm -u $@ | grep -E 'aienos_cap_|rx_caproot_mint|rx_caproot_revoke' ; then \
		echo "rx_contract.o references an authority admin operation; a contract must not mint"; \
		rm -f $@; exit 1; fi

$(RX_TYPED_TEST): $(RX_TYPED_SRCS) $(RX_CONTRACT_OBJ) $(RX_GRAPH_OBJ) src/runtime/rx_caproot.h \
	src/runtime/aienos_cap.h $(AIENOS_CAP_LIB) | $(OUT_DIR)
	$(CC) $(CFLAGS) -pthread -o $@ $(RX_TYPED_SRCS) $(RX_CONTRACT_OBJ) $(RX_GRAPH_OBJ) \
		$(AIENOS_CAP_LIB) -lm

.PHONY: test-typed-results
test-typed-results: $(RX_TYPED_TEST)
	./$(RX_TYPED_TEST) $(TYPED_RESULTS_N)

# ---- ARGUS producer (feat/argus-producer, lanes H/H2 of ARGUS-0) ---------
# The runtime feeds ARGUS (src/runtime/rx_argus.{c,h}, ABI v1.1: per-thread
# rings, use tables flushed as CAPABILITY_USE_SUMMARY, full events only on
# transitions). Default builds are unchanged (RX_ARGUS undefined = 0: the
# hooks compile to nothing). This block builds the AEGIS/promotion suites with
# RX_ARGUS=$(RX_ARGUS) against ARGUS pinned by argus.lock, extracted with
# `git archive` from ARGUS_REPO (never built in place). ARGUS's sha256.c is
# Omega's src/sha256.c copied unchanged; Omega's is linked, not both.
#
# Grant source (ARGUS_AUTH):
#   observer (default) the authority announces its own mints/revokes
#            (aienos_cap_set_observer, aienos 12add16, native in the authority
#            pinned by aienos.lock, d39dd5b); the observer is installed by a link
#            wrap of aienos_cap_start, which also announces the office (cap 0).
#   aegis    GRANTED/REVOKED only where rx_aegis mints/revokes (harness mints
#            are unseen and show up as FORGED).
# Every ARGUS target links the aienos.lock authority, $(AIENOS_CAP_LIB), the
# same library as the default targets (with no observer set it
# costs one NULL check per admin operation), RX_ARGUS=0 included.
ARGUS_REPO ?= ../aienos-argus
ARGUS_COMMIT ?= $(shell head -n 1 argus.lock)
ARGUS_SHORT = $(shell echo $(ARGUS_COMMIT) | cut -c1-7)
ARGUS_SRC = $(OUT_DIR)/argus-src/$(ARGUS_SHORT)/native/argus
ARGUS_STAMP = $(OUT_DIR)/argus-src/$(ARGUS_SHORT)/.extracted
ARGUS_LIB_SRCS = $(addprefix $(ARGUS_SRC)/,argus_event.c argus_ring.c argus_core.c argus_detect.c)
RX_ARGUS ?= 2
ARGUS_AUTH ?= observer
ARGUS_CAP_LIB = $(AIENOS_CAP_LIB)
comma := ,
ARGUS_AUTH_FLAGS = $(if $(filter observer,$(ARGUS_AUTH)),-DRX_ARGUS_AUTHORITY_OBSERVER -Wl$(comma)--wrap=aienos_cap_start,)
ARGUS_VARIANT = $(if $(filter observer,$(ARGUS_AUTH)),,-$(ARGUS_AUTH))
ARGUS_OUT = $(OUT_DIR)/argus$(RX_ARGUS)$(ARGUS_VARIANT)
ARGUS_CFLAGS = $(CFLAGS) -DRX_ARGUS=$(RX_ARGUS) -I$(ARGUS_SRC)
ARGUS_STREAMS ?= $(HOME)/workspace/argus-runtime-streams
ARGUS_RUN_ID = $(shell git rev-parse --short HEAD 2>/dev/null)-argus$(ARGUS_SHORT)

$(ARGUS_STAMP):
	@test -n "$(ARGUS_COMMIT)" || { echo "argus.lock is empty"; exit 1; }
	mkdir -p $(OUT_DIR)/argus-src/$(ARGUS_SHORT)
	git -C $(ARGUS_REPO) archive $(ARGUS_COMMIT) native/argus | tar -x -C $(OUT_DIR)/argus-src/$(ARGUS_SHORT)
	touch $@

$(ARGUS_LIB_SRCS): $(ARGUS_STAMP)

# RX_ARGUS=0 links no ARGUS code at all.
ARGUS_RX = $(if $(filter 0,$(RX_ARGUS)),,src/runtime/rx_argus.c $(ARGUS_LIB_SRCS))
ARGUS_LINK_FLAGS = $(if $(filter 0,$(RX_ARGUS)),,$(ARGUS_AUTH_FLAGS))
ARGUS_R7 = $(ARGUS_OUT)/rx_r7_native_test
ARGUS_R8 = $(ARGUS_OUT)/rx_r8_aegis_test
ARGUS_R9 = $(ARGUS_OUT)/rx_r9_barrier_test
ARGUS_BENCH = $(ARGUS_OUT)/bench_rx_argus
ARGUS_BENCH_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_aegis.c src/sha256.c tests/bench_rx_argus.c

$(ARGUS_OUT):
	mkdir -p $@

$(ARGUS_R7): $(RX_R7_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) $(ARGUS_LINK_FLAGS) -pthread -o $@ $(RX_R7_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm
$(ARGUS_R8): $(RX_R8_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) $(ARGUS_LINK_FLAGS) -pthread -o $@ $(RX_R8_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm
$(ARGUS_R9): $(RX_R9_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) $(ARGUS_LINK_FLAGS) -pthread -o $@ $(RX_R9_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm
# The bench mints directly and announces its one grant itself: no observer wrap.
$(ARGUS_BENCH): $(ARGUS_BENCH_SRCS) src/runtime/rx_argus.h $(ARGUS_RX) $(ARGUS_STAMP) $(ARGUS_CAP_LIB) | $(ARGUS_OUT)
	$(CC) $(ARGUS_CFLAGS) -pthread -o $@ $(ARGUS_BENCH_SRCS) $(ARGUS_RX) $(ARGUS_CAP_LIB) -lm

# ARGUS_RUNTIME_INTEGRATION: run each suite with the consumer ingesting into
# the real argus_core; keep the raw 128-byte stream (consumer ingest order)
# and a JSON summary: $(ARGUS_STREAMS)/<argus>-<suite>-v11.bin/.json
.PHONY: test-argus-runtime bench-rx-argus
test-argus-runtime: $(ARGUS_R7) $(ARGUS_R8) $(ARGUS_R9)
	mkdir -p $(ARGUS_STREAMS)
	@set -e; for s in r7:$(ARGUS_R7) r8:$(ARGUS_R8) r9:$(ARGUS_R9); do \
		n=$${s%%:*}; b=$${s#*:}; \
		echo "== ARGUS runtime integration $$n"; \
		RX_ARGUS_CONSUMER=ingest RX_ARGUS_RUN_ID=$(ARGUS_RUN_ID)-$$n RX_ARGUS_SUITE=$$n \
		RX_ARGUS_STREAM=$(ARGUS_STREAMS)/$(ARGUS_SHORT)-$$n$(ARGUS_VARIANT)-v11.bin \
		RX_ARGUS_SUMMARY=$(ARGUS_STREAMS)/$(ARGUS_SHORT)-$$n$(ARGUS_VARIANT)-v11.json ./$$b > $(ARGUS_OUT)/$$n.log 2>&1 \
		|| { tail -20 $(ARGUS_OUT)/$$n.log; exit 1; }; \
		tail -2 $(ARGUS_OUT)/$$n.log; \
	done

bench-rx-argus: $(ARGUS_BENCH)
	./$(ARGUS_BENCH)

ARGUS_REPLAY = $(OUT_DIR)/argus_replay-$(ARGUS_SHORT)
$(ARGUS_REPLAY): tools/argus_replay.c src/sha256.c $(ARGUS_STAMP) | $(OUT_DIR)
	$(CC) $(CFLAGS) -I$(ARGUS_SRC) -o $@ tools/argus_replay.c src/sha256.c $(ARGUS_LIB_SRCS)
.PHONY: argus-replay
argus-replay: $(ARGUS_REPLAY)
