# Parses `perf report -T --stdio` per-thread counters into one JSON line.
# Classes: consumer (tid == root+1 when cfg != 0: the RX_ARGUS=2
# constructor creates it before main), workers (other threads of root), children
# (other processes: R8's posix_spawn'd /bin/true). root = smallest pid in the table.
# main (the leader thread) is not in perf report -T; it comes in as mainj from a
# separate `perf stat --no-inherit` run of the same cfg in the same round.
/^#[ ]+PID[ ]+TID/ { for (i = 2; i <= NF; i++) h[i-1] = $i; hdr = 1; next }
hdr && NF >= 10 && $1 ~ /^[0-9]+$/ { rows[++nr] = $0; if (root == "" || $1 + 0 < root) root = $1 + 0; next }
END {
  for (q = 1; q <= nr; q++) {
    nf = split(rows[q], f, " "); pid = f[1] + 0; tid = f[2] + 0
    c = (pid != root) ? "children" : (tid == root) ? "main" : (tid == root + 1 && cfg != "0") ? "consumer" : "workers"
    for (i = 3; i <= nf; i++) v[c SUBSEP i] += f[i]
    n[c]++
  }
  printf "{\"cfg\":\"%s\",\"round\":%d,\"wall_us\":%d,\"passed\":%s", cfg, r, wall, pass
  if (mainj != "") printf ",\"main\":%s", mainj
  split("consumer workers children", cl, " ")
  for (j = 1; j <= 3; j++) {
    c = cl[j]; printf ",\"%s\":{\"n\":%d", c, n[c] + 0
    for (i = 3; i in h; i++) {
      k = h[i]; if (k ~ /dummy|period/) continue
      gsub(/armv8_pmuv3_/, "pmu", k); gsub(/\//, "_", k); sub(/_$/, "", k)
      printf ",\"%s\":%d", k, v[c SUBSEP i] + 0
    }
    printf "}"
  }
  print "}"
}
