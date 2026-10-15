# tools/exp445/reader.sh -- sourced by run.sh and score.sh: the one definition of how a request is built,
# how an answer becomes a candidate and how a generation's log row follows from its raw files
# (omega#445, docs/one_layer/EXP445_PROTOCOL.md sections 6 and 10).
# reader <x445> <omega_emit> <arm> <content file> <out prefix> <requirement>
#   writes <out>.payload (first fenced block), <out>.fence, <out>.candidate.osc (text) or
#   <out>.candidate.oscir (bytes, structured) when the arm's reader makes one, and <out>.check (one
#   line: OK or the refusal). The check uses public data only.
reader() {
  local X="$1" EMIT="$2" arm="$3" content="$4" o="$5" req="$6" line=""
  "$X" extract "$content" "$o.payload" > "$o.fence"
  rm -f "$o.candidate.osc" "$o.candidate.oscir"
  case "$arm" in
    text) cp "$o.payload" "$o.candidate.osc"; line="$("$X" check osc "$o.candidate.osc" "$req" || true)" ;;
    bytes) if line="$("$X" hex "$o.payload" "$o.candidate.oscir")"; then line="$("$X" check oscir "$o.candidate.oscir" "$req" || true)"; fi ;;
    structured) if line="$("$EMIT" emit "$o.payload" "$o.candidate.oscir" 2>&1)"; then line="$("$X" check oscir "$o.candidate.oscir" "$req" || true)"
                else rm -f "$o.candidate.oscir"; fi ;;
    glyph) if line="$("$G" emit "$o.payload" "$o.glyph.json" 2>&1)" && line="$("$EMIT" emit "$o.glyph.json" "$o.candidate.oscir" 2>&1)"; then
             line="$("$X" check oscir "$o.candidate.oscir" "$req" || true)"
           else rm -f "$o.candidate.oscir"; fi ;;
    *) line="REFUSED checker: unknown arm $arm" ;;
  esac
  [ -n "$line" ] || line="REFUSED checker: no result"
  printf '%s\n' "$line" | head -n 1 > "$o.check"
}

# ---- requests, rounds and rows: shared by run.sh (to make them) and score.sh (to re-derive and check them)
# user_msg <requirement> <prompt file>: the user turn (signature lines + the public task prompt)
user_msg() { printf 'Write the function described below.\n\n%s\n\n%s\n' "$(grep -E '^(entry|params|ret)( |$)' "$1")" "$(cat "$2")"; }
# first_msgs <system text> <user text>
first_msgs() { jq -n -c --arg s "$1" --arg u "$2" '[{role: "system", content: $s}, {role: "user", content: $u}]'; }
# repair_msgs <round-1 messages> <round-1 answer> <round-1 check line>: the one repair format of every arm
repair_msgs() {
  local fix="Your program was refused: ${3#REFUSED }"$'\n'"Reply with the corrected complete program in one fenced code block."
  jq -c --arg a "$2" --arg f "$fix" '. + [{role: "assistant", content: $a}, {role: "user", content: $f}]' <<< "$1"
}
# request_json <model> <messages> <max_tokens> <temperature> <top_p> <seed> <config extra>
request_json() {
  jq -n -c --arg m "$1" --argjson msgs "$2" --argjson mt "$3" --argjson t "$4" --argjson p "$5" --argjson s "$6" --argjson x "$7" \
    '{model: $m, messages: $msgs, max_tokens: $mt, temperature: $t, top_p: $p, seed: $s, stream: false} + $x'
}
# seed_of <seed_base> <attempt> <task index, 1-based in sorted task order>: one seed per task and attempt
seed_of() { echo $(($1 + 1000 * $2 + $3)); }
resp_ok() { jq -e '.choices[0].message.content | type == "string"' "$1" > /dev/null 2>&1; }
content_of() { jq -r '.choices[0].message.content' "$1"; }
# usage_of <response>: prompt tokens, completion tokens, finish reason (tab-separated). The response is
# untrusted: a token count that is not a non-negative integer (or is missing) prints BAD, a finish reason
# outside [A-Za-z_]{1,32} prints BAD; score.sh refuses a run with a BAD token count.
usage_of() {
  jq -r 'def n: if type == "number" and . >= 0 and . == floor and . < 100000000 then floor | tostring else "BAD" end;
    [(.usage.prompt_tokens | n), (.usage.completion_tokens | n),
     (.choices[0].finish_reason | if type == "string" and test("^[A-Za-z_]{1,32}$") then . else "BAD" end)] | @tsv' "$1"
}
# A round <p> = <gen dir>/rN is committed when <p>.transport exists: one line per try,
#   try=K http=CODE curl_exit=E time_s=T body=rN.resp.json|rN.err.K.json|none msg=<curl message>
# and it succeeded when its last try's body is rN.resp.json.
round_ok() { [ -f "$1.transport" ] && [ "$(tail -n 1 "$1.transport" | sed -n 's/.* body=\([^ ]*\).*/\1/p')" = "${1##*/}.resp.json" ]; }
wall_of() { local f; for f in "$1"/r*.transport; do if [ -f "$f" ]; then cat "$f"; fi; done | sed -n 's/.* time_s=\([^ ]*\).*/\1/p' | awk '{ s += $1 } END { printf "%.6f", s }'; }
# gen_row <set> <task> <raw gen dir> <derived dir>: sets ROW (the log.tsv row) and FINAL (the final round or
# empty) from the raw responses and transport records in <raw gen dir> and the reader's rN.check /
# rN.candidate.* in <derived dir>. Tokens, finish reasons and wall time come from the raw files only.
gen_row() {
  local d="$3" v="$4" rounds=0 pt=0 p2=0 c1=0 c2=- f1=- f2=- k1=- k2=- cs=- ext
  FINAL=""
  if round_ok "$d/r1"; then
    rounds=1; IFS=$'\t' read -r pt c1 f1 < <(usage_of "$d/r1.resp.json"); k1="$(< "$v/r1.check")"
    if [ "$k1" = OK ]; then FINAL=1
    elif round_ok "$d/r2"; then
      rounds=2; IFS=$'\t' read -r p2 c2 f2 < <(usage_of "$d/r2.resp.json"); if [[ "$pt" =~ ^[0-9]+$ && "$p2" =~ ^[0-9]+$ ]]; then pt=$((pt + p2)); else pt=BAD; fi; k2="$(< "$v/r2.check")"; FINAL=2
    else k2="REQUEST FAILED"; fi
  else k1="REQUEST FAILED"; fi
  if [ -n "$FINAL" ]; then
    for ext in osc oscir; do if [ -f "$v/r$FINAL.candidate.$ext" ]; then cs="$(sha256sum < "$v/r$FINAL.candidate.$ext" | cut -d' ' -f1)"; fi; done
  fi
  ROW="$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' "$1" "$2" "$rounds" "$pt" "$c1" "$c2" "$f1" "$f2" "$(wall_of "$d")" \
    "${k1//$'\t'/ }" "${k2//$'\t'/ }" "$cs")"
}
# gen_sums <gen dir> / run_sums <run dir>: the hashed raw record (requests, responses, failed bodies,
# transport records; and per run also the prompts, meta.txt, log.tsv, weights.check, server.json)
gen_sums() { (cd "$1" && for f in r*.req.json r*.resp.json r*.err.*.json r*.transport; do if [ -f "$f" ]; then sha256sum "$f"; fi; done) > "$1/SHA256SUMS"; }
run_files() { (cd "$1" && { find gen prompts -type f \( -name '*.json' -o -name '*.transport' -o -name '*.system.txt' \); printf '%s\n' meta.txt log.tsv weights.check server.json; } | LC_ALL=C sort); }
run_sums() { (cd "$1" && run_files . | tr '\n' '\0' | xargs -0 sha256sum) > "$1/SHA256SUMS"; }
# weights_ok <run dir> <hash list> <model id>: weights.check is exactly "<file>: OK" for every file of the
# config's frozen hash list, and server.json (GET /models) lists the model id
weights_ok() {
  [ -f "$1/weights.check" ] && [ -f "$1/server.json" ] && [ -s "$2" ] || return 1
  [ "$(awk 'NF { n = $2; sub(/^\*/, "", n); print n ": OK" }' "$2" | LC_ALL=C sort)" = "$(LC_ALL=C sort "$1/weights.check")" ] || return 1
  jq -e --arg m "$3" '[.data[]?.id] | index($m) != null' "$1/server.json" > /dev/null 2>&1
}
