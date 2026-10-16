#!/bin/sh
# repeats 1 and 2 of the green --update run (timing stability)
B=/tmp/e2-scratch/r3/gpu_matmul_api_test.green
D=/tmp/e2-scratch/r3
for r in 1 2; do
OMEGA_GPU_MATMUL_SHAPES="16,768,2304;16,768,768;16,768,3072;16,3072,768;16,768,50304" timeout 300 $B --update --out $D/green-chip-update-rep$r.json > $D/green-chip-update-rep$r.txt 2>&1; echo "exit=$?" >> $D/green-chip-update-rep$r.txt
done
