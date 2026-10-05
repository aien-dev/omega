#!/bin/sh
# G15 diagnostic in the reducer (tools/r15_reduce.c, m10_gpu_residency.worst_process_diagnostic).
# Uses the real CAND-1 window-2 trial-SEQ-06 raw file (16348 of 19253 samples live),
# plus copies whose residency record is extended with sampler diag fields.
# Checks: the verdict is untouched (G15 FAIL, value 0.849114424) in every case; old
# evidence says UNMEASURED and infers nothing; a long not-live run is reported as
# seat_stopped_long_run; scattered losses with a late sampler are reported as such.
# Needs build/r15_reduce (make build/r15_reduce).
cd "$(dirname "$0")/.." || exit 1
RED=${R15_REDUCE:-build/r15_reduce}; SRC=evidence/CAND1-WINDOW2-20261005T1259Z/R15-silicon/raw/trial-SEQ-06.jsonl
[ -x "$RED" ] && [ -f "$SRC" ] || { echo "need $RED and $SRC"; exit 2; }
W=$(mktemp -d) || exit 1; trap 'rm -rf "$W"' EXIT; fail=0
run() { # name diag-json-or-empty expected-class-text expected-extra-text
    mkdir "$W/$1"
    if [ -n "$2" ]; then sed "s|\"claims_max\":1}}|\"claims_max\":1,\"diag\":$2}}|" "$SRC" > "$W/$1/trial-SEQ-06.jsonl"
    else cp "$SRC" "$W/$1/trial-SEQ-06.jsonl"; fi
    ( cd "$W/$1" && sha256sum trial-SEQ-06.jsonl > SHA256SUMS )
    "$RED" "$W/$1" "$W/$1/s.json" >/dev/null 2>&1
    s=$(tr -d '\n' < "$W/$1/s.json")
    if printf '%s' "$s" | grep -Fq "$3" && printf '%s' "$s" | grep -Fq "$4" \
       && printf '%s' "$s" | grep -Fq '"id":"G15","criterion":"GPU residency","threshold":">= 99% of samples live","value":0.849114424,"outcome":"FAIL"'; then
        echo "[+] $1"
    else echo "[-] $1: $(printf '%s' "$s" | grep -o '"worst_process_diagnostic":{[^}]*}')"; fail=$((fail + 1)); fi
}
run old_evidence "" '"class":"UNMEASURED"' '"lost_samples":2905'
run seat_stopped '{"late_2ms":0,"late_10ms":0,"max_gap_ns":1100000,"dead_runs":1,"dead_longest":2905,"dead_at_end":2905,"t_first":1000,"t_last":20000000000,"first_dead_t":17000001000,"last_live_t":16999000000,"stale_max_ns":3000000000}' \
    '"class":"seat_stopped_long_run"' '"sampler_late_observed":false'
run scattered_late_sampler '{"late_2ms":400,"late_10ms":60,"max_gap_ns":80000000,"dead_runs":2905,"dead_longest":1,"dead_at_end":0,"t_first":1000,"t_last":20000000000,"first_dead_t":5000000,"last_live_t":19999000000,"stale_max_ns":2000000}' \
    '"class":"scattered_not_live"' '"sampler_late_observed":true'
# xid-scan: a GPU fault must be reported, none must read as none, and an unreadable
# log must never read as none (window 2's dmesg-based count did exactly that).
T=tools/r15_machine_state.sh
xcheck() { # name expected-exit expected-text
    out=$(R15_KLOG_FILE="$W/$1" "$T" xid-scan 0 1 2>&1); rc=$?
    if [ "$rc" = "$2" ] && printf '%s\n' "$out" | grep -Fq -- "$3"; then echo "[+] xid-scan $1"
    else echo "[-] xid-scan $1: exit $rc: $out"; fail=$((fail + 1)); fi
}
printf '%s\n' '2026-10-05T08:08:16-05:00 spark kernel: NVRM: Xid (PCI:000f:01:00): 109, pid=2050644, name=rx_r15_perf_sil, channel 0x0000000a, errorString CTX SWITCH TIMEOUT, Info 0x3c002' \
    '2026-10-05T08:08:17-05:00 spark kernel: audit: unrelated' > "$W/with_xid"
: > "$W/none"
xcheck with_xid 0 "xid lines: 1"
xcheck none 0 "xid lines: 0"
xcheck missing_file 4 "XID_LOG_UNREADABLE"
echo "failures $fail"; [ "$fail" = 0 ]
