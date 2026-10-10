#!/bin/bash
# AT-1 Agent 7 phase 2: run make at1-check on the MacBook (detached; never killed)
export LC_ALL=C
W=$HOME/at1-replication/run-ff814662a19c
cd "$W"
LOG=$W/receipt-commands.log
if [ ! -d arch/.git ]; then
  echo "\$ git clone -q https://github.com/aien-dev/aien-architecture.git arch" >> "$LOG"
  git clone -q https://github.com/aien-dev/aien-architecture.git arch; echo "exit $?" >> "$LOG"
fi
echo "arch HEAD $(git -C arch rev-parse HEAD)" >> "$LOG"
cd omega
echo "\$ AT1_ARCH_DIR=$W/arch make at1-check   (started $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$LOG"
AT1_ARCH_DIR=$W/arch make at1-check > "$W/at1-check.console.log" 2>&1
rc=$?
echo "exit $rc   (finished $(date -u +%Y-%m-%dT%H:%M:%SZ))" >> "$LOG"
echo "$rc" > "$W/at1-check.exit"
