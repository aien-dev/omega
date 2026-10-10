#!/bin/bash
# AT-1 Agent 7 phase 2, second at1-check run: same entry, CC=cc-nosan (sanitizer refused), separate out dir.
# The first run (build/at1/check) is left running untouched: its ASan probe hangs and is never killed.
export LC_ALL=C
W=$HOME/at1-replication/run-ff814662a19c
LOG=$W/receipt-commands.log
cd "$W/omega"
echo "\$ AT1_ARCH_DIR=$W/arch CC=$W/ccwrap/cc-nosan make at1-check AT1_OUT=build/at1-nosan   (started $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$LOG"
AT1_ARCH_DIR=$W/arch CC=$W/ccwrap/cc-nosan make at1-check AT1_OUT=build/at1-nosan > "$W/at1-check2.console.log" 2>&1
rc=$?
echo "exit $rc   (finished $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$LOG"
echo "$rc" > "$W/at1-check2.exit"
