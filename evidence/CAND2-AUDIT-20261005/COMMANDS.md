# CAND-2 evidence: how each directory was made

Exact commands are in the run files themselves (`command.txt` per step, `00-declared/*.sh`, `CAND2-BUILD-*/cand2_build.sh`).
This file only points at them. Times are UTC unless a log says otherwise.

| Directory | Command source | Published at |
|---|---|---|
| `CAND2-BUILD-A`, `CAND2-BUILD-B` | `runners/run_builds.sh` calls `cand2_build.sh A|B <root> <omega> <aienos> <physics> <sovereign-core>`; stdout in `logs/buildA.out`, `logs/buildB.out`, exit codes in `logs/builds.done`. The script itself is also in each build directory. | `CAND2-BUILD-A/cand2_build.sh`, `CAND2-BUILD-B/cand2_build.sh` |
| `CAND2-ATTN-IDENTITY` | `README.txt` says: gpu_attention_test built at omega cb06d08 and 79a805d, same worktree path, `OUT_DIR=build/ab`, `make -j6 build/ab/gpu_attention_test`. No script file was kept for this step. | `CAND2-ATTN-IDENTITY/README.txt` |
| `CAND2-LIVING-79a805d` quiet window | `00-declared/run_window.sh` (window driver), `00-declared/cand2_ladder.sh` (ladder lane), `00-declared/collect_receipts.sh` (builds `CLAIM-INDEX.md` and `receipts/`). Their sha256 are in `00-declared/sha256.txt`; each is also listed in `ORIGINAL-SHA256SUMS`. Per step: `<step>/command.txt`, `exit.txt`, `seconds.txt`, `machine-before/`, `machine-after/`. Window log: `logs/window.out`, `00-declared/window.txt`. | `CAND2-LIVING-79a805d/00-declared/` |
| `A7b-R15` | `00-declared/A7b/cand2_r15b.sh`, declared in `00-declared/A7b/DECLARED-ATTEMPT-A7b.md` before it ran; stdout in `logs/a7b.out`. | `CAND2-LIVING-79a805d/00-declared/A7b/` |
| CHIPWAIT | `CAND2-LIVING-79a805d/CHIPWAIT/command.txt` (`tools/chipwait_campaign.sh ... --runs 3`) and per run `CHIPWAIT/campaign/run-00N/command.txt` (`m19r_qualify.sh ... --run-id run-00N`). | see PUBLICATION-NOTES.md, section CHIPWAIT |
The four scripts and two declared-attempt files under `CAND2-LIVING-79a805d/00-declared/` were compared byte for byte
with the originals in `~/workspace/cand3-campaign/cand2/` on the Spark at publication time: all identical.
