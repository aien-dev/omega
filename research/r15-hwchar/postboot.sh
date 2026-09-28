#!/bin/bash
# postboot.sh -- the reboot-and-repeat after Drake's full power-off
# (2026-09-28 17:33:53, boot 17:34:39). Runs run.sh and burst.sh with the SPBM
# reader NOT loaded (as the machine booted), then loads the owner-key signed
# reader and repeats burst.sh, so the result is comparable with the afternoon
# runs (reader loaded) and also shows whether the reader matters.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
R=$HOME/workspace/r15-practice
KO=$HOME/workspace/omega-r15/research/m15/spbm/aien_spbm_readonly.ko
{
    echo "== boot"; journalctl --list-boots --no-pager 2>/dev/null | tail -3
    echo "== rtc"; journalctl -b 0 -k --no-pager 2>/dev/null | grep -m2 "rtc-efi"
    echo "== spbm module at start"; lsmod | grep -i spbm || echo "not loaded"
    echo "== NV_ERR_TIMEOUT since boot: $(journalctl -b 0 -k --no-pager 2>/dev/null | grep -c NV_ERR_TIMEOUT)"
    echo "== uptime"; uptime
} > "$R/postboot-boot-facts.txt" 2>&1
"$HERE/run.sh" "$R/hwchar-postboot-unloaded"
"$HERE/burst.sh" "$R/hwchar-burst-postboot-unloaded"
if sudo -n insmod "$KO"; then
    echo "$(date +%T) insmod ok: $(modinfo -F srcversion "$KO")" >> "$R/postboot-boot-facts.txt"
else
    echo "$(date +%T) insmod FAILED" >> "$R/postboot-boot-facts.txt"
fi
sleep 5
"$HERE/burst.sh" "$R/hwchar-burst-postboot-loaded"
echo "$(date +%T) postboot sequence finished" >> "$R/postboot-boot-facts.txt"
