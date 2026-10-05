#!/bin/sh
# receipt.sh OUT_DIR [quiet] -- build OUT_DIR/LG-receipt.json from the logs
# the proof targets wrote (model.log, diff.log, tsan.log). Every field is read
# from a log or from git; nothing is assumed. A missing log is NOT_RUN.
# Evidence class: host (CPU only; no QEMU, no hardware chip).
set -u
out=$1
mode=${2:-run}
rcpt=$out/LG-receipt.json

commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)
if [ -n "$(git status --porcelain 2>/dev/null)" ]; then dirty=true; else dirty=false; fi
cc_ver=$(${CC:-gcc} --version 2>/dev/null | head -n1)
host=$(uname -srm)
now=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# get KEY: value of the last "RESULT KEY=..." line in any log, else empty.
get() {
	cat "$out/model.log" "$out/diff.log" "$out/tsan.log" 2>/dev/null |
		sed -n "s/^RESULT $1=//p" | tail -n1
}
# tsan values come only from tsan.log (the tsan build prints the same keys).
get_tsan() { sed -n "s/^RESULT $1=//p" "$out/tsan.log" 2>/dev/null | tail -n1; }
get_main() {
	cat "$out/model.log" "$out/diff.log" 2>/dev/null | sed -n "s/^RESULT $1=//p" | tail -n1
}
esc() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
or_nr() { if [ -z "$1" ]; then echo NOT_RUN; else echo "$1"; fi; }

if [ "$mode" = quiet ]; then
	cat > "$rcpt" <<EOF
{
  "lane": "LG",
  "evidence": "host",
  "commit": "$commit",
  "tree_dirty": $dirty,
  "generated": "$now",
  "verdict": "NOT_RUN",
  "reason": "quiet flag set; nothing was built or run"
}
EOF
	echo "receipt: $rcpt (NOT_RUN)"
	exit 0
fi

model_v=$(or_nr "$(get_main model.verdict)")
diff_v=$(or_nr "$(get_main diff.verdict)")
wd_v=$(or_nr "$(get_main world_diff.verdict)")
tsan_v=$(get_tsan tsan)
case "$tsan_v" in "") tsan_v=NOT_RUN ;; esac

# Per invariant, from the observed results only.
inv() { # name spec_key asbuilt_key mutant_key real_ok(yes/no/known)
	s=$(get_main "model.SPEC.$2"); a=$(get_main "model.AS_BUILT.$2"); k=$(get_main "$3")
	r=$4
	if [ -z "$s" ] || [ -z "$a" ] || [ -z "$k" ]; then echo NOT_RUN; return; fi
	if [ "$s" != PASS ] || [ "$k" != KILLED ]; then echo FAIL; return; fi
	if [ "$a" = PASS ] && [ "$r" = yes ]; then echo PASS; return; fi
	if [ "$a" = KNOWN_FAIL ] && [ "$r" = known ]; then echo KNOWN_FAIL; return; fi
	echo FAIL
}
z() { [ "$(get_main "$1")" = 0 ]; }
if [ "$diff_v" = PASS ] && z real.I1.settled_leak_violations && z real.I1.quiescent_leak_violations &&
   [ "$(get_main diff.DIFF_MUT_LEAK_ON_FAIL)" = KILLED ] && [ "$(get_main shim.real_leak_check)" = KILLED ]; then
	r1=yes; else r1=no; fi
if [ "$diff_v" = PASS ] && z real.I3.duplicate_commit_runs &&
   [ "$(get_main diff.DIFF_MUT_COMMIT_KEEPS_ACTIVATION)" = KILLED ] &&
   [ "$(get_main shim.commit_unique_check)" = KILLED ]; then r3=yes; else r3=no; fi
if [ "$(get_main real.I2.directed)" = KNOWN_FAIL ]; then r2=known; else r2=no; fi
if [ "$(get_main real.I4.directed)" = KNOWN_FAIL ]; then r4=known; else r4=no; fi
i1=$(inv I1 I1_no_budget_leak model.MUT_LEAK_ON_FAIL.I1_no_budget_leak "$r1")
i2=$(inv I2 I2_cancel_reaches_children model.MUT_CANCEL_NO_PROPAGATE.I2_cancel_reaches_children "$r2")
i3=$(inv I3 I3_no_double_commit model.MUT_COMMIT_KEEPS_ACTIVATION.I3_no_double_commit "$r3")
i4=$(inv I4 I4_deadline_overrun_surfaced model.MUT_DEADLINE_ADMIT_ONLY.I4_deadline_overrun_surfaced "$r4")

overall=PASS
for v in "$model_v" "$wd_v"; do [ "$v" = PASS ] || overall=FAIL; done
for v in "$i1" "$i2" "$i3" "$i4"; do case "$v" in PASS|KNOWN_FAIL) ;; *) overall=FAIL ;; esac; done
case "$tsan_v" in PASS|NOT_RUN*) ;; *) overall=FAIL ;; esac
if [ "$overall" = PASS ]; then for v in "$i1" "$i2" "$i3" "$i4"; do [ "$v" = KNOWN_FAIL ] && overall=PASS_WITH_KNOWN_FAIL; done; fi

{
	echo "{"
	echo "  \"lane\": \"LG\","
	echo "  \"evidence\": \"host\","
	echo "  \"commit\": \"$commit\","
	echo "  \"tree_dirty\": $dirty,"
	echo "  \"generated\": \"$now\","
	echo "  \"host\": \"$(esc "$host")\","
	echo "  \"compiler\": \"$(esc "$cc_ver")\","
	echo "  \"verdict\": \"$overall\","
	echo "  \"model_check\": \"$model_v\","
	echo "  \"differential\": \"$diff_v\","
	echo "  \"tsan\": \"$(esc "$tsan_v")\","
	echo "  \"invariants\": {"
	echo "    \"I1_no_budget_leak\": \"$i1\","
	echo "    \"I2_cancel_reaches_children\": \"$i2\","
	echo "    \"I3_no_double_commit\": \"$i3\","
	echo "    \"I4_deadline_overrun_surfaced\": \"$i4\""
	echo "  },"
	echo "  \"findings\": {"
	echo "    \"I2_counterexample\": \"$(esc "$(get_main real.I2.counterexample)")\","
	echo "    \"I2_children_in_flight_after_cancel\": \"$(esc "$(get_main real.I2.children_in_flight_after_cancel)")\","
	echo "    \"I2_child_commit_after_cancel\": \"$(get_main real.I2.child_commit_after_cancel)\","
	echo "    \"I4_counterexample\": \"$(esc "$(get_main real.I4.counterexample)")\","
	echo "    \"I4_spec_overdue\": \"$(get_main real.I4.spec_overdue)\","
	echo "    \"I4_real_deadline_overdue\": \"$(get_main real.I4.real_deadline_overdue)\","
	echo "    \"I4_late_commit_published\": \"$(get_main real.I4.late_commit_published)\""
	echo "  },"
	echo "  \"results\": {"
	first=1
	for f in model.log diff.log; do
		[ -f "$out/$f" ] || continue
		sed -n 's/^RESULT \([^=]*\)=\(.*\)$/\1\t\2/p' "$out/$f"
	done | while IFS="	" read -r k v; do
		if [ $first = 1 ]; then first=0; else printf ',\n'; fi
		printf '    "%s": "%s"' "$(esc "$k")" "$(esc "$v")"
	done
	echo
	echo "  },"
	echo "  \"tsan_results\": {"
	if [ -f "$out/tsan.log" ]; then
		sed -n 's/^RESULT \(tsan[^=]*\|stress[^=]*\|world_diff.verdict\)=\(.*\)$/\1\t\2/p' "$out/tsan.log" |
		awk -F'\t' 'BEGIN{n=0} {gsub(/\\/,"\\\\",$2); gsub(/"/,"\\\"",$2); printf "%s    \"%s\": \"%s\"", (n++?",\n":""), $1, $2} END{print ""}'
	fi
	echo "  }"
	echo "}"
} > "$rcpt"
echo "receipt: $rcpt"
echo "LG proof: $overall (I1=$i1 I2=$i2 I3=$i3 I4=$i4 model=$model_v diff=$diff_v tsan=$tsan_v)"
case "$overall" in PASS|PASS_WITH_KNOWN_FAIL) exit 0 ;; *) exit 1 ;; esac
