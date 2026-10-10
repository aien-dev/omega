#!/bin/bash
# AT-1 Agent 7 phase 2, third at1-check run. Two recorded substitutions:
#  1. sh = /bin/dash via a PATH shim (macOS /bin/sh is bash 3.2.57, which rejects run.sh line 338:
#     a case statement inside $( ) ; the Spark's /bin/sh is dash).
#  2. CC = cc-nosan (refuses -fsanitize; Apple's ASan runtime hangs at startup on macOS 26.6.2).
# Runs 1 and 2 are left as they are (run 1 still hung in the ASan probe, never killed).
export LC_ALL=C
W=$HOME/at1-replication/run-ff814662a19c
LOG=$W/receipt-commands.log
cd "$W/omega"
echo "\$ PATH=$W/shwrap:\$PATH AT1_ARCH_DIR=$W/arch CC=$W/ccwrap/cc-nosan make at1-check AT1_OUT=build/at1-dash   (started $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$LOG"
PATH=$W/shwrap:$PATH AT1_ARCH_DIR=$W/arch CC=$W/ccwrap/cc-nosan make at1-check AT1_OUT=build/at1-dash > "$W/at1-check3.console.log" 2>&1
rc=$?
echo "exit $rc   (finished $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$LOG"
echo "$rc" > "$W/at1-check3.exit"
