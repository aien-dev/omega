#!/bin/sh
# Operating-characteristics table for est-v6 section 6 (appendix est-v6-appendix-prefreeze.md).
# usage: est6opchar_table.sh <est6opchar binary> [reps]
# Reads no data file. Clustering settings: w 0.08 (design effect about 3.3), 0.12 (about 4.5),
# 0.20 (about 7); rho 0.02. Two 2700 s runs pooled (H1 + H2); three runs for comparison.
set -eu
B=${1:?usage: est6opchar_table.sh BIN [reps]}
R=${2:-4000}
for w in 0.08 0.12 0.20; do
  for runs in 2 3; do
    for c in a b; do
      "$B" run --case $c --w $w --rho 0.02 --runs $runs --reps "$R" --boot 1000 | tail -n 2 | paste -sd' ' | sed "s/^/w=$w runs=$runs /"
    done
  done
done
for w in 0.08 0.12 0.20; do
  for b0 in 0.100 0.090 0.080 0.076 0.070 0.065 0.060; do
    "$B" run --case c --bin0 $b0 --w $w --rho 0.02 --runs 2 --reps "$R" --boot 1000 | tail -n 2 | paste -sd' ' | sed "s/^/w=$w /"
  done
done
