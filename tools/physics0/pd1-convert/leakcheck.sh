#!/bin/sh
# leakcheck.sh FILE...: fail if any learner-visible output carries a forbidden token.
# The forbidden list is the PD-1 label-stripping checklist (physics docs/PD1_MACHINE_LAW_BENCHMARK.md).
set -u
PAT='cache|cached|uncached|coheren|stale|flush|l2|nvidia|nvrm|nvos|gpu|cuda|marker|sem|arm|baseline|fix|variant|oldstream|sleep|m18|world|probe|chipwait|astra|e1|commit|omega|physics|allocation|attr'
rc=0
for f in "$@"; do
  hits=$(tr 'A-Z' 'a-z' < "$f" | grep -cE "\\b($PAT)\\b" || true)
  if [ "$hits" -ne 0 ]; then echo "LEAK: $f has $hits line(s) with a forbidden token"; rc=1; else echo "clean: $f"; fi
done
exit $rc
