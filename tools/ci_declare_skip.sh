#!/bin/sh
# Declare, in CI, a check that cannot run on a GitHub runner.
#
# Usage: tools/ci_declare_skip.sh <make-target> <class> <reason>
#   class: chip-only  (needs the GB10 resident seat / graphics processor)
#          spark-only (needs the DGX Spark itself, e.g. its X925 + A725 cores)
#
# Prints one SKIP line and appends it to $GITHUB_STEP_SUMMARY when set. A
# declared SKIP is never a pass: the script refuses any text containing PASS,
# so a SKIP line can never be read (or grepped) as a pass. Exit 0 = declared.
set -eu
if [ $# -ne 3 ]; then
    echo "usage: $0 <make-target> chip-only|spark-only <reason>" >&2
    exit 2
fi
target=$1; class=$2; reason=$3
case "$class" in chip-only|spark-only) ;; *) echo "bad class: $class" >&2; exit 2 ;; esac
case "$target $reason" in
    *PASS*) echo "refused: a SKIP declaration must not say pass: $target $reason" >&2; exit 2 ;;
esac
grep -Eq "^$target:" Makefile mk/*.mk 2>/dev/null || {
    echo "refused: no make target named $target" >&2; exit 2; }
line="SKIP $class: $target: $reason"
echo "$line"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    echo "- \`$line\`" >> "$GITHUB_STEP_SUMMARY"
fi
