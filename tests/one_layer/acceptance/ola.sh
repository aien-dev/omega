#!/usr/bin/env bash
# tests/one_layer/acceptance/ola.sh -- one-layer stage 1 hostile acceptance harness (omega#443).
# Contract: HARNESS_INTERFACE.md next to this file (v1; its sha256 is pinned in RULES and checked here).
#
#   ola.sh score <corpus.tar> <SEALED.sha256>
#
# Refuses a dirty git tree. Writes evidence/ONE-LAYER-1/runs/<run_id>/ (never overwritten) with
# verdicts.txt, probes.log, score.txt (+ replay/verdicts.txt, meta.txt) and content-addressed receipts
# under evidence/ONE-LAYER-1/receipts/. Steps:
#   1 clean tree, contract hash, build ola + ola_runner from this commit
#   2 ola untar: sealed hash check, member validation, extract WITHOUT corpus/LABELS (private dir)
#   3 pass A: every case run in the sandbox (ola_runner), verdict + receipt
#   4 pass B: a second clean extraction and execution; verdicts must be byte-identical
#   5 LABELS extracted only now; isolation probes (each blocked AND logged, each with a control;
#     the evidence probe aims at a private copy of verdicts.txt, the real one is made read-only)
#   6 rules and harness re-hashed (unchanged), tally -> score.txt (GATE PASS|FAIL)
# Selftest-only knobs (each forces GATE FAIL): OLA_ALLOW_DIRTY=1, OLA_EVIDENCE_ROOT=<dir>.
# OLA_PROBE_ONLY (a probe subset for `ola probes`) is cleared here and also forces GATE FAIL; tally
# independently requires every probe in probes.log. The sandbox is aarch64-only: other machines are refused.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
cd "$ROOT"

die() { echo "ola.sh: $*" >&2; exit 2; }
kv() { awk -F= -v k="$1" '$1 == k { print substr($0, length(k) + 2); exit }' "$HERE/RULES"; }
sha() { sha256sum "$1" | awk '{print $1}'; }

# harness identity: every tracked file of the harness and its make fragment, in name order
harness_sha() {
  { git ls-files -z -- tests/one_layer/acceptance mk/one-layer-acceptance.mk | sort -z | xargs -0 sha256sum; } | sha256sum | awk '{print $1}'
}

score() {
  [ $# -eq 2 ] || die "usage: ola.sh score <corpus.tar> <SEALED.sha256>"
  local tar sealed; tar="$(realpath -e "$1")" || die "no corpus tarball $1"
  sealed="$(realpath -m "$2")"
  [ -f "$sealed" ] || die "$sealed does not exist: the corpus is not sealed yet; nothing scored"
  [ "$(uname -m)" = aarch64 ] || die "the sandbox (seccomp allow-list) is aarch64-only; refusing to score on $(uname -m)"

  # 1. clean tree: no change to tracked files, nothing untracked except earlier ONE-LAYER-1 output
  local clean=1 dirty
  dirty="$(git status --porcelain=v1 --untracked-files=all | grep -Ev '^\?\? evidence/ONE-LAYER-1/(runs|receipts)/' || true)"
  if [ -n "$dirty" ]; then
    if [ "${OLA_ALLOW_DIRTY:-0}" = 1 ]; then clean=0; echo "ola.sh: DIRTY tree allowed for a selftest: the gate cannot pass" >&2
    else echo "$dirty" | head -20 >&2; die "refusing a dirty git tree (commit or stash first)"; fi
  fi
  local evid="$ROOT/evidence/ONE-LAYER-1"
  if [ -n "${OLA_EVIDENCE_ROOT:-}" ]; then evid="$OLA_EVIDENCE_ROOT"; clean=0; fi
  if [ -n "${OLA_PROBE_ONLY:-}" ]; then
    echo "ola.sh: OLA_PROBE_ONLY is a selftest knob: cleared, every probe runs, the gate cannot pass" >&2
    clean=0
  fi
  unset OLA_PROBE_ONLY
  [ "$(sha "$HERE/HARNESS_INTERFACE.md")" = "$(kv interface_sha256)" ] || die "HARNESS_INTERFACE.md does not match the pinned v1 hash"
  local commit; commit="$(git rev-parse HEAD)"
  [ "$clean" = 1 ] || commit="$commit-dirty"
  make -s PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 one-layer-acceptance-tools >&2
  local bin="$ROOT/build/one_layer" rules_sha harness
  rules_sha="$(sha "$HERE/RULES")"
  harness="$(harness_sha)"

  # 2. private directory (never inside the repo, never visible to a candidate)
  priv="$(mktemp -d "${TMPDIR:-/tmp}/ola-priv.XXXXXXXX")"   # global: the EXIT trap removes it
  chmod 700 "$priv"
  trap 'rm -rf "${priv:-}"' EXIT
  mkdir "$priv/A" "$priv/B" "$priv/scratch" "$priv/keys" "$priv/outside"
  head -c 32 /dev/urandom > "$priv/keys/decoy.key"   # a key-shaped secret the probes try to read
  local corpus_sha
  corpus_sha="$("$bin/ola" untar "$tar" "$sealed" "$priv/A" cases | sed -n 's/^corpus_sha256=//p')"
  [ ${#corpus_sha} -eq 64 ] || die "untar gave no corpus hash"

  local run_id run
  run_id="$(date -u +%Y%m%dT%H%M%SZ)-${corpus_sha:0:12}-$$"
  mkdir -p "$evid/runs" "$evid/receipts"
  run="$evid/runs/$run_id"
  mkdir "$run" || die "run directory $run exists: never overwritten"
  mkdir "$run/replay"
  {
    echo "commit=$commit"; echo "harness_sha256=$harness"; echo "rules_sha256=$rules_sha"
    echo "corpus_sha256=$corpus_sha"; echo "time_ms=$(kv time_ms)"; echo "mem_mb=$(kv mem_mb)"
  } > "$priv/meta"
  cp "$priv/meta" "$run/meta.txt"
  echo "ola.sh: run $run_id corpus $corpus_sha" >&2

  # 3. pass A
  echo "pass A:" >&2
  "$bin/ola" pass "$priv/A/corpus" "$run/verdicts.txt" "$evid/receipts" "$priv/scratch" "$bin/ola_runner" "$priv/meta"
  # 4. pass B from a fresh extraction
  echo "pass B (clean replay):" >&2
  "$bin/ola" untar "$tar" "$sealed" "$priv/B" cases >/dev/null
  "$bin/ola" pass "$priv/B/corpus" "$run/replay/verdicts.txt" "$evid/receipts" "$priv/scratch" "$bin/ola_runner" "$priv/meta"
  local replay=0
  "$bin/ola" cmp "$run/verdicts.txt" "$run/replay/verdicts.txt" && replay=1
  chmod a-w "$run/verdicts.txt" "$run/replay/verdicts.txt"   # written once; the probes aim at a copy
  local verdicts_sha; verdicts_sha="$(sha "$run/verdicts.txt")"
  mkdir "$priv/evidence" && cp "$run/verdicts.txt" "$priv/evidence/verdicts.txt" && chmod u+w "$priv/evidence/verdicts.txt"

  # 5. LABELS only now; then the isolation probes against the real private files
  "$bin/ola" untar "$tar" "$sealed" "$priv/A" labels >/dev/null
  local first; first="$(ls "$priv/A/corpus/cases" | LC_ALL=C sort | head -n 1)"
  echo "isolation probes:" >&2
  "$bin/ola" probes "$bin/ola_runner" "$priv/scratch" "$run/probes.log" "$(kv time_ms)" "$(kv mem_mb)" \
    hidden_inputs="$priv/A/corpus/cases/$first/hidden/inputs" expected="$priv/A/corpus/cases/$first/hidden/expected" \
    labels="$priv/A/corpus/LABELS" tar="$tar" key="$priv/keys/decoy.key" evidence="$priv/evidence/verdicts.txt" \
    scorer="$HERE/ola.sh" rules="$HERE/RULES" outside="$priv/outside/escape" ||
    echo "ola.sh: isolation probes did not all pass" >&2

  # 6. nothing the candidates could reach changed the rules or the harness
  [ "$(sha "$HERE/RULES")" = "$rules_sha" ] || { echo "ola.sh: RULES changed during the run" >&2; clean=0; }
  [ "$(harness_sha)" = "$harness" ] || { echo "ola.sh: harness changed during the run" >&2; clean=0; }
  [ "$(sha "$run/verdicts.txt")" = "$verdicts_sha" ] || { echo "ola.sh: verdicts.txt changed during the probes" >&2; clean=0; }
  "$bin/ola" tally "$run/verdicts.txt" "$priv/A/corpus/LABELS" "$replay" "$run/probes.log" "$clean" > "$run/score.txt"
  cat "$run/score.txt"
  echo "ola.sh: evidence in ${run#"$ROOT"/}" >&2
}

case "${1:-}" in
  score) shift; score "$@" ;;
  *) die "usage: ola.sh score <corpus.tar> <SEALED.sha256>" ;;
esac
