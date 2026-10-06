#!/bin/bash
# Window lane evidence guard (omega #315). Source this from a window ladder (cand<N>_ladder.sh):
#   . "$(dirname "$0")/window_lane_guard.sh"   (or from tools/ in the omega worktree)
# It keeps a lane's receipts from being lost when a later lane runs `make clean`
# (tools/m19r_qualify.sh does, and removes build/ with every receipt in build/qual-runs).
#
#   wl_begin    <out> <lane>              declare that <lane> will produce receipts that must be archived
#   wl_archive  <out> <lane> <file> <dir> copy <file> to <dir>/<stem>.<sha256>.<ext> (never overwrites,
#                                         verifies the copy), then mark <lane> archived
#   wl_no_receipt <out> <lane>            mark <lane> archived when it produced no receipt (explicit, logged)
#   wl_r11_archive <out> <build_dir>      the r11 lane: archive the newest R11 receipt written by this run
#                                         into <out>/R11-living/ under its sha256 name
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
