# HD-13: the Spark quiet flag as a real lock.
#
# Included by the top-level Makefile through `-include mk/*.mk`. Every make
# goal in omega first runs `quietlock check` while the Makefile is being read,
# so a build or test started by anyone (agent, script, human) stops with the
# marker QUIETLOCK_REFUSED and the holder's name while someone else holds the
# quiet flag. (make itself then exits 2; `quietlock run -- make ...` exits 75.)
# The holder runs its own commands as
#     quietlock hold --owner ID --minutes N -- make <goal>
# which sets QUIETLOCK_HOLD so its own make passes.
#
# This is a real entry-point check, not text matching. Rules:
# - `make quietlock` (or `make $(OUT_DIR)/quietlock`) builds the tool without
#   the check (bootstrap).
# - Any other goal builds the tool on demand (missing or older than its
#   source) into a temp file and renames it into place, so parallel makes never
#   run a half-written binary.
# - GitHub CI / another machine: no state dir means clear, so CI is unaffected.
#   If the tool cannot be built at all: continue with a NOTICE when no quiet
#   flag exists, refuse when one does.
# - No make variable skips the check. The QUIETLOCK_DIR environment variable
#   moves the state dir (tests need it); this is a cooperative lock against
#   accidents, not a security boundary.
QUIETLOCK_SRC := tools/quietlock/quietlock.c
QUIETLOCK_BIN := $(OUT_DIR)/quietlock
QUIETLOCK_CFLAGS := -std=gnu11 -Wall -Wextra -Werror -O2 -D_GNU_SOURCE
QUIETLOCK_BOOT_GOALS := quietlock $(QUIETLOCK_BIN)

QUIETLOCK_NEED_CHECK :=
ifeq ($(strip $(MAKECMDGOALS)),)
QUIETLOCK_NEED_CHECK := 1
endif
ifneq ($(strip $(filter-out $(QUIETLOCK_BOOT_GOALS),$(MAKECMDGOALS))),)
QUIETLOCK_NEED_CHECK := 1
endif

ifeq ($(QUIETLOCK_NEED_CHECK),1)
QUIETLOCK_STATUS := $(shell ql='$(QUIETLOCK_BIN)'; \
	if [ ! -x "$$ql" ] || [ '$(QUIETLOCK_SRC)' -nt "$$ql" ]; then \
		{ mkdir -p '$(OUT_DIR)' && $(CC) $(QUIETLOCK_CFLAGS) -o "$$ql.tmp.$$$$" '$(QUIETLOCK_SRC)' && \
		  mv -f "$$ql.tmp.$$$$" "$$ql"; } >&2 || rm -f "$$ql.tmp.$$$$"; \
	fi; \
	if [ -x "$$ql" ]; then "$$ql" check >&2; echo $$?; \
	elif [ -e "$${QUIETLOCK_DIR:-$$HOME/workspace}/.spark-quiet" ]; then \
		echo "quietlock: QUIETLOCK_REFUSED the quietlock tool could not be built and a quiet flag exists" >&2; echo 75; \
	else echo "quietlock: NOTICE the quietlock tool could not be built; no quiet flag exists, continuing" >&2; echo 0; fi)
ifneq ($(QUIETLOCK_STATUS),0)
$(error QUIETLOCK_REFUSED this make run (status $(QUIETLOCK_STATUS)); see the message above. Wait for the quiet flag to clear, or run under 'quietlock hold')
endif
endif

.PHONY: quietlock quietlock-check
quietlock: $(QUIETLOCK_BIN)

$(QUIETLOCK_BIN): $(QUIETLOCK_SRC)
	mkdir -p $(dir $@)
	$(CC) $(QUIETLOCK_CFLAGS) -o $@.tmp.$$$$ $< && mv -f $@.tmp.$$$$ $@

# Cheap goal that does nothing but pass the check (used by tests/quietlock).
quietlock-check:
	@echo "quietlock: clear for this make run"
