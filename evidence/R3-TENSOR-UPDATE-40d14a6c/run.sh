#!/bin/sh
# R3 chip run: green --update over the five training weight shapes (m = 16), then the default sweep as a
# regression check. Run under quietlock.
B=/tmp/e2-scratch/r3/gpu_matmul_api_test.green
D=/tmp/e2-scratch/r3
OMEGA_GPU_MATMUL_SHAPES="16,768,2304;16,768,768;16,768,3072;16,3072,768;16,768,50304" timeout 300 $B --update --out $D/green-chip-update.json > $D/green-chip-update.txt 2>&1; echo "exit=$?" >> $D/green-chip-update.txt
timeout 300 $B --out $D/green-chip-sweep.json > $D/green-chip-sweep.txt 2>&1; echo "exit=$?" >> $D/green-chip-sweep.txt
