#!/usr/bin/env bash
# tools/exp445/measure.sh <base-url> <model> <dev cases dir> -- token length of every PUBLIC dev task's
# reference solution in each arm's spelling, as the served model's tokenizer counts it (omega#445,
# sizes the per-round output cap in docs/one_layer/EXP445_PROTOCOL.md). No program is generated:
# each request asks for 1 output token and the count is prompt_tokens(message = spelling) minus
# prompt_tokens(empty message). Spellings: text = reference.osc; bytes = `x445 tohex` of the
# compiled IR; structured = `omega_emit lift` of the same IR. Prints: task text bytes structured.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
[ $# -eq 3 ] || { echo "usage: measure.sh <base-url> <model> <dev cases dir>" >&2; exit 2; }
BASE="${1%/}" MODEL="$2" CASES="$3"
make -s -C "$ROOT" PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 omega-emit build/compiler/oscc >&2
T="$(mktemp -d "${TMPDIR:-/tmp}/exp445-measure.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
ptoks() { # ptoks <file>: prompt tokens of one user message holding the file
  jq -n -c --arg m "$MODEL" --rawfile c "$1" '{model: $m, messages: [{role: "user", content: $c}], max_tokens: 1, temperature: 0}' |
    curl -sS -m 120 -H 'Content-Type: application/json' --data-binary @- "$BASE/chat/completions" | jq -e -r '.usage.prompt_tokens'
}
: > "$T/empty"; base="$(ptoks "$T/empty")"
echo "# task text bytes structured (tokens; empty-message baseline $base subtracted)"
for c in "$CASES"/*/; do
  id="$(basename "$c")"
  "$ROOT/build/compiler/oscc" "$c/hidden/reference.osc" "$T/$id" > /dev/null
  "$ROOT/build/omega_emit/x445" tohex "$T/$id.ir" > "$T/$id.hex"
  "$ROOT/build/omega_emit/omega_emit" lift "$T/$id.ir" > "$T/$id.json"
  echo "$id $(( $(ptoks "$c/hidden/reference.osc") - base )) $(( $(ptoks "$T/$id.hex") - base )) $(( $(ptoks "$T/$id.json") - base ))"
done
