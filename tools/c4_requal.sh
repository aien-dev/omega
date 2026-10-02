#!/bin/sh
# C4 requalification gate: the complete local runtime (World -> J-Space ->
# AEGIS/FORGE verify -> commit -> Cortex) on one machine, one repeatable task,
# with interruption and recovery. Refuses a dirty tree, builds and runs the
# committed source, stores a receipt named by its content hash:
#   evidence/C4-REQUAL/<sha256-of-receipt>.json
# The receipt carries the commit of every repo of the candidate (and whether it
# matches the CAND-0 manifest in aien-architecture), the executable digests,
# the 14-step COMPOSITION-2 gate verdict, every injection case, and (with
# --mutants) the recovery-check mutant results.
#   usage: tools/c4_requal.sh [--mutants]
# Env: MAKE_ARGS (e.g. AIENOS_LOCK_REPO=...), C4_SC_DIR, C4_PROTO_DIR, C4_ARCH_DIR.
set -eu
want_mut=0
[ "${1:-}" = "--mutants" ] && want_mut=1
root=$(git rev-parse --show-toplevel)
cd "$root"
if [ -n "$(git status --porcelain --untracked-files=normal)" ]; then
    echo "c4_requal: working tree is dirty; commit first" >&2
    git status --short >&2
    exit 2
fi
commit=$(git rev-parse HEAD)
sc_dir=${C4_SC_DIR:-$HOME/workspace/aien-sovereign-core}
proto_dir=${C4_PROTO_DIR:-$HOME/workspace/aien-protocols}
arch_dir=${C4_ARCH_DIR:-$HOME/workspace/aien-architecture}
head_of() { git -C "$1" rev-parse origin/main 2>/dev/null || echo unknown; }
aienos_pin=$(head -n 1 aienos.lock)
sc_head=$(head_of "$sc_dir"); proto_head=$(head_of "$proto_dir"); arch_head=$(head_of "$arch_dir")

# CAND-0 pins (aien-architecture qualification/candidates/CAND-0.toml on origin/main).
cand=$(git -C "$arch_dir" show origin/main:qualification/candidates/CAND-0.toml 2>/dev/null || true)
cand_status=$(printf '%s\n' "$cand" | sed -n 's/^status *= *"\(.*\)"/\1/p' | head -n 1)
pin() { printf '%s\n' "$cand" | sed -n "s/^$1 *= *\"\([0-9a-f]\{40\}\)\".*/\1/p" | head -n 1; }
c_omega=$(pin omega); c_aienos=$(pin aienos); c_sc=$(pin aien-sovereign-core)
c_proto=$(pin aien-protocols); c_arch=$(pin aien-architecture)
m() { [ -n "$2" ] && [ "$1" = "$2" ] && echo true || echo false; }

make ${MAKE_ARGS:-} c4-requal-bin composition-gate-bin >/dev/null
bin=$(make -s ${MAKE_ARGS:-} print-c4-requal-bin)
gbin=$(make -s ${MAKE_ARGS:-} print-composition-gate-bin)
work=$(mktemp -d /tmp/c4req.XXXXXX)
trap 'rm -rf "$work"' EXIT
set +e
"./$gbin" "$commit" "$work/composition.json" >"$work/composition.out" 2>&1
gstatus=$?
"./$bin" "$work/injection.json" >"$work/injection.out" 2>&1
istatus=$?
mut_status=0
if [ $want_mut -eq 1 ]; then
    tools/c4_requal_mutants.sh "$work/mutants.json" >"$work/mutants.out" 2>&1
    mut_status=$?
fi
set -e
[ -s "$work/injection.json" ] || { echo "c4_requal: no injection receipt" >&2; cat "$work/injection.out" >&2; exit 2; }
gverdict=$(sed -n 's/.*"verdict": "\([A-Z]*\)".*/\1/p' "$work/composition.json" | head -n 1)
iverdict=$(sed -n 's/.*"verdict": "\([A-Z]*\)".*/\1/p' "$work/injection.json" | head -n 1)
verdict=PASS
[ "$gstatus" -eq 0 ] && [ "$gverdict" = PASS ] || verdict=FAIL
[ "$istatus" -eq 0 ] && [ "$iverdict" = PASS ] || verdict=FAIL
[ "$mut_status" -eq 0 ] || verdict=FAIL
bsha=$(sha256sum "$bin" | cut -d' ' -f1); gsha=$(sha256sum "$gbin" | cut -d' ' -f1)
binding=NOT_BOUND
if [ "$cand_status" = frozen ] && [ "$(m "$commit" "$c_omega")" = true ] && [ "$(m "$aienos_pin" "$c_aienos")" = true ]; then binding=BOUND; fi
indent() { sed 's/^/    /' "$1"; }
tmp="$work/receipt.json"
{
    printf '{\n  "schema": "C4RequalReceiptV1",\n  "gate": "C4-REQUAL",\n  "verdict": "%s",\n' "$verdict"
    printf '  "date_utc": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '  "env_class": "host CPU on the Spark (no chip, no QEMU, no network)",\n'
    printf '  "machine": {"host": "%s", "arch": "%s", "kernel": "%s"},\n' "$(hostname)" "$(uname -m)" "$(uname -r)"
    printf '  "tree_dirty": false,\n'
    printf '  "repos": {\n'
    printf '    "omega": {"commit": "%s", "exercised": true, "cand0_pin": "%s", "matches_cand0": %s},\n' "$commit" "$c_omega" "$(m "$commit" "$c_omega")"
    printf '    "aienos": {"commit": "%s", "source": "omega aienos.lock (capability authority library built from this commit)", "exercised": true, "cand0_pin": "%s", "matches_cand0": %s},\n' "$aienos_pin" "$c_aienos" "$(m "$aienos_pin" "$c_aienos")"
    printf '    "aien-sovereign-core": {"commit": "%s", "source": "origin/main of local clone at run time", "exercised": false, "cand0_pin": "%s", "matches_cand0": %s},\n' "$sc_head" "$c_sc" "$(m "$sc_head" "$c_sc")"
    printf '    "aien-protocols": {"commit": "%s", "source": "origin/main of local clone at run time", "exercised": false, "cand0_pin": "%s", "matches_cand0": %s},\n' "$proto_head" "$c_proto" "$(m "$proto_head" "$c_proto")"
    printf '    "aien-architecture": {"commit": "%s", "source": "origin/main of local clone at run time", "exercised": false, "cand0_pin": "%s", "matches_cand0": %s}\n' "$arch_head" "$c_arch" "$(m "$arch_head" "$c_arch")"
    printf '  },\n'
    printf '  "candidate": {"id": "CAND-0", "manifest_status": "%s", "binding": "%s", "note": "binding is BOUND only when CAND-0 is frozen and the omega and aienos commits match; this run is evidence for the listed commits, not a candidate qualification"},\n' "${cand_status:-unreadable}" "$binding"
    printf '  "executables": {"rx_c4_requal": "%s", "rx_composition_gate": "%s", "build": "make c4-requal-bin composition-gate-bin (c4 binary is a test build, -DAIEN_TEST_BUILD=1; the composition gate binary is the production build)"},\n' "$bsha" "$gsha"
    printf '  "composition_gate": {"verdict": "%s", "exit": %s},\n' "$gverdict" "$gstatus"
    printf '  "known_gaps": [\n'
    printf '    "FORGE is not on the composed path: the Skills are CPU procedures; omega_forge_realize has no caller in src/runtime (GPU tier uses the m16 native path, not FORGE)",\n'
    printf '    "aien-sovereign-core and aien-protocols are not exercised by this chain",\n'
    printf '    "Cortex owner is unresolved (three implementations); this gate qualifies omega rx_cortex only",\n'
    printf '    "Fabric and remote J-Space do not exist; local only",\n'
    printf '    "J-Space spill file is empty in this task, so corruption of spilled data is not exercised",\n'
    printf '    "power-loss durability (fsync ordering on real media) is not tested; crashes are process exits and SIGKILL"\n'
    printf '  ],\n'
    printf '  "injection":\n'
    indent "$work/injection.json"
    if [ $want_mut -eq 1 ]; then printf '  ,\n  "mutants":\n'; indent "$work/mutants.json"; else printf '  ,\n  "mutants": null\n'; fi
    printf '}\n'
} > "$tmp"
sum=$(sha256sum "$tmp" | cut -d' ' -f1)
mkdir -p evidence/C4-REQUAL
out="evidence/C4-REQUAL/$sum.json"
cp "$tmp" "$out"
chmod 0644 "$out"
cat "$work/injection.out" | tail -n 5
[ $want_mut -eq 1 ] && cat "$work/mutants.out"   # one line per mutant (an ERROR line carries the make exit code and last log lines)
echo "receipt: $out"
echo "verdict: $verdict (omega $commit, binding $binding)"
[ "$verdict" = PASS ]
