#!/usr/bin/env bash
# tests/one_layer/generalization/dev/pack_dev.sh <outdir> -- pack and seal the PUBLIC GEN dev fixtures
# devgen01..devgen10 (aien-spark/omega#476) into <outdir>/tasks.tar + <outdir>/TASKS.sha256, exactly the
# held-out layout of tests/one_layer/acceptance/HARNESS_INTERFACE_V2.md section 3. Same steps as
# tests/one_layer/exp445/pack_dev.sh (which packs only its own directory, so it cannot be called here):
# first every hidden/expected is checked against gen_expected.c (computed from the prompts, independently
# of reference.osc; a mismatch refuses), and the case directories must be exactly the task ids of META.
# Reproducible bytes (ustar, sorted, owner 0, mtime 0). META stays OUTSIDE the tarball (member rules).
# Then: tests/one_layer/acceptance/ola.sh tasks-check <outdir>/tasks.tar <outdir>/TASKS.sha256
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: pack_dev.sh <outdir>}"
mkdir -p "$OUT"; OUT="$(realpath -e "$OUT")"
T="$(mktemp -d "${TMPDIR:-/tmp}/gen-pack.XXXXXXXX")"; trap 'rm -rf "$T"' EXIT
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -O2 -o "$T/gen" "$HERE/gen_expected.c"
awk '!/^#/ && NF && $0 != "gen-meta 1" { print $1 }' "$HERE/META" | LC_ALL=C sort > "$T/meta.ids"
(cd "$HERE/tasks/cases" && ls -1) | LC_ALL=C sort > "$T/case.ids"
cmp -s "$T/meta.ids" "$T/case.ids" || { echo "pack_dev: META task ids differ from tasks/cases/" >&2; exit 1; }
n=0
for c in "$HERE"/tasks/cases/*/; do
  id="$(basename "$c")"
  "$T/gen" "$id" < "$c/hidden/inputs" > "$T/exp"
  cmp -s "$T/exp" "$c/hidden/expected" || { echo "pack_dev: $id hidden/expected differs from gen_expected" >&2; exit 1; }
  n=$((n + 1))
done
(cd "$HERE" && tar --format=ustar --sort=name --owner=0 --group=0 --numeric-owner --mtime=@0 -cf "$OUT/tasks.tar.tmp" tasks)
mv "$OUT/tasks.tar.tmp" "$OUT/tasks.tar"
(cd "$OUT" && sha256sum tasks.tar > TASKS.sha256.tmp && mv TASKS.sha256.tmp TASKS.sha256)
echo "pack_dev: $n dev tasks -> $OUT/tasks.tar ($(cut -c1-16 "$OUT/TASKS.sha256")...)"
