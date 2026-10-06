#!/bin/bash
# Window lane evidence guard (omega #315). Source this from a window ladder (cand<N>_ladder.sh):
#   . "$(dirname "$0")/window_lane_guard.sh"   (or from tools/ in the omega worktree)
# It keeps a lane's receipts from being lost when a later lane runs `make clean`
# (tools/m19r_qualify.sh does, and removes build/ with every receipt in build/qual-runs).
# tools/chipwait_campaign.sh (the chipwait lane) sources this file and calls wl_guard_build
# before every campaign; a ladder exports WINDOW_LANE_OUT=<window dir> and archives its R11
# receipts (wl_r11_archive / wl_r11_archive_all) first, or the campaign refuses (exit 2).
#
#   wl_begin    <out> <lane>              declare that <lane> will produce receipts that must be archived
#   wl_archive  <out> <lane> <file> <dir> copy <file> to <dir>/<stem>.<sha256>.<ext> (never overwrites,
#                                         verifies the copy), then mark <lane> archived
#   wl_no_receipt <out> <lane>            mark <lane> archived when it produced no receipt (explicit, logged)
#   wl_r11_archive <out> <build_dir>      the r11 lane: archive the newest R11 receipt written by this run
#                                         into <out>/R11-living/ under its sha256 name
#   wl_r11_archive_all <out> <build_dir>  archive every R11 receipt under <build_dir>/qual-runs
#   wl_guard_build <out|""> <lane> <build_dir>  wl_guard_clean plus: refuse while any R11 receipt in
#                                         <build_dir>/qual-runs is unarchived (chipwait_campaign.sh calls it)
#   wl_guard_clean <out> <lane>           call before any lane that runs `make clean`: returns 2 and says
#                                         which lane while an earlier lane's receipt is still unarchived
#
# State lives in <out>/.lane-guard/{pending,archived}/<lane>. Nothing here deletes or edits an existing file.
wl_begin() { mkdir -p "$1/.lane-guard/pending" "$1/.lane-guard/archived"; date -u +%FT%TZ > "$1/.lane-guard/pending/$2"; }

wl_archive() { # out lane file dir
    local out=$1 lane=$2 f=$3 dir=$4
    [ -s "$f" ] || { echo "wl_archive: $lane has no receipt at $f" >&2; return 1; }
    local sha base stem ext dest
    sha=$(sha256sum "$f" | cut -c1-64); base=$(basename "$f")
    stem=${base%.*}; ext=${base##*.}
    mkdir -p "$dir" "$out/.lane-guard/archived"
    dest="$dir/$stem.$sha.$ext"
    if [ -e "$dest" ]; then
        [ "$(sha256sum "$dest" | cut -c1-64)" = "$sha" ] || { echo "wl_archive: $dest exists with other content" >&2; return 1; }
    else
        cp "$f" "$dest" || return 1
        [ "$(sha256sum "$dest" | cut -c1-64)" = "$sha" ] || { echo "wl_archive: copy of $f does not match" >&2; return 1; }
    fi
    echo "$dest" >> "$out/.lane-guard/archived/$lane"
}

wl_no_receipt() { mkdir -p "$1/.lane-guard/archived"; echo "no receipt produced" >> "$1/.lane-guard/archived/$2"; }

wl_r11_archive() { # out build_dir
    local out=$1 b=$2 f
    f=$(ls -1t "$b"/qual-runs/*/R11/rx_aien_faculty_receipt.json 2>/dev/null | head -1)
    [ -n "$f" ] || { echo "wl_r11_archive: no R11 receipt under $b/qual-runs" >&2; return 1; }
    wl_archive "$out" r11 "$f" "$out/R11-living"
}

wl_guard_clean() { # out lane
    local p lane bad=0
    for p in "$1"/.lane-guard/pending/*; do
        [ -e "$p" ] || continue
        lane=$(basename "$p")
        [ "$lane" = "$2" ] && continue
        if [ ! -s "$1/.lane-guard/archived/$lane" ]; then
            echo "REFUSED: lane $2 runs make clean while lane $lane has an unarchived receipt" >&2; bad=1
        fi
    done
    [ "$bad" = 0 ] || return 2
}

# wl_r11_archive_all <out> <build_dir>: archive EVERY R11 receipt under <build_dir>/qual-runs
# (the ladder lane's R16 rung and the r11 lane both write one) into <out>/R11-living/.
wl_r11_archive_all() { # out build_dir
    local out=$1 b=$2 f n=0
    for f in "$b"/qual-runs/*/R11/rx_aien_faculty_receipt.json; do
        [ -e "$f" ] || continue
        wl_archive "$out" r11 "$f" "$out/R11-living" || return 1; n=$((n + 1))
    done
    [ "$n" -gt 0 ] || { echo "wl_r11_archive_all: no R11 receipt under $b/qual-runs" >&2; return 1; }
}

# wl_guard_build <out|""> <lane> <build_dir>: called by tools/chipwait_campaign.sh (whose
# m19r_qualify.sh runs `make clean`) before anything is cleaned. Returns 2 when
#   * <out> is set and an earlier lane's receipt is unarchived (wl_guard_clean), or
#   * any R11 receipt under <build_dir>/qual-runs has no byte-identical copy under
#     <out>/R11-living/ (or <out> is empty): the clean would destroy R11 evidence.
wl_guard_build() { # out lane build_dir
    local out=$1 lane=$2 b=$3 f sha bad=0
    if [ -n "$out" ]; then wl_guard_clean "$out" "$lane" || bad=1; fi
    for f in "$b"/qual-runs/*/R11/rx_aien_faculty_receipt.json; do
        [ -e "$f" ] || continue
        sha=$(sha256sum "$f" | cut -c1-64)
        if [ -z "$out" ] || [ ! -f "$out/R11-living/rx_aien_faculty_receipt.$sha.json" ]; then
            echo "REFUSED: lane $lane would clean $b while R11 receipt $f (sha256 $sha) is unarchived;" \
                 "set WINDOW_LANE_OUT=<window dir> and run wl_r11_archive_all <window dir> $b first" >&2
            bad=1
        fi
    done
    [ "$bad" = 0 ] || return 2
}
