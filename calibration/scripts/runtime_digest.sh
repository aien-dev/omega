#!/bin/sh
# runtime_digest.sh: the runtime_digest of Turing-profile-v1.0 (rule turing.cal.runtime.v1).
#
#   sh calibration/scripts/runtime_digest.sh [--lines]
#
# The evaluator runtime is exactly these five binaries, built from the freeze commit (the crumbs binary is
# prebuilt and pinned, never rebuilt):
#   crumbline-learner       build/crumbline-learner                          (make crumbline-learner)
#   crumbs                  ~/workspace/hive-worktrees/crumbs-v1/target/release/crumbs (or $TC_CRUMBS)
#   turing-cal-candidates   build/turing-exp001-a/turing-cal-candidates      (make turing-exp001-a-build)
#   turing-cal-overlap      build/turing-cal/turing-cal-overlap              (make turing-cal-overlap)
#   turing-coder            build/tests-turing-exp001-b/turing-coder         (make turing-coder)
# Listing = one line per binary, '<sha256 of the file, 64 lowercase hex>  <name>' + LF, sorted by name
# (byte order), exactly as above. runtime_digest = SHA-256 of the listing bytes, 64 lowercase hex.
# --lines prints the listing instead of the digest. Refuses if any binary is missing, or if crumbs or the
# learner differ from the pins in calibration/scripts/generate_sealed_data.sh.
# Computed at freeze time only; the value goes into the profile before the sidecar is written.
# No Python: POSIX sh + sha256sum + sort.
set -eu
dir=$(cd "$(dirname "$0")/../.." && pwd)
out="${OUT_DIR:-$dir/build}"
crumbs="${TC_CRUMBS:-$HOME/workspace/hive-worktrees/crumbs-v1/target/release/crumbs}"
die() { echo "runtime_digest: REFUSED: $*" >&2; exit 1; }
pin() { grep "^$1=" "$dir/calibration/scripts/generate_sealed_data.sh" | head -1 | grep -o "[0-9a-f]\{64\}"; }
sha() { sha256sum "$1" | cut -c1-64; }
list=$(
    for pair in "crumbline-learner:$out/crumbline-learner" "crumbs:$crumbs" \
        "turing-cal-candidates:$out/turing-exp001-a/turing-cal-candidates" \
        "turing-cal-overlap:$out/turing-cal/turing-cal-overlap" \
        "turing-coder:$out/tests-turing-exp001-b/turing-coder"; do
        n=${pair%%:*}; p=${pair#*:}
        [ -f "$p" ] || die "$n missing at $p"
        printf '%s  %s\n' "$(sha "$p")" "$n"
    done | LC_ALL=C sort -k2
)
cs=$(printf '%s\n' "$list" | sed -n 's/  crumbs$//p')
ls_=$(printf '%s\n' "$list" | sed -n 's/  crumbline-learner$//p')
[ "$cs" = "$(pin CRUMBS_SHA256)" ] || die "crumbs $cs != pinned $(pin CRUMBS_SHA256)"
[ "$ls_" = "$(pin LEARNER_SHA256)" ] || die "learner $ls_ != pinned $(pin LEARNER_SHA256)"
if [ "${1:-}" = "--lines" ]; then printf '%s\n' "$list"; else printf '%s\n' "$list" | sha256sum | cut -c1-64; fi
