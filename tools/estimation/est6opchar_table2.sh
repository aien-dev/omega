#!/bin/sh
# SIMULATION table 2 for est-v6 section 6 (appendix est-v6-appendix-prefreeze.md section 5b). Reads no data file.
# usage: est6opchar_table2.sh <est6opchar binary> [reps]
# Adds to table 1: (d) a forecaster with the measured regime-dependent PIT (case d), a replication of (a) and (b)
# under another seed, and the false-accept rate (PASS) of forecasters whose true bin 0 breaks the 0.07 limit,
# with three runs for comparison. All rows: rho 0.02, boot 1000.
set -eu
B=${1:?usage: est6opchar_table2.sh BIN [reps]}
R=${2:-20000}
row() { "$B" run "$@" --rho 0.02 --reps "$R" --boot 1000 | tail -n 2 | paste -sd' '; }
echo "# (d) regime-dependent PIT (v5 D1 per-level shares), runs 2 and 3"
for w in 0.08 0.12; do for runs in 2 3; do row --case d --w $w --runs $runs; done; done
echo "# replication of (a) and (b) under seed 0xE6A777 (table 1 used 0xE6A001)"
for c in a b; do for runs in 2 3; do row --case $c --w 0.08 --runs $runs --seed 0xE6A777; done; done
echo "# false accept: true bin 0 below the 0.07 limit, runs 2 and 3, w 0.08"
for b0 in 0.070 0.068 0.065 0.060 0.055; do for runs in 2 3; do row --case c --bin0 $b0 --w 0.08 --runs $runs; done; done
