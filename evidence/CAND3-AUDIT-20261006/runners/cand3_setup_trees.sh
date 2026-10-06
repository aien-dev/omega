#!/usr/bin/env bash
# Create the clean chip worktrees and the R16 survey checkouts at the CAND-3 commits (git worktree add only; no build, no chip).
# Same layout as CAND-2 (cand2/wt-omega-chip, wt-physics-chip, survey/{aegis-runtime,aienos,aien-sovereign-core}).
set -eu
. "$(dirname "$0")/cand3_env.sh"
add() { # repo dir sha
  [ -d "$2" ] || git -C "$1" worktree add -q --detach "$2" "$3"
  [ "$(git -C "$2" rev-parse HEAD | cut -c1-${#3})" = "$3" ] && [ -z "$(git -C "$2" status --porcelain)" ] || { echo "NOT CLEAN/NOT AT $3: $2"; exit 3; }
}
git -C $HOME/workspace/omega fetch -q origin; git -C $HOME/workspace/physics fetch -q origin
add $HOME/workspace/omega "$W/wt-omega-chip" "$HARNESS_SHA"
add $HOME/workspace/physics "$W/wt-physics-chip" "$PHYS_SHA"
add $HOME/workspace/r16-survey/aegis-runtime "$W/survey/aegis-runtime" "$AEGIS_RUNTIME_SHA"
add $HOME/workspace/r16-survey/aienos "$W/survey/aienos" "$AIENOS_SHA"
add $HOME/workspace/r16-survey/aien-sovereign-core "$W/survey/aien-sovereign-core" "$SC_FINAL"
[ "$(tr -d '[:space:]' < "$W/survey/aien-sovereign-core/omega.lock")" = "$OMEGA_FINAL" ] || { echo "sc omega.lock != OMEGA_FINAL"; exit 3; }
echo "trees ready"
