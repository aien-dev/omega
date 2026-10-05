# ALLEN v0 (ARCH-0035 PROPOSED, OS-0018 PROPOSED): the durable subject inside
# the AIEN organism. Picked up by `-include mk/*.mk`. New files only:
# src/allen/allen_bind.{c,h}, tools/allen.c, tests/allen/run.sh, spec/allen.md.
# Nothing here touches src/runtime; rx_aien.o is the same object R11 links.
#
# The subject object codec is the AIENOS one (native/kernel/svc/
# continuity_subject.c at aienos.lock), compiled here from $(AIENOS_R7_DIR):
# omega never redefines the format. The default extraction (Makefile, R7) only
# takes native/capability, so $(ALLEN_STAMP) extends it with the four trees
# the codec needs.
#
# mk/*.mk is read before the Makefile defines OUT_DIR-derived R7 variables
# (AIENOS_R7_DIR, AIENOS_LOCK, RX_AIEN_OBJ, AIENOS_CAP_LIB), so every
# prerequisite that depends on them is written $$(...) and expanded in the
# second phase (as polyglot.mk does). Recipes expand late anyway.
#
# test-allen: tests/allen/run.sh runs gates G1..G6 (identity, restart,
# memory, model independence, no authority / no loop, negative control) plus
# the refusals (unknown version, corruption, replay, supersession, subject
# mismatch, model mismatch tolerance) and writes nothing outside $(ALLEN_DIR).
.PHONY: test-allen allen
.SECONDEXPANSION:
ALLEN_DIR = $(OUT_DIR)/allen
ALLEN_TOOL = $(ALLEN_DIR)/allen
ALLEN_BIND_OBJ = $(ALLEN_DIR)/allen_bind.o
ALLEN_MAIN_OBJ = $(ALLEN_DIR)/allen.o
ALLEN_STAMP = $(ALLEN_DIR)/aienos-tree.stamp
ALLEN_AIENOS_SVC = $(AIENOS_R7_DIR)/native/kernel/svc
# kernel/core is -idirafter: it holds a sched.h that must not shadow <sched.h>.
ALLEN_AIENOS_INC = -I$(ALLEN_AIENOS_SVC) -idirafter $(AIENOS_R7_DIR)/native/kernel/core \
	-I$(AIENOS_R7_DIR)/native/argus -I$(AIENOS_R7_DIR)/native/store
# One sha256: omega's src/sha256.c (same ctx layout and names as native/argus).
ALLEN_AIENOS_SRCS = $(ALLEN_AIENOS_SVC)/continuity_subject.c $(ALLEN_AIENOS_SVC)/continuity_codec.c \
	$(AIENOS_R7_DIR)/native/store/store_v1.c
ALLEN_RX_SRCS = src/runtime/rx_caproot.c src/runtime/rx_world.c src/runtime/rx_coherent.c \
	src/runtime/rx_native_bind.c src/runtime/rx_cortex.c src/runtime/rx_cortex_record.c \
	src/sha256.c
ALLEN_HDRS = src/allen/allen_bind.h src/runtime/rx_aien.h src/runtime/rx_world.h \
	src/runtime/rx_cortex.h src/runtime/rx_cortex_record.h src/runtime/aienos_cap.h

# The aienos trees the codec needs, at aienos.lock (same shape as the
# $(AIENOS_CAP_LIB) rule). The stamp records the lock it was made for.
$(ALLEN_STAMP): aienos.lock
	@mkdir -p $(ALLEN_DIR)
	@if [ "$(AIENOS_R7_DIR)" = "$(AIENOS_R7_DEFAULT)" ] && [ ! -f "$(ALLEN_AIENOS_SVC)/continuity_subject.h" ]; then \
		git -C $(AIENOS_LOCK_REPO) cat-file -e $(AIENOS_LOCK)^{commit} 2>/dev/null || { \
			echo "aienos.lock commit $(AIENOS_LOCK) is not in AIENOS_LOCK_REPO=$(AIENOS_LOCK_REPO)"; exit 1; }; \
		mkdir -p "$(AIENOS_R7_DIR)" && \
		git -C $(AIENOS_LOCK_REPO) archive $(AIENOS_LOCK) native/kernel/svc native/kernel/core native/argus native/store \
			| tar -x -C "$(AIENOS_R7_DIR)"; \
	fi
	@test -f "$(ALLEN_AIENOS_SVC)/continuity_subject.h" || { \
		echo "AIENOS_R7_DIR=$(AIENOS_R7_DIR) has no native/kernel/svc/continuity_subject.h (aienos.lock too old?)"; exit 1; }
	@echo "$(AIENOS_LOCK)" > $@

$(ALLEN_BIND_OBJ): src/allen/allen_bind.c $(ALLEN_HDRS) $(ALLEN_STAMP)
	$(CC) $(CFLAGS) $(ALLEN_AIENOS_INC) -c -o $@ src/allen/allen_bind.c

$(ALLEN_MAIN_OBJ): tools/allen.c $(ALLEN_HDRS) src/runtime/rx_omega.h $(ALLEN_STAMP)
	$(CC) $(CFLAGS) $(ALLEN_AIENOS_INC) -c -o $@ tools/allen.c

$(ALLEN_TOOL): $(ALLEN_MAIN_OBJ) $(ALLEN_BIND_OBJ) $$(RX_AIEN_OBJ) $(ALLEN_RX_SRCS) $$(ALLEN_AIENOS_SRCS) $$(AIENOS_CAP_LIB) $(ALLEN_STAMP)
	$(CC) $(CFLAGS) $(ALLEN_AIENOS_INC) -pthread -o $@ $(ALLEN_MAIN_OBJ) $(ALLEN_BIND_OBJ) $(RX_AIEN_OBJ) \
		$(ALLEN_RX_SRCS) $(ALLEN_AIENOS_SRCS) $(AIENOS_CAP_LIB) -lm

allen: $(ALLEN_TOOL)

test-allen: $(ALLEN_TOOL)
	ALLEN_TOOL=$(ALLEN_TOOL) ALLEN_BIND_OBJ=$(ALLEN_BIND_OBJ) ALLEN_MAIN_OBJ=$(ALLEN_MAIN_OBJ) \
		ALLEN_OUT=$(ALLEN_DIR)/run sh tests/allen/run.sh
