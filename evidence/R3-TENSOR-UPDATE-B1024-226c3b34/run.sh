#!/bin/sh
# #483 re-run at the default CTA budget 1024 (after #481): --update over the five training weight shapes, 3 runs,
# then the default sweep as a regression check. Run under quietlock.
B=/tmp/e2-scratch/r3c/api_test
D=/tmp/e2-scratch/r3c
for r in 0 1 2; do
OMEGA_GPU_MATMUL_SHAPES="16,768,2304;16,768,768;16,768,3072;16,3072,768;16,768,50304" timeout 300 $B --update --out $D/update-b1024-run$r.json > $D/update-b1024-run$r.txt 2>&1; echo "exit=$?" >> $D/update-b1024-run$r.txt
done
timeout 300 $B --out $D/sweep-b1024.json > $D/sweep-b1024.txt 2>&1; echo "exit=$?" >> $D/sweep-b1024.txt
