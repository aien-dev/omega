# GB10 CPU PMU attribution probe (R15 clarification C1, item 8)

`pmu_probe.c` opens six events per task with `inherit = 1` on both CPU PMUs
(type 10 = Cortex-A725, type 11 = Cortex-X925), then a thread created after
the counters runs a fixed loop on cpu 1 (A725) and the same loop on cpu 6
(X925). It changes no machine setting. It was run once as root on
2026-09-28 so that `kernel.perf_event_paranoid` could stay at 4.

    gcc -O1 -pthread -o pmu_probe pmu_probe.c && sudo ./pmu_probe

Output on the Spark (kernel 7.0.0-1019-nvidia):

    worker ran on cpu 1 then cpu 6
    cpu_cycles        A725=604907562    X925=601488058    sum=1206395620   run/en A=0.5827 X=0.4173
    inst_retired      A725=802234732    X925=801582365    sum=1603817097   run/en A=0.5827 X=0.4173
    bus_access        A725=958084       X925=1316909      sum=2274993      run/en A=0.5827 X=0.4173
    ll_cache_miss_rd  A725=6631         X925=8477         sum=15108        run/en A=0.5827 X=0.4173
    l2d_cache_refill  A725=10487        X925=20696        sum=31183        run/en A=0.5827 X=0.4173
    mem_access        A725=737379       X925=538590       sum=1275969      run/en A=0.5827 X=0.4173

What it shows: each loop (2 × 200M iterations, 4 instructions each) is
counted wholly by the PMU of the core it ran on, so summing the two PMUs
attributes work correctly after migration. Running/enabled is below 1 on
each PMU only because the task spent the rest of its time on the other core
class; the two fractions sum to 1.0000, so nothing was multiplexed. Scaling
each PMU by enabled/running would have inflated the counts, which is why the
R15 method sums raw counts and uses the running-sum test for multiplexing.
