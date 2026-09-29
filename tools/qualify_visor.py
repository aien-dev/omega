#!/usr/bin/env python3
"""Omega Visor V1 qualification glue (lane 8). DISPOSABLE.

Runs the make targets and tests/visor/qualification/run_qualification.sh, collects their
counts and writes ONE new content-addressed receipt evidence/VISOR/<sha256>.json plus
evidence/VISOR/VISOR_V1_QUALIFICATION.md. Every assertion lives in the C tests, the
.omega-session goldens and the runner; this file only runs, counts and records.

usage: python3 tools/qualify_visor.py [--skip-regression] [--no-asan]
Builds only into private OUT_DIRs (build/q8r, build/q8asan, build/q8reg); never touches build/.
"""
import argparse, datetime, hashlib, json, os, re, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHYS = "/home/drakestapleton/workspace/physics-r13"
CAPLIB = "/home/drakestapleton/workspace/aienos-authority-c8ab65e/native/capability/out/libaienos_capability.a"
OUT = "build/q8r"; ASAN_OUT = "build/q8asan"; REG_OUT = "build/q8reg"
PF = ["PHYSICS_DIR=/nonexistent", "PHYSICS_LOCK_CHECK=0"]
FREE_SUITES = ["test-visor-semantic", "test-language", "test-visor-verify", "test-visor-evidence",
               "test-visor-machine", "test-visor-realization", "test-visor-console"]
LINKED_SUITES = ["test-visor-world", "test-visor-authority"]
REGRESSION = ["test", "test-m5", "test-m6", "test-m7", "test-m8", "test-m9", "test-m10", "test-m13", "test-m14"]
NEVER = {"test-m12": "GPU (living matvec)", "test-m15": "accelerator/GPU", "test-m17": "Blackwell GPU",
         "test-m18": "GPU", "test-m19": "GPU"}
GATES = {
    "OMEGA_VISOR_SEMANTIC_PASS": {"suites": ["test-visor-semantic", "test-language"], "sections": ["semantic", "determinism"]},
    "OMEGA_VISOR_VERIFY_PASS": {"suites": ["test-visor-verify", "test-visor-evidence", "test-visor-world"], "sections": ["usability"]},
    "OMEGA_VISOR_REALIZE_PASS": {"suites": ["test-visor-machine", "test-visor-realization", "test-visor-console"],
                                 "sections": ["realize", "scriptability"]},
    "OMEGA_VISOR_AUTHORITY_ISOLATION_PASS": {"suites": ["visor-authority-check", "test-visor-authority"],
                                             "sections": ["hostile", "authority"]},
    "OMEGA_VISOR_REGRESSION_PASS": {"suites": REGRESSION + ["visor-physics-free-check"], "sections": []},
}


def sh(cmd, **kw):
    p = subprocess.run(cmd, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                       errors="replace", **kw)
    return p.returncode, p.stdout


def sha_file(path):
    h = hashlib.sha256()
    with open(os.path.join(ROOT, path), "rb") as f:
        h.update(f.read())
    return h.hexdigest()


def tree_state():
    _, head = sh(["git", "rev-parse", "HEAD"])
    _, st = sh(["git", "status", "--short"])
    files = []
    for line in st.splitlines():
        path = line[3:].strip()
        full = os.path.join(ROOT, path)
        if os.path.isfile(full):
            files.append({"status": line[:2].strip(), "path": path, "sha256": sha_file(path)})
        else:
            files.append({"status": line[:2].strip(), "path": path, "sha256": None})
    return head.strip(), st, files


def make(target, out_dir, extra, log):
    rc, out = sh(["make", "OUT_DIR=" + out_dir] + extra + [target])
    log.write("=== make %s (rc=%d)\n%s\n" % (target, rc, out))
    return rc, out


def suite_counts(out):
    """Sum 'PASS n/n' / 'FAIL n/m' lines and omegatool 'TOTAL GATES: n | PASSED: n | FAILED: n' lines."""
    run = failed = 0
    for m in re.finditer(r"^(?:PASS|FAIL)\s+(\d+)/(\d+)\s*$", out, re.M):
        ok, n = int(m.group(1)), int(m.group(2)); run += n; failed += n - ok
    for m in re.finditer(r"TOTAL GATES:\s*(\d+)\s*\|\s*PASSED:\s*(\d+)\s*\|\s*FAILED:\s*(\d+)", out):
        run += int(m.group(1)); failed += int(m.group(3))
    return run, failed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-regression", action="store_true")
    ap.add_argument("--no-asan", action="store_true")
    a = ap.parse_args()
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    log = open(os.path.join(ROOT, "build", "q8-qualify.log"), "w")
    run_id = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    head0, st0, files0 = tree_state()
    suites, reasons = [], []

    # 1. physics-free omega binary + physics-free suites (fresh private OUT_DIR)
    sh(["rm", "-rf", OUT])
    rc, _ = make(OUT + "/omega", OUT, PF, log)
    binary_ok = rc == 0
    for t in FREE_SUITES + ["visor-authority-check", "visor-physics-free-check"]:
        rc, out = make(t, OUT, PF, log)
        n, f = suite_counts(out)
        if t == "visor-authority-check":
            n, f = 1, (0 if "OMEGA_VISOR_AUTHORITY_ISOLATION_PASS" in out and rc == 0 else 1)
        if t == "visor-physics-free-check":
            n, f = 1, (0 if "OMEGA_VISOR_PHYSICS_FREE_BUILD_PASS" in out and rc == 0 else 1)
        suites.append({"target": t, "rc": rc, "tests_run": n, "tests_failed": f,
                       "status": "PASS" if rc == 0 and n > 0 and f == 0 else "FAIL"})
    # 2. runtime-linked suites
    for t in LINKED_SUITES:
        rc, out = make(t, OUT, ["PHYSICS_DIR=" + PHYS, "VISOR_AUTH_CAP_LIB=" + CAPLIB], log)
        n, f = suite_counts(out)
        suites.append({"target": t, "rc": rc, "tests_run": n, "tests_failed": f,
                       "status": "PASS" if rc == 0 and n > 0 and f == 0 else "FAIL"})
    # 3. regression (CPU gates only)
    quiet = [p for p in (os.path.join(ROOT, ".spark-quiet"), "/home/drakestapleton/workspace/omega/.spark-quiet")
             if os.path.exists(p)]
    for t in REGRESSION:
        if a.skip_regression:
            suites.append({"target": t, "status": "NOT_RUN", "reason": "--skip-regression"}); continue
        rc, out = make(t, REG_OUT, ["PHYSICS_DIR=" + PHYS], log)
        n, f = suite_counts(out)
        suites.append({"target": t, "rc": rc, "tests_run": n, "tests_failed": f,
                       "status": "PASS" if rc == 0 and n > 0 and f == 0 else "FAIL"})
    for t, why in NEVER.items():
        suites.append({"target": t, "status": "NOT_RUN", "reason": "GPU/silicon target excluded by lane-8 brief: " + why})
    suites.append({"target": "test-m11", "status": "NOT_RUN", "reason": "not in the lane-8 regression list"})

    # 4. console campaign (runner) on the release binary, then on an ASan+UBSan build
    rc, rout = sh(["bash", "tests/visor/qualification/run_qualification.sh", OUT + "/omega"],
                  env=dict(os.environ, QUAL_WORK=OUT + "/qual-work"))
    log.write("=== runner (rc=%d)\n%s\n" % (rc, rout))
    sections = {m.group(1): {"run": int(m.group(2)), "failed": int(m.group(3))}
                for m in re.finditer(r"^QUAL_SECTION (\S+) run=(\d+) failed=(\d+)$", rout, re.M)}
    hm = re.search(r"^QUAL_HOSTILE run=(\d+) failed_closed=(\d+) crashed=(\d+) noop=(\d+)$", rout, re.M)
    dm = re.search(r"^QUAL_DETERMINISM runs=(\d+) identical=(\w+)$", rout, re.M)
    failed_cases = re.findall(r"^QUAL (\S+) (.+?) FAIL ?(.*)$", rout, re.M)
    asan = {"status": "NOT_RUN"}
    if not a.no_asan:
        sh(["rm", "-rf", ASAN_OUT])
        brc, _ = make(ASAN_OUT + "/omega", ASAN_OUT,
                      PF + ["CC=cc -fsanitize=address,undefined -fno-omit-frame-pointer -g"], log)
        arc, aout = sh(["bash", "tests/visor/qualification/run_qualification.sh", ASAN_OUT + "/omega"],
                       env=dict(os.environ, QUAL_WORK=ASAN_OUT + "/qual-work"))
        log.write("=== runner ASan (rc=%d)\n%s\n" % (arc, aout))
        san = re.search(r"^QUAL hostile no-sanitizer-reports-in-any-transcript (PASS|FAIL)", aout, re.M)
        am = re.search(r"^QUAL_HOSTILE run=(\d+) failed_closed=(\d+) crashed=(\d+)", aout, re.M)
        asan = {"status": "RUN" if brc == 0 else "BUILD_FAILED",
                "sanitizer_reports": "none" if san and san.group(1) == "PASS" else "PRESENT_OR_UNKNOWN",
                "crashed": int(am.group(3)) if am else None,
                "same_case_failures_as_release": sorted(re.findall(r"^QUAL \S+ (.+?) FAIL", aout, re.M)) ==
                sorted(c[1] for c in failed_cases)}

    head1, st1, files1 = tree_state()
    tree_stable = head0 == head1 and files0 == files1

    # 5. gates
    by_target = {s["target"]: s for s in suites}
    gates = {}
    for g, spec in GATES.items():
        run = failed = 0; srcs = []; notrun = []
        for t in spec["suites"]:
            s = by_target.get(t)
            if not s or s["status"] == "NOT_RUN":
                notrun.append(t); continue
            run += s["tests_run"]; failed += s["tests_failed"] + (1 if s["status"] == "FAIL" and s["tests_failed"] == 0 else 0)
            srcs.append("make " + t)
        for sec in spec["sections"]:
            if sec in sections:
                run += sections[sec]["run"]; failed += sections[sec]["failed"]
                srcs.append("run_qualification.sh:" + sec)
            else:
                notrun.append("runner:" + sec)
        if run == 0:
            status = "NOT_RUN"
        else:
            status = "PASS" if failed == 0 and not notrun else "FAIL"
        gates[g] = {"status": status, "tests_run": run, "tests_failed": failed, "sources": srcs}
        if notrun:
            gates[g]["not_run"] = notrun
        if status != "PASS":
            reasons.append("%s %s (%d/%d failed%s)" % (g, status, failed, run,
                                                          "; not run: " + ", ".join(notrun) if notrun else ""))
    for sec, name, detail in failed_cases:
        reasons.append("runner %s: %s FAIL %s" % (sec, name, detail[:140]))
    if not tree_stable:
        reasons.append("tree changed during the run (HEAD or modified-file hashes differ): receipt INVALID")
    verdict = "OMEGA_VISOR_V1_PASS" if all(v["status"] == "PASS" for v in gates.values()) and tree_stable else "OMEGA_VISOR_V1_FAIL"

    def rd(p):
        try:
            return open(p).read().strip()
        except OSError:
            return None
    only_ours = all(f["path"].startswith(("tests/visor/qualification", "tools/qualify_visor.py", "evidence/VISOR"))
                    for f in files0)
    receipt = {
        "schema": "omega-visor-v1-qualification/1",
        "run_id": run_id,
        "candidate_commit": head0,
        "tree_dirty": bool(st0.strip()),
        "tree_dirty_detail": ("only lane-8 files (untracked, not yet committed)" if only_ours and st0.strip()
                              else "see dirty_files"),
        "dirty_files": files0,
        "tree_stable_during_run": tree_stable,
        "physics_commit": rd(os.path.join(ROOT, "physics.lock")),
        "host": {"uname_m": os.uname().machine, "uname_r": os.uname().release,
                 "product_name": rd("/sys/devices/virtual/dmi/id/product_name")},
        "hardware_scope": "host-only: no GPU/silicon claim; no QEMU claim by the Visor (the pre-existing test-m5 gate runs its own QEMU check)",
        "omega_binary": {"path": OUT + "/omega", "built": binary_ok,
                         "sha256": sha_file(OUT + "/omega") if binary_ok else None,
                         "build": "make OUT_DIR=%s PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 %s/omega" % (OUT, OUT)},
        "gates": gates,
        "suites": suites,
        "runner_sections": sections,
        "runner_failed_cases": [{"section": s, "case": n, "detail": d} for s, n, d in failed_cases],
        "hostile_cases": {"run": int(hm.group(1)), "failed_closed": int(hm.group(2)), "crashed": int(hm.group(3)),
                          "benign_noop": int(hm.group(4)),
                          "note": "benign_noop = blank/whitespace/comment lines, a spec'd no-op (exit 0); the rest must exit 1 with an error line"} if hm else None,
        "sanitizer_pass": asan,
        "determinism": {"runs": int(dm.group(1)), "identical": dm.group(2) == "yes",
                        "scope": "same host, fresh processes; text, --json and realization ids"} if dm else None,
        "scriptability": {"section": sections.get("scriptability"),
                          "contract": "exit 0 = every line ok; 1 = at least one line errored; 2 = usage error or unopenable script; "
                                      "--json = one object per non-blank line with keys command/status/class/result/error; "
                                      "no prompt when stdin is not a tty; --script - reads stdin"},
        "ux_findings": UX_FINDINGS,
        "defects": DEFECTS,
        "non_claims": NON_CLAIMS,
        "verdict": verdict,
        "verdict_reasons": reasons,
    }
    body = (json.dumps(receipt, indent=2, sort_keys=False) + "\n").encode()
    evd = os.path.join(ROOT, "evidence", "VISOR")
    os.makedirs(evd, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=evd, prefix=".tmp-")
    with os.fdopen(fd, "wb") as f:
        f.write(body)
    digest = hashlib.sha256(body).hexdigest()
    final = os.path.join(evd, digest + ".json")
    if os.path.exists(final):
        os.unlink(tmp)
    else:
        os.rename(tmp, final)
    write_md(os.path.join(evd, "VISOR_V1_QUALIFICATION.md"), receipt, "evidence/VISOR/" + digest + ".json")
    log.close()
    for g, v in gates.items():
        print("%s=%s (%d run, %d failed)" % (g, v["status"], v["tests_run"], v["tests_failed"]))
    print("RECEIPT evidence/VISOR/%s.json" % digest)
    print("VERDICT %s" % verdict)
    return 0 if verdict == "OMEGA_VISOR_V1_PASS" else 1


UX_FINDINGS = [
    "`true`/`false` echo as 1/0 although `type _` says bool; a user reads 1 as an integer.",
    "`help` shows the source form as `let x = <expr>`, but V0 requires `let x: u64 = <expr>` (the error message does explain).",
    "`help` lists `graph [x]` (argument optional) but `graph` with no argument is an error: 'give a name, _ or id'.",
    "`alternatives <value>` prints a doubled prefix: `error: alternatives: alternatives: only programs have alternative realizations in V1`.",
    "`effects <non-effect>` prints `'x' is not an EFFECT object` without the `error:` prefix in text mode (JSON correctly says status error; exit code is 1).",
    "`why <x>` is described as 'explain where x came from' but only explains realizations; on a value it says `use realize x first`, and `realize x` then refuses a value, a dead end.",
    "`authorize x`, `execute _` and `delete x` give parser jargon ('unexpected x after the end of the statement') instead of 'unknown command'; only `mint`/`grant`/... are reserved words, `authorize`/`submit`/`execute` are not.",
    "Errors raised before parsing are labelled with the command name `unknown` (`error: unknown: invalid UTF-8 at byte 4`, `error: unknown: line too long`).",
    "`id` prints the same hash three times (id, canonical_sha256) plus canonical_len; the relation is not explained.",
    "After `realize _`, `_` silently becomes the realization, so `inspect _`/`type _` now refer to a different object than one line earlier.",
    "Every u64 ADD has the same realization id (`realized` = the ADD operation, not the apply), so `x + y` and `x + 1` share one realization id; correct per spec but surprising in `bindings`/`compare`.",
    "`run _ a b` on a binary-apply realization runs the operation on the given numbers, not on the object's operands; nothing in the output says the object was not what ran.",
    "Machine/realization output uses hex profile codes (`profile 0x01 vs machine 0x01`), `physics flag=set seal=builder-constant placeholder`, and C function names (`omega_machine_estimate_latency(...)`, `omega_exec_native_f3`) as explanations; not readable without the C source.",
    "`verify` row INVARIANTS reports 399 generic checks that are 'not object-specific'; a user may read the PASS as evidence about their object.",
    "`evidence <name>` for a session object always says `no evidence`: receipts are repository files, not linked to session objects; the question 'what evidence supports this object' cannot be answered in V1.",
    "Blank lines produce no JSON object while comment-only lines produce one (`kind: none`); a script driver counting lines must know this.",
    "Commands after `quit` in --command/--script mode are silently dropped (exit 0).",
]

DEFECTS = [
    {"id": "D1", "severity": "high (fixed in ec2ec0b, found at 4fab549)",
     "what": "The ./omega shipped at commit 4fab549 (sha256 543d874d...d4d3) crashes on every realize/run path: "
             "`./omega --command 'let x: u64 = 7' --command 'let y: u64 = 11' --command 'x + y' --command 'realize _'` -> exit 139 (SIGSEGV in om_realization_show); "
             "`realize b` on a bool -> '*** stack smashing detected ***', exit 134. Campaign on that binary: 37 failures, 6 hostile crashes. "
             "Cause: tools/omega.c's object was not rebuilt when src/visor/visor.h changed (omega_main.d missing from VISOR_DEPS), so the binary mixed two struct layouts. "
             "The in-process console test (146/146) could not see it. ec2ec0b adds omega_main.d; a clean-checkout build of 4fab549 was not tested."},
    {"id": "D2", "severity": "medium (wrong output, exit 0)",
     "what": "`run` does not check arity. `fn f(x: u64) -> u64 { x * 2 + 1 }` then `run f` prints 1 (f(0), x silently 0); `run f 5 6` prints 11 (6 ignored). "
             "On `x + y` (7, 11) after `realize _`: `run _ 1` prints 12 (first operand replaced, second kept), `run _ 1 2 3` prints 3 (third ignored). All status ok, exit 0."},
    {"id": "D3", "severity": "low",
     "what": "`./omega --script /` (a directory) exits 0 with no output instead of refusing the script (exit 2 like a missing file)."},
    {"id": "D4", "severity": "low (cosmetic)",
     "what": "`alternatives x` on a value: 'error: alternatives: alternatives: only programs ...' (prefix doubled); `effects x` text-mode error lacks 'error:'; `help` says `graph [x]` but `graph` alone is an error."},
]

NON_CLAIMS = [
    "Host-only qualification on one DGX Spark (aarch64); no GPU, no silicon, no QEMU claim by the Visor.",
    "No measured or qualified cost: the Visor fills only predicted (static) and estimated (assumed machine model) cost; measured/qualified are ABSENT by design.",
    "Machine identity is the assumed canonical profile chosen from DMI, not an observed descriptor; the physics seal is a builder placeholder.",
    "World is unattached in the omega binary: `world` observes nothing; the snapshot API is covered only by the runtime-linked test-visor-world.",
    "Language V0 scope only: explicit-width unsigned integers, bool, let, + - * / & |, fn chains (x op c)...; realize/run cover u64 binary ADD/SUB/MUL/AND/OR only.",
    "Effects: the language cannot build an EFFECT object, so `run` on an effect is not reachable from the console; effect-request refusal is covered only by the C hostile suite (test-visor-authority) and the link check.",
    "GPU targets test-m12, test-m15, test-m17, test-m18, test-m19 and all Blackwell paths were not run (lane-8 brief); test-m11 was not in the list.",
    "The pre-existing test-m5 gate launches QEMU and writes build/qemu_omega_runner.bin under the shared build/ directory (hard-coded path in omegatool).",
    "The e2e session golden (tests/visor/sessions/e2e.expected) is compared host-independently only after ec2ec0b; the lane-8 goldens mask the machine block, machine name/id and estimated cycles.",
    "Determinism is shown across fresh processes on this host only, not across hosts, compilers or ABIs (golden bytes are pinned to LP64/aarch64).",
    "Visor V1 is not claimed safe against a hostile local user with write access to the binary or the evidence directory.",
]


def write_md(path, r, receipt_path):
    L = []
    L.append("# Omega Visor V1 qualification (lane 8)\n")
    L.append("Verdict: **%s**  \nReceipt: `%s`  \nCommit: `%s` (tree dirty: %s, %s)  \nRun: %s on %s %s (%s)\n" % (
        r["verdict"], receipt_path, r["candidate_commit"], r["tree_dirty"], r["tree_dirty_detail"], r["run_id"],
        r["host"]["uname_m"], r["host"]["uname_r"], r["host"]["product_name"]))
    L.append("Scope: %s\n" % r["hardware_scope"])
    L.append("## The five gates in plain words\n")
    L.append("| Gate | What it checks | Result | Tests | Failed |\n|---|---|---|---|---|")
    plain = {
        "OMEGA_VISOR_SEMANTIC_PASS": "Same meaning gets the same fingerprint (7, 07, 0x07, 0b111; spacing; comments); different widths differ; repeat runs match byte for byte",
        "OMEGA_VISOR_VERIFY_PASS": "The checker and the evidence viewer report what is really there, and a new user can find it from `help`",
        "OMEGA_VISOR_REALIZE_PASS": "Machine code built for an expression gives the same answer as the language; cost is labelled estimate, never measured; scripting behaves",
        "OMEGA_VISOR_AUTHORITY_ISOLATION_PASS": "The console can ask but never grant; bad or hostile input is refused cleanly and never crashes it",
        "OMEGA_VISOR_REGRESSION_PASS": "The older CPU milestone gates still pass and the tool still builds without the physics checkout",
    }
    for g, v in r["gates"].items():
        L.append("| %s | %s | %s | %d | %d |" % (g, plain[g], v["status"], v["tests_run"], v["tests_failed"]))
    L.append("")
    h = r["hostile_cases"] or {}
    L.append("Hostile inputs: %s tried, %s refused cleanly, %s crashed, %s harmless blank/comment lines." % (
        h.get("run"), h.get("failed_closed"), h.get("crashed"), h.get("benign_noop")))
    d = r["determinism"] or {}
    L.append("Repeatability: %s runs, identical: %s.  Memory-checker build: %s.\n" % (
        d.get("runs"), d.get("identical"), json.dumps(r["sanitizer_pass"])))
    L.append("## Why the verdict is what it is\n")
    for x in r["verdict_reasons"] or ["All five gates passed."]:
        L.append("- " + x)
    L.append("\n## Defects found\n")
    for x in r["defects"]:
        L.append("- **%s** (%s): %s" % (x["id"], x["severity"], x["what"]))
    L.append("\n## Test suites\n")
    L.append("| Target | Result | Tests | Failed | Note |\n|---|---|---|---|---|")
    for s in r["suites"]:
        L.append("| %s | %s | %s | %s | %s |" % (s["target"], s["status"], s.get("tests_run", "-"), s.get("tests_failed", "-"),
                                                s.get("reason", "")))
    L.append("\n## Usability findings (not failures unless listed as defects)\n")
    for x in r["ux_findings"]:
        L.append("- " + x)
    L.append("\n## What this does NOT claim\n")
    for x in r["non_claims"]:
        L.append("- " + x)
    L.append("\n## How to re-run\n")
    L.append("```\npython3 tools/qualify_visor.py            # everything, writes a new receipt\n"
             "make OUT_DIR=build/q8r PHYSICS_DIR=/nonexistent PHYSICS_LOCK_CHECK=0 build/q8r/omega\n"
             "bash tests/visor/qualification/run_qualification.sh build/q8r/omega   # console campaign only\n```\n")
    with open(path, "w") as f:
        f.write("\n".join(L))


if __name__ == "__main__":
    sys.exit(main())
