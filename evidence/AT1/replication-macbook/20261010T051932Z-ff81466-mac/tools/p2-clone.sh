#!/bin/bash
set -euo pipefail
export LC_ALL=C
FREEZE=ff814662a19cf7da6b183f3e667c6c921e0ead21
W=$HOME/at1-replication/run-ff814662a19c
mkdir -p "$W"; cd "$W"
LOG=$W/receipt-commands.log
run() { echo "\$ $*" >> "$LOG"; set +e; "$@" >> "$W/cmd.out" 2>&1; rc=$?; set -e; echo "exit $rc" >> "$LOG"; return $rc; }
if [ ! -d omega/.git ]; then
  run git init -q omega
  run git -C omega remote add origin https://github.com/aien-dev/omega.git
  run git -C omega fetch -q --depth 1 origin $FREEZE
  run git -C omega checkout -q --detach FETCH_HEAD
fi
git -C omega rev-parse HEAD
git -C omega rev-list --count HEAD
git -C omega status --porcelain | wc -l
ls omega/mk/at1.mk omega/research/atemporal/at1/
