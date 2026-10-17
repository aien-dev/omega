#!/usr/bin/env bash
# tests/one_layer/acceptance/ola.sh -- one-layer hostile acceptance harness (stage 1 omega#443, stage 2 omega#445).
# Contracts: HARNESS_INTERFACE.md (v1) and HARNESS_INTERFACE_V2.md (v2) next to this file; their sha256
# are pinned in RULES and checked here.
#
#   ola.sh score         <corpus.tar> <SEALED.sha256>                    v1, unchanged (evidence/ONE-LAYER-1)
#   ola.sh score-v2      <corpus.tar> <SEALED.sha256>                    v2 corpus with kind oscir (evidence/ONE-LAYER-2)
#   ola.sh tasks-public  <tasks.tar> <TASKS.sha256> <outdir>             only requirement + prompt of every task
#   ola.sh experiment    <tasks.tar> <TASKS.sha256> <candidates_dir>     score candidate sets, no LABELS, RUN VALID|INVALID
#   ola.sh tasks-check   <tasks.tar> <TASKS.sha256>                      experiment with no sets: references only
#
# Refuses a dirty git tree. Writes evidence/ONE-LAYER-<1|2>/runs/<run_id>/ (never overwritten) with
# verdicts.txt, probes.log, score.txt (+ replay/verdicts.txt, meta.txt) and content-addressed receipts
# under evidence/ONE-LAYER-<1|2>/receipts/. Steps:
#   1 clean tree, contract hash, build ola + ola_runner from this commit
#   2 ola untar: sealed hash check, member validation, extract WITHOUT corpus/LABELS (private dir)
#   3 pass A: every case run in the sandbox (ola_runner), verdict + receipt
#   4 pass B: a second clean extraction and execution; verdicts must be byte-identical
#   5 LABELS extracted only now; isolation probes (each blocked AND logged, each with a control;
#     the evidence probe aims at a private copy of verdicts.txt, the real one is made read-only)
#   6 rules and harness re-hashed (unchanged), tally -> score.txt (GATE PASS|FAIL)
# experiment (V2 section 3.3) follows the same steps with sets/<set_id>/ instead of one verdicts.txt,
# the task references as set `references`, the read_labels probe aimed at a hidden/reference.osc, and
# score.txt ending RUN VALID|INVALID.
# Untracked output allowed by the clean-tree check: evidence/ONE-LAYER-1/(runs|receipts)/ and
# evidence/ONE-LAYER-2/(runs|receipts|generations)/ (generations = the exp445 driver's request logs).
# Selftest-only knobs (each forces GATE FAIL / RUN INVALID): OLA_ALLOW_DIRTY=1, OLA_EVIDENCE_ROOT=<dir>.
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

# common start of every scoring command: sets clean, commit, rules_sha, harness, bin, evid ($1 = 1|2)
prepare() {
  [ "$(uname -m)" = aarch64 ] || die "the sandbox (seccomp allow-list) is aarch64-only; refusing to score on $(uname -m)"
  # 1. clean tree: no change to tracked files, nothing untracked except earlier harness output
  clean=1
  local dirty
  dirty="$(git status --porcelain=v1 --untracked-files=all |
           grep -Ev '^\?\? evidence/(ONE-LAYER-1/(runs|receipts)|ONE-LAYER-2/(runs|receipts|generations))/' || true)"
  if [ -n "$dirty" ]; then
    if [ "${OLA_ALLOW_DIRTY:-0}" = 1 ]; then clean=0; echo "ola.sh: DIRTY tree allowed for a selftest: the gate cannot pass" >&2
    else echo "$dirty" | head -20 >&2; die "refusing a dirty git tree (commit or stash first)"; fi
  fi
  evid="$ROOT/evidence/ONE-LAYER-$1"
  if [ -n "${OLA_EVIDENCE_ROOT:-}" ]; then evid="$OLA_EVIDENCE_ROOT"; clean=0; fi
  if [ -n "${OLA_PROBE_ONLY:-}" ]; then
    echo "ola.sh: OLA_PROBE_ONLY is a selftest knob: cleared, every probe runs, the gate cannot pass" >&2
    clean=0
  fi
  unset OLA_PROBE_ONLY
  [ "$(sha "$HERE/HARNESS_INTERFACE.md")" = "$(kv interface_sha256)" ] || die "HARNESS_INTERFACE.md does not match the pinned v1 hash"
  if [ "$1" = 2 ]; then
    [ "$(sha "$HERE/HARNESS_INTERFACE_V2.md")" = "$(kv interface_v2_sha256)" ] || die "HARNESS_INTERFACE_V2.md does not match the pinned v2 hash"
  fi
  commit="$(git rev-parse HEAD)"
  [ "$clean" = 1 ] || commit="$commit-dirty"
  make -s PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 one-layer-acceptance-tools >&2
  bin="$ROOT/${OUT_DIR:-build}/one_layer"   # mk/one-layer-acceptance.mk: OLA_DIR = $(OUT_DIR)/one_layer
  rules_sha="$(sha "$HERE/RULES")"
  harness="$(harness_sha)"
  # 2. private directory (never inside the repo, never visible to a candidate)
  priv="$(mktemp -d "${TMPDIR:-/tmp}/ola-priv.XXXXXXXX")"   # global: the EXIT trap removes it
  chmod 700 "$priv"
  trap 'rm -rf "${priv:-}"' EXIT
  mkdir "$priv/A" "$priv/B" "$priv/scratch" "$priv/keys" "$priv/outside"
  head -c 32 /dev/urandom > "$priv/keys/decoy.key"   # a key-shaped secret the probes try to read
}

write_meta() { # write_meta <corpus_sha> <run dir>
  {
    echo "commit=$commit"; echo "harness_sha256=$harness"; echo "rules_sha256=$rules_sha"
    echo "corpus_sha256=$1"; echo "time_ms=$(kv time_ms)"; echo "mem_mb=$(kv mem_mb)"
  } > "$priv/meta"
  cp "$priv/meta" "$2/meta.txt"
}

new_run() { # new_run <corpus_sha> -> sets run, run_id
  run_id="$(date -u +%Y%m%dT%H%M%SZ)-${1:0:12}-$$"
  mkdir -p "$evid/runs" "$evid/receipts"
  run="$evid/runs/$run_id"
  mkdir "$run" || die "run directory $run exists: never overwritten"
}

probes() { # probes <dir holding hidden/inputs + hidden/expected> <labels target> <tar> <evidence copy>
  echo "isolation probes:" >&2
  "$bin/ola" probes "$bin/ola_runner" "$priv/scratch" "$run/probes.log" "$(kv time_ms)" "$(kv mem_mb)" \
    hidden_inputs="$1/hidden/inputs" expected="$1/hidden/expected" \
    labels="$2" tar="$3" key="$priv/keys/decoy.key" evidence="$4" \
    scorer="$HERE/ola.sh" rules="$HERE/RULES" outside="$priv/outside/escape" ||
    echo "ola.sh: isolation probes did not all pass" >&2
}

unchanged() { # step 6: nothing the candidates could reach changed the rules or the harness
  [ "$(sha "$HERE/RULES")" = "$rules_sha" ] || { echo "ola.sh: RULES changed during the run" >&2; clean=0; }
  [ "$(harness_sha)" = "$harness" ] || { echo "ola.sh: harness changed during the run" >&2; clean=0; }
}

score() { # score <1|2> <corpus.tar> <SEALED.sha256>
  local V="$1"; shift
  local cmd=score; [ "$V" = 2 ] && cmd=score-v2
  [ $# -eq 2 ] || die "usage: ola.sh $cmd <corpus.tar> <SEALED.sha256>"
  local tar sealed; tar="$(realpath -e "$1")" || die "no corpus tarball $1"
  sealed="$(realpath -m "$2")"
  [ -f "$sealed" ] || die "$sealed does not exist: the corpus is not sealed yet; nothing scored"
  local sfx="" pass=pass tally=tally
  [ "$V" = 2 ] && { sfx=2; pass=pass2; tally=tally2; }
  prepare "$V"
  local corpus_sha
  corpus_sha="$("$bin/ola" untar "$tar" "$sealed" "$priv/A" "cases$sfx" | sed -n 's/^corpus_sha256=//p')"
  [ ${#corpus_sha} -eq 64 ] || die "untar gave no corpus hash"
  new_run "$corpus_sha"
  mkdir "$run/replay"
  write_meta "$corpus_sha" "$run"
  echo "ola.sh: run $run_id corpus $corpus_sha" >&2

  # 3. pass A
  echo "pass A:" >&2
  "$bin/ola" "$pass" "$priv/A/corpus" "$run/verdicts.txt" "$evid/receipts" "$priv/scratch" "$bin/ola_runner" "$priv/meta"
  # 4. pass B from a fresh extraction
  echo "pass B (clean replay):" >&2
  "$bin/ola" untar "$tar" "$sealed" "$priv/B" "cases$sfx" >/dev/null
  "$bin/ola" "$pass" "$priv/B/corpus" "$run/replay/verdicts.txt" "$evid/receipts" "$priv/scratch" "$bin/ola_runner" "$priv/meta"
  local replay=0
  "$bin/ola" cmp "$run/verdicts.txt" "$run/replay/verdicts.txt" && replay=1
  chmod a-w "$run/verdicts.txt" "$run/replay/verdicts.txt"   # written once; the probes aim at a copy
  local verdicts_sha; verdicts_sha="$(sha "$run/verdicts.txt")"
  mkdir "$priv/evidence" && cp "$run/verdicts.txt" "$priv/evidence/verdicts.txt" && chmod u+w "$priv/evidence/verdicts.txt"

  # 5. LABELS only now; then the isolation probes against the real private files
  "$bin/ola" untar "$tar" "$sealed" "$priv/A" "labels$sfx" >/dev/null
  local first; first="$(ls "$priv/A/corpus/cases" | LC_ALL=C sort | head -n 1)"
  probes "$priv/A/corpus/cases/$first" "$priv/A/corpus/LABELS" "$tar" "$priv/evidence/verdicts.txt"

  # 6. nothing the candidates could reach changed the rules or the harness
  unchanged
  [ "$(sha "$run/verdicts.txt")" = "$verdicts_sha" ] || { echo "ola.sh: verdicts.txt changed during the probes" >&2; clean=0; }
  if [ "$V" = 2 ]; then
    "$bin/ola" tally2 "$run/verdicts.txt" "$priv/A/corpus/LABELS" "$replay" "$run/probes.log" "$clean" "$priv/A/corpus" > "$run/score.txt"
  else
    "$bin/ola" tally "$run/verdicts.txt" "$priv/A/corpus/LABELS" "$replay" "$run/probes.log" "$clean" > "$run/score.txt"
  fi
  cat "$run/score.txt"
  echo "ola.sh: evidence in ${run#"$ROOT"/}" >&2
}

tasks_public() {
  [ $# -eq 3 ] || die "usage: ola.sh tasks-public <tasks.tar> <TASKS.sha256> <outdir>"
  local tar sealed out; tar="$(realpath -e "$1")" || die "no task tarball $1"
  sealed="$(realpath -m "$2")"
  [ -f "$sealed" ] || die "$sealed does not exist: the tasks are not sealed yet"
  [ "$(sha "$HERE/HARNESS_INTERFACE_V2.md")" = "$(kv interface_v2_sha256)" ] || die "HARNESS_INTERFACE_V2.md does not match the pinned v2 hash"
  mkdir -p "$3"; out="$(realpath -e "$3")"
  [ -z "$(ls -A "$out")" ] || die "$out is not empty"
  make -s PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 one-layer-acceptance-tools >&2
  "$ROOT/${OUT_DIR:-build}/one_layer/ola" untar "$tar" "$sealed" "$out" public
}

# check_sets <candidates_dir> <tasks/cases dir>: the whole run is refused unless every path is
# <set_id>/, <set_id>/<task_id>/ or <set_id>/<task_id>/candidate.osc|candidate.oscir (regular files and
# directories only, known task ids, at most one candidate per task). ola xpass checks it again.
check_sets() {
  local c="$1" cases="$2" p rel set task n
  while IFS= read -r -d '' p; do
    rel="${p#"$c"/}"
    if [ -L "$p" ] || { [ ! -f "$p" ] && [ ! -d "$p" ]; }; then die "refused: $rel is not a regular file or directory"; fi
    IFS=/ read -r set task n <<< "$rel"
    [[ "$set" =~ ^[a-z0-9_-]{1,40}$ ]] && [ "$set" != references ] || die "refused: set id '$set'"
    [ -n "${task:-}" ] || { [ -d "$p" ] || die "refused: $rel is not a set directory"; continue; }
    [ -d "$cases/$task" ] || die "refused: $rel: '$task' is not a task of the corpus"
    if [ -z "${n:-}" ]; then
      [ -d "$p" ] || die "refused: $rel is not a task directory"
      [ "$(find "$p" -mindepth 1 | wc -l)" -le 1 ] || die "refused: $rel has more than one entry"
    else
      [ -f "$p" ] && { [ "$n" = candidate.osc ] || [ "$n" = candidate.oscir ]; } || die "refused: $rel is not a candidate file"
    fi
  done < <(find "$c" -mindepth 1 -print0)
}

# experiment <tasks.tar> <TASKS.sha256> <candidates_dir|-> (- = tasks-check: references only)
experiment() {
  local tar sealed cands=""; tar="$(realpath -e "$1")" || die "no task tarball $1"
  sealed="$(realpath -m "$2")"
  [ -f "$sealed" ] || die "$sealed does not exist: the tasks are not sealed yet; nothing scored"
  if [ "$3" != - ]; then cands="$(realpath -e "$3")" || die "no candidates dir $3"; [ -d "$cands" ] || die "$3 is not a directory"; fi
  prepare 2
  local corpus_sha
  corpus_sha="$("$bin/ola" untar "$tar" "$sealed" "$priv/A" tasks | sed -n 's/^corpus_sha256=//p')"
  [ ${#corpus_sha} -eq 64 ] || die "untar gave no task corpus hash"
  # frozen private copy of the candidates, checked before anything is written
  mkdir "$priv/sets"
  if [ -n "$cands" ]; then
    check_sets "$cands" "$priv/A/tasks/cases"
    cp -R --no-dereference "$cands/." "$priv/sets/"
    check_sets "$priv/sets" "$priv/A/tasks/cases"
  fi
  local sets=(); local s
  while IFS= read -r s; do [ -n "$s" ] && sets+=("$s"); done < <(ls "$priv/sets" | LC_ALL=C sort)
  new_run "$corpus_sha"
  write_meta "$corpus_sha" "$run"
  (cd "$priv/sets" && find . -type f -print0 | LC_ALL=C sort -z | xargs -0 -r sha256sum) > "$run/candidates.sha256"
  echo "ola.sh: run $run_id tasks $corpus_sha sets ${#sets[@]}" >&2
  local replay=1 L
  for L in A B; do
    if [ "$L" = B ]; then "$bin/ola" untar "$tar" "$sealed" "$priv/B" tasks >/dev/null; fi
    for s in references "${sets[@]}"; do
      local out="$run/sets/$s/verdicts.txt" arg="$priv/sets/$s"
      [ "$s" = references ] && arg=REFERENCES
      [ "$L" = B ] && out="$run/sets/$s/replay/verdicts.txt"
      mkdir -p "$(dirname "$out")"
      echo "pass $L set $s:" >&2
      "$bin/ola" xpass "$priv/$L/tasks" "$arg" "$out" "$evid/receipts" "$priv/scratch" "$bin/ola_runner" "$priv/meta"
      if [ "$L" = B ]; then
        "$bin/ola" cmp "$run/sets/$s/verdicts.txt" "$out" || replay=0
        chmod a-w "$run/sets/$s/verdicts.txt" "$out"
      fi
    done
  done
  local verdicts_sha; verdicts_sha="$(sha "$run/sets/references/verdicts.txt")"
  mkdir "$priv/evidence" && cp "$run/sets/references/verdicts.txt" "$priv/evidence/verdicts.txt" && chmod u+w "$priv/evidence/verdicts.txt"
  local first; first="$(ls "$priv/A/tasks/cases" | LC_ALL=C sort | head -n 1)"
  probes "$priv/A/tasks/cases/$first" "$priv/A/tasks/cases/$first/hidden/reference.osc" "$tar" "$priv/evidence/verdicts.txt"
  unchanged
  [ "$(sha "$run/sets/references/verdicts.txt")" = "$verdicts_sha" ] || { echo "ola.sh: verdicts changed during the probes" >&2; clean=0; }
  "$bin/ola" xscore "$run/sets" "$replay" "$run/probes.log" "$clean" > "$run/score.txt"
  awk '$2 != "ACCEPT" { print "ola.sh: reference not accepted: " $1 " " $2 " " $3 }' "$run/sets/references/verdicts.txt" >&2
  cat "$run/score.txt"
  echo "ola.sh: evidence in ${run#"$ROOT"/}" >&2
}

usage="usage: ola.sh score|score-v2 <corpus.tar> <SEALED.sha256> | tasks-public <tasks.tar> <TASKS.sha256> <outdir> | experiment <tasks.tar> <TASKS.sha256> <candidates_dir> | tasks-check <tasks.tar> <TASKS.sha256>"
case "${1:-}" in
  score) shift; score 1 "$@" ;;
  score-v2) shift; score 2 "$@" ;;
  tasks-public) shift; tasks_public "$@" ;;
  experiment) shift; [ $# -eq 3 ] || die "usage: ola.sh experiment <tasks.tar> <TASKS.sha256> <candidates_dir>"; [ "$3" != - ] || die "candidates_dir '-'"; experiment "$@" ;;
  tasks-check) shift; [ $# -eq 2 ] || die "usage: ola.sh tasks-check <tasks.tar> <TASKS.sha256>"; experiment "$1" "$2" - ;;
  *) die "$usage" ;;
esac
