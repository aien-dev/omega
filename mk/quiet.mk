# HD-13: the Spark quiet flag as a real lock.
#
# Included by the top-level Makefile through `-include mk/*.mk`. Every make
# goal in omega first runs `quietlock check` while the Makefile is being read,
# so a build or test started by anyone (agent, script, human) stops with
# exit status 2 and the holder's name while someone else holds the quiet flag.
# The holder runs its own commands as
#     quietlock hold --owner ID --minutes N -- make <goal>
# which sets QUIETLOCK_HOLD so its own make passes.
#
# Bootstrap: `make quietlock` (or `make $(OUT_DIR)/quietlock`) builds the tool
# without the check. For any other goal the tool is built here on demand
# (missing or older than its source) and a build failure also stops make
# (fails closed). There is deliberately no make variable that skips the check.
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
QUIETLOCK_STATUS := $(shell { { [ -x '$(QUIETLOCK_BIN)' ] && [ ! '$(QUIETLOCK_SRC)' -nt '$(QUIETLOCK_BIN)' ]; } || { mkdir -p '$(OUT_DIR)' && $(CC) $(QUIETLOCK_CFLAGS) -o '$(QUIETLOCK_BIN)' '$(QUIETLOCK_SRC)'; } >&2; } && '$(QUIETLOCK_BIN)' check >&2; echo $$?)
ifneq ($(QUIETLOCK_STATUS),0)
$(error quietlock refused this make run (status $(QUIETLOCK_STATUS)); see the message above. Wait for the quiet flag to clear, or run under 'quietlock hold')
endif
endif

.PHONY: quietlock quietlock-check
quietlock: $(QUIETLOCK_BIN)

$(QUIETLOCK_BIN): $(QUIETLOCK_SRC)
	mkdir -p $(dir $@)
	$(CC) $(QUIETLOCK_CFLAGS) -o $@ $<

# Cheap goal that does nothing but pass the check (used by tests/quietlock).
quietlock-check:
	@echo "quietlock: clear for this make run"
