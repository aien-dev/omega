# tools/exp445/reader.sh -- sourced by run.sh and score.sh: the one definition of how an answer becomes
# a candidate (omega#445, docs/one_layer/EXP445_PROTOCOL.md section 6, step 3).
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
    *) line="REFUSED checker: unknown arm $arm" ;;
  esac
  [ -n "$line" ] || line="REFUSED checker: no result"
  printf '%s\n' "$line" | head -n 1 > "$o.check"
}
