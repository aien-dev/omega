# CAND-3 qualification window W2: declared before it runs

W2 replaces nothing. It is a new, separately declared attempt under rule 1 of `DECLARED-ATTEMPTS.md`
(sha256 9f4b8b91713226038143ec0ec17425d6dba51845aceb75207dcabaf5bcbdf184, unchanged). That file's attempts A1 to A8,
pass rules, machine conditions, invalid-run rule and lane order apply to W2 word for word. R15 (A7) stays NOT ATTEMPTED;
G7 and G8 stay NOT_RUN.

## W1 (kept, INVALID)

W1 started 2026-10-06T01:44:15Z under quietlock hold q49857-1791251055-b53601a9. Every lane stopped at its first line
(`cd .../wt-omega-chip: No such file or directory`, exit 2): the clean chip worktrees and survey checkouts
(`cand3_setup_trees.sh`) had not been created. No build, no chip work, no receipt. Under the invalid-run rule (the
program under test never started; harness defect before any test) W1 is INVALID. Its directory
`evidence-out/CAND3-LIVING-f816473` is sealed read-only with SHA256SUMS (sha256 of that file: 56c43277d52895fd11c78d069d703cdea3c355bc59ae358369a53fc4f1bc63c3) and INVALID-W1.txt.

## What changed for W2 (and nothing else)

1. `cand3_setup_trees.sh` was run (git worktree add only): omega 97ee275 clean, physics 6d7cf0d clean, code-identical to
   f816473 apart from the R16 map and crumbs; survey checkouts aegis-runtime f4e8709, aienos bbad5e4, sovereign-core
   80e071a (omega.lock = f816473). The starter now reruns this check before taking the hold and does not start if it fails.
2. Each window writes its own evidence directory: `cand3_ladder.sh` and `run_window.sh` append `-$WINDOW_TAG`
   (W2: `evidence-out/CAND3-LIVING-f816473-w2`), so W1 is never written to. `run_window.sh` also copies this file.
   `cand3_ladder.sh` is otherwise byte-identical to W1's copy (now sha256 42f56ba74a0f87d56e2442438d44b3e0bba5ca28109edfb2aed10ffdcdb5036b).
3. Preflight checked before declaring: `sudo -n` passwordless and `gdb` present (G6 host test prerequisites);
   ARGUS is read with `git archive` at argus.lock b375dca, which is present in the ARGUS checkout.

Window W2 runs once, under quietlock owner cand3-campaign, starting when no qemu runs and the 1-minute load is below 1.5.
Written 2026-10-06T01:45:51Z, before W2 started.
