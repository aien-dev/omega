#!/usr/bin/env bash
# record_void.sh: record a VOID attempt of EXP-001 that no tool records by itself: a failed build or test of the
# frozen tree at BLINDING_PROTOCOL.md step 7 (stage "tests"). It writes the next void receipt in the one EXP-001
# counter ($TC_EVAL_ROOT/<C_f>/run/bundle, tc_void_lib.sh) and, on the third void of the experiment, the
# INCONCLUSIVE (INFRA) final receipt.
#
#   record_void.sh --commit C_F --step STEP --code CODE --reason TEXT --command TEXT [--repo DIR]
#
# The profile digest and candidate manifest digest are read from C_f through git. A retry of step 7 must repeat
# the command of the first "tests" void byte for byte; record_void.sh refuses a different one. Refuses (exit 2,
# nothing written) when EXP-001 has already ended.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=tc_void_lib.sh
. "$here/tc_void_lib.sh"
die() { echo "record_void: REFUSED: $*" >&2; exit 2; }
repo="$(git -C "$here" rev-parse --show-toplevel)"
. "$here/tc_exp.sh"
cf="" step="" code="" reason="" command=""
while [ $# -gt 0 ]; do
    case "$1" in
    --commit) cf="$2"; shift 2 ;;
    --step) step="$2"; shift 2 ;;
    --code) code="$2"; shift 2 ;;
    --reason) reason="$2"; shift 2 ;;
    --command) command="$2"; shift 2 ;;
    --repo) repo="$2"; shift 2 ;;
    *) die "unknown option '$1'" ;;
    esac
done
[[ "$cf" =~ ^[0-9a-f]{40}$ ]] || die "--commit must be the 40-hex freeze commit C_f"
[ -n "$step" ] && [ -n "$code" ] && [ -n "$reason" ] && [ -n "$command" ] || die "--step --code --reason --command are required"
pd="$(git -C "$repo" show "$cf:$EXP_PROFILE" 2>/dev/null | sha256sum | cut -c1-64)"
cm="$(git -C "$repo" show "$cf:$EXP_DIR/candidate_manifest.json" 2>/dev/null | sha256sum | cut -c1-64)"
git -C "$repo" cat-file -e "$cf:$EXP_DIR/candidate_manifest.json" 2>/dev/null || die "no candidate manifest at $cf"
dir="$(tc_void_dir "$cf")"
tc_void_ended "$dir" && die "$EXP_ID has ended ($dir holds a final receipt or three void receipts)"
first="$(tc_void_first_command "$dir" tests)"
[ -z "$first" ] || [ "$first" = "$(tc_jclean "$command")" ] || die "retry differs from the first tests void ($first)"
n="$(tc_void_write "$cf" tests "$step" "$code" "$reason" "$command" "frozen tree $cf" "$pd" "$cm")"
echo "record_void: void attempt $n of $EXP_ID written to $dir/void_receipt_$n.json" >&2
[ "$n" -lt "$TC_MAX_ATTEMPTS" ] || echo "record_void: third void attempt: $EXP_ID is INCONCLUSIVE (INFRA), final_receipt.json written" >&2
