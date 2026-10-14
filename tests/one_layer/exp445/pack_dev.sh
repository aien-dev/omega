#!/usr/bin/env bash
# tests/one_layer/exp445/pack_dev.sh <outdir> -- pack and seal the PUBLIC exp445 dev tasks (omega#445,
# HARNESS_INTERFACE_V2.md section 4) into <outdir>/tasks.tar + <outdir>/TASKS.sha256, exactly the
# held-out layout. First checks every hidden/expected against gen_expected.c (computed from the
# prompts, independently of reference.osc); a mismatch refuses. Reproducible bytes (ustar, sorted,
# owner 0, mtime 0). Then: tests/one_layer/acceptance/ola.sh tasks-check <outdir>/tasks.tar <outdir>/TASKS.sha256
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: pack_dev.sh <outdir>}"
mkdir -p "$OUT"; OUT="$(realpath -e "$OUT")"
T="$(mktemp -d "${TMPDIR:-/tmp}/exp445-pack.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O2 -o "$T/gen" "$HERE/gen_expected.c"
n=0
for c in "$HERE"/dev/tasks/cases/*/; do
  id="$(basename "$c")"
  "$T/gen" "$id" < "$c/hidden/inputs" > "$T/exp"
  cmp -s "$T/exp" "$c/hidden/expected" || { echo "pack_dev: $id hidden/expected differs from gen_expected" >&2; exit 1; }
  n=$((n + 1))
done
(cd "$HERE/dev" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf "$OUT/tasks.tar.tmp" tasks)
mv "$OUT/tasks.tar.tmp" "$OUT/tasks.tar"
(cd "$OUT" && sha256sum tasks.tar > TASKS.sha256.tmp && mv TASKS.sha256.tmp TASKS.sha256)
echo "pack_dev: $n dev tasks -> $OUT/tasks.tar ($(cut -c1-16 "$OUT/TASKS.sha256")...)"
