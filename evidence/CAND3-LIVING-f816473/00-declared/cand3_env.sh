# CAND-3 window constants. Sourced by cand3_ladder.sh, run_window.sh and cand3_setup_trees.sh.
# Fill the two placeholders at freeze time (and nowhere else); every script refuses to run while one is left.
OMEGA_FINAL="f816473df391bc4fbcf99df722edd70ed26ebef1"       # omega main commit that contains #309 (and any other CAND-3 omega PR): the CAND-3 code commit
SC_FINAL="80e071a3ef700dbe31b1b081a957eebdcbf5ffc3"             # sovereign-core main commit after the omega.lock bump PR (omega.lock == OMEGA_FINAL)
HARNESS_SHA="97ee27584cda0d8eaca136f31a44293cd36c9b02"       # omega commit the harness runs from: OMEGA_FINAL, or a map-only descendant (guard checks)
PHYS_SHA=6d7cf0d4d8eb2cda7b512100ff6058e25dbb3ddf     # omega physics.lock at origin/main 21092e0 and at #309 (unchanged)
AIENOS_SHA=bbad5e4250e57f8cbd1be4cf1390109aa65ef92c   # omega aienos.lock (unchanged)
ARGUS_SHA=b375dcaa2887d53cda407d773c2e4e489a0b1365    # omega argus.lock (unchanged)
AEGIS_RUNTIME_SHA=f4e8709                              # R16 survey checkout used for CAND-2; re-verify against the map section 8.6 at freeze
W=$HOME/workspace/cand3-campaign/cand3
EVID=$HOME/workspace/evidence-out
BUILD_DIGESTS=$EVID/CAND3-BUILD-A/digests.txt          # written by cand3_build.sh; the window re-hashes its own builds against it
case "$OMEGA_FINAL$SC_FINAL$HARNESS_SHA" in *'<'*) echo "cand3_env.sh: placeholders not filled (OMEGA_FINAL, SC_FINAL, HARNESS_SHA)"; return 2 2>/dev/null || exit 2;; esac
