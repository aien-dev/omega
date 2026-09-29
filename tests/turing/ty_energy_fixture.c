/* Synthetic meter windows for reducer tests. No hardware measurement claim. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void edge(uint64_t time, uint64_t energy, int ok, int overflow) {
    printf("{\"t_ns\":%" PRIu64 ",\"spbm_ok\":%d,\"overflow\":[%d,0,0],\"energy_uj\":[%" PRIu64 ",1000000,1000000]}", time, ok, overflow, energy);
}
static void participant(char tag, uint64_t start, int oracle, int multiplexed) {
    printf("{\"ok\":1,\"result\":{\"tag\":\"%c\",\"rz\":\"R1_plain\",\"cpus\":\"5\",\"pinned\":1,\"oracle_ok\":%d,\"planned_ns\":1000000000,\"go_ns\":%" PRIu64 ",\"t0_ns\":%" PRIu64 ",\"t1_ns\":%" PRIu64 ",\"done_ns\":%" PRIu64 ",\"calls\":1,\"pmu_open\":1,\"pmu\":{\"ok\":1,\"sum\":{\"cpu_cycles\":100,\"inst_retired\":200,\"l2d_cache_refill\":5},\"pmu\":[{\"value\":[1,1,1,1,1,1],\"enabled\":[1000,1000,1000,1000,1000,1000],\"running\":[%d,%d,%d,%d,%d,%d]}]}}}", tag, oracle, start, start+1000000, start+999000000, start+999000001, multiplexed ? 0 : 1000, multiplexed ? 0 : 1000, multiplexed ? 0 : 1000, multiplexed ? 0 : 1000, multiplexed ? 0 : 1000, multiplexed ? 0 : 1000);
}
int main(int argc, char **argv) {
    if (argc != 4) return 64;
    const char *cond = argv[1], *fault = argv[3];
    uint64_t start = strtoull(argv[2], NULL, 10);
    int ok = strcmp(fault, "sensor") != 0;
    uint64_t end_energy = !strcmp(fault, "wrap") ? 999999 : 1100000;
    printf("{\"run\":\"fixture\",\"config\":\"S1\",\"level\":\"%s\",\"round\":1,\"trial\":1,\"planned_ns\":1000000000,\"tool_cpu\":0,\"meter_ok\":1,\"spec_a\":\"R1_plain:5\",\"spec_b\":\"R2c_crumb:15\",\"e0\":", cond);
    edge(start, 1000000, ok, 0); printf(",\"e1\":");
    edge(start+1000000000, end_energy, ok, !strcmp(fault, "overflow"));
    printf(",\"tel\":{\"t_ns\":[");
    for (int i=1; i<=10; i++) printf("%s%" PRIu64, i == 1 ? "" : ",", start+(uint64_t)i*100000000);
    printf("],\"ok\":[1,1,1,1,1,1,1,1,1,1]},\"participants\":[");
    if (strcmp(cond, "IDLE")) for (size_t i=0; i<strlen(cond); i++) {
        if (i) printf(",");
        participant(cond[i], start, strcmp(fault, "oracle") != 0, !strcmp(fault, "multiplexed"));
    }
    printf("],\"s0\":{\"busy_jiffies\":[0],\"temp_mc\":[40000]},\"s1\":{\"busy_jiffies\":[0],\"temp_mc\":[40000]}}\n");
    return 0;
}
