#!/bin/bash
R=/home/drakestapleton/.claude/jobs/9d0b86f6/tmp/bench-scratch/runs
cd $R || exit 1
sec() { awk -F: '{ if (NF==3) print $1*3600+$2*60+$3; else print $1*60+$2 }'; }
stat3() { sort -n | awk '{a[NR]=$1} END{printf "%s / %s / %s", a[int((NR+1)/2)], a[1], a[NR]}'; }
echo "name | wall_s med/min/max | maxRSS_kB med/min/max | user+sys_s med/min/max | n"
for n in rx_heartbeat_test rx_action_graph_test rx_state_projection_test rx_plan_reuse_test rx_sem_incremental_test rx_capability_query_test rx_semantic_comm_test rx_cognitive_routing_test rx_workflow_fusion_test rx_typed_results_test crumbline-learner-decode; do
  w=$(grep -h 'Elapsed' $n.time.[1-5] | awk '{print $NF}' | sec | stat3)
  m=$(grep -h 'Maximum resident' $n.time.[1-5] | awk '{print $NF}' | stat3)
  c=$(for f in $n.time.[1-5]; do awk '/User time/{u=$NF}/System time/{s=$NF}END{print u+s}' $f; done | stat3)
  k=$(ls $n.time.[1-5] 2>/dev/null | wc -l)
  echo "$n | $w | $m | $c | $k"
done
echo; echo "SHIM results"
for n in rx_heartbeat_test rx_action_graph_test rx_state_projection_test rx_plan_reuse_test rx_sem_incremental_test rx_capability_query_test rx_semantic_comm_test rx_cognitive_routing_test rx_workflow_fusion_test rx_typed_results_test crumbline-learner-decode; do
  echo "== $n (SHIM line groups: $(grep -c 'SHIM malloc=' $n.shimerr)) shimRSS_kB=$(grep 'Maximum resident' $n.shimtime | awk '{print $NF}')"
  grep '^SHIM' $n.shimerr
done
echo; echo "exit codes not 0:"; grep -v 'rc=0' rc.txt
