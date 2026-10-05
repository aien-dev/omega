#!/bin/bash
# Red then green on the chip for the attention divergence fix (omega f8d0aca). One heavy job, never killed.
cd "$(dirname "$0")"
./gpu_attention_test.red > chip-red.log 2>&1; echo "variant=red(569c204 test, 6b940fa kernel) rc=$? $(tail -1 chip-red.log)"
for i in 1 2 3 4 5; do ./gpu_attention_test.green --out chip-green-$i.json > chip-green-$i.log 2>&1; echo "variant=green(f8d0aca) run=$i rc=$? $(tail -1 chip-green-$i.log)"; done
./gpu_attention_test.green --timing --out timing.json > chip-timing.log 2>&1; echo "variant=green timing rc=$? $(tail -1 chip-timing.log)"
