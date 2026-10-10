#!/bin/bash
# AT-1 Agent 7 phase 2: 21-row hidden-case table. Joins the committed EXPECTED.tsv (hidden set
# 63b2edf0...) with the evaluator's TABLE.tsv, the spliced result files (case_id, acceptance_id,
# the twelve check statuses, outcome) and VALUES_VS_DETAILS.tsv. Usage: hidden-table.sh <hidden-run dir> <EXPECTED.tsv>
export LC_ALL=C
H=$1; E=$2
printf '# file\tclass\texpected_outcome\texpected_codes\tcodec\tengine\toracle\tevaluator_outcome\tevaluator_codes\tcase_verdict\tchecks_expected\tchecks_got\tcase_id\tacceptance_id\tvalues_vs_exact\tmatch\n'
grep -v '^#' "$E" | while IFS=$'\t' read -r file class kind eo ec checks cid aid att; do
  name=${file%.case}
  row=$(grep "/$file	" "$H/out/TABLE.tsv")
  tex=$(printf "%s" "$row" | cut -f3); tco=$(printf "%s" "$row" | cut -f4); ten=$(printf "%s" "$row" | cut -f5); tor=$(printf "%s" "$row" | cut -f6); tout=$(printf "%s" "$row" | cut -f7); tcodes=$(printf "%s" "$row" | cut -f8); tv=$(printf "%s" "$row" | cut -f9)
  if [ "$kind" = RESULT ]; then
    r=$(ls "$H"/out/spliced/*_"$name".result)
    got=$(awk '$1=="check"{s=substr($3,1,1); if($3=="NOT_EVALUATED")s="N"; printf "%s", s}' "$r")
    rc=$(awk '$1=="case_id"{print $2}' "$r"); ra=$(awk '$1=="acceptance_id"{print $2}' "$r")
    [ "$rc" = "$cid" ] && cm=same || cm=DIFF; [ "$ra" = "$aid" ] && am=same || am=DIFF
    vv=$(awk -v n="$name" '$1==n{print $NF}' "$H/VALUES_VS_DETAILS.tsv")
    ok=yes; [ "$tout" = "$eo" ] || ok=no; [ "$tcodes" = "$ec" ] || ok=no; [ "$tv" = PASS ] || ok=no
    [ "$got" = "$checks" ] || ok=no; [ $cm = same ] && [ $am = same ] || ok=no; [ "$vv" = AGREE ] || ok=no
  else
    got=-; cm=-; am=-; vv=-; tout=REFUSED; tcodes=$(printf '%s' "$tex" | awk '{print $2}')
    ok=yes; [ "$tcodes" = "$eo" ] || ok=no; [ "$tv" = PASS ] || ok=no; [ "$ten" = refused-exactly ] && [ "$tor" = refused-exactly ] && [ "$tco" = ok ] || ok=no
  fi
  printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" "$file" "$class" "$eo" "$ec" "$tco" "$ten" "$tor" "$tout" "$tcodes" "$tv" "$checks" "$got" "$cm" "$am" "$vv" "$ok"
done
