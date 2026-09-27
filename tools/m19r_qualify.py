#!/usr/bin/env python3
"""M19R Gate 1/2 qualification. Permanent receipts are written only on success."""
import argparse
import datetime as dt
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import uuid

OMEGA = Path(__file__).resolve().parents[1]
SHA = re.compile(r"[0-9a-fA-F]{40}\Z")
GATE = re.compile(r"^\s*\[(PASS|FAIL)\]\s+([A-Za-z0-9_:-]+)", re.M)
NAMED_PASS = re.compile(r"^([A-Z][A-Z0-9_]+_PASS):", re.M)
CUDA = re.compile(r"libcuda(?:rt)?\.so|\b(?:cuInit|cuCtx|cuMem|cuStream|cudaMalloc)\b", re.I)


def command(args, cwd, log=None, env=None):
    result = subprocess.run(args, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if log:
        log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{args[0]} exited {result.returncode}; see {log}")
    return result.stdout


def git(repo, *args):
    return command(["git", *args], repo).strip()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def file_digest(path):
    return digest(path.read_bytes())


def canonical(obj):
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode()


def write_immutable_receipt(target, receipt):
    target.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o444)
    with os.fdopen(fd, "wb") as f:
        f.write(json.dumps(receipt, sort_keys=True, indent=2).encode() + b"\n")
        f.flush()
        os.fsync(f.fileno())


def require_passed(events_seen):
    if not events_seen or any(item["status"] != "PASS" for item in events_seen):
        raise RuntimeError("at least one observed gate failed")


def observed_counts(events_seen):
    return {"completed": len(events_seen),
            "passed": sum(item["status"] == "PASS" for item in events_seen),
            "failed": sum(item["status"] == "FAIL" for item in events_seen)}


def events(output, suite):
    found = [{"suite": suite, "id": name, "status": status}
             for status, name in GATE.findall(output)]
    found += [{"suite": suite, "id": name, "status": "PASS"}
              for name in NAMED_PASS.findall(output)]
    return found


def tagged_json(output, tag):
    values = [json.loads(line.split(":", 1)[1]) for line in output.splitlines()
              if line.startswith(tag + ":")]
    if not values:
        raise RuntimeError(f"missing {tag} observation")
    return values


def must_candidate(repo, supplied):
    if not SHA.fullmatch(supplied):
        raise RuntimeError("candidate must be an explicit full 40-hex SHA")
    if git(repo, "rev-parse", "HEAD").lower() != supplied.lower():
        raise RuntimeError(f"{repo.name} HEAD differs from supplied candidate")
    if git(repo, "status", "--porcelain", "--untracked-files=normal"):
        raise RuntimeError(f"{repo.name} candidate tree is dirty")


def physics_binary(physics, output, source):
    headers = physics / "third_party/nvidia-open-580.173.02"
    include = [physics / "nvrm", physics / "m16",
               headers / "src/common/sdk/nvidia/inc",
               headers / "kernel-open/common/inc",
               headers / "kernel-open/nvidia-uvm",
               headers / "src/nvidia/arch/nvalloc/unix/include"]
    args = ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror"]
    for path in include:
        args.extend(["-I", str(path)])
    args += [str(physics / "nvrm/nvrm.c"), str(physics / "m16/m16_native.c"),
             str(physics / source), "-o", str(output)]
    command(args, physics)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--omega-candidate", required=True)
    ap.add_argument("--physics-candidate", required=True)
    ap.add_argument("--physics-dir", type=Path, default=OMEGA.parent / "physics")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--quick", action="store_true", help="development run; no permanent receipt")
    args = ap.parse_args()
    if args.record and args.quick:
        ap.error("--quick cannot emit a permanent receipt")
    physics = args.physics_dir.resolve()
    run_id = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:12]
    run_dir = OMEGA / "build/qual-runs" / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    results = {"run_id": run_id, "commands": [], "status": "FAILED"}
    try:
        must_candidate(OMEGA, args.omega_candidate)
        must_candidate(physics, args.physics_candidate)
        locked = (OMEGA / "physics.lock").read_text().strip()
        if locked.lower() != args.physics_candidate.lower():
            raise RuntimeError("physics.lock differs from supplied physics candidate")
        historical = {p: file_digest(OMEGA / p) for p in git(OMEGA, "ls-files", "evidence").splitlines()}
        commands_manifest = [
            "nvrm/lifecycle_gates", "m16/m16_requalify", "m16/m16_concurrent",
            "omega/m15", "omega/world_lifecycle", "omega/m19", "omega/m19r_soak"]
        environment = os.environ.copy()
        environment["OMEGA_M19R_CANDIDATE"] = args.omega_candidate
        environment["OMEGA_QUAL_RECORD"] = "1"
        environment["PHYSICS_DIR"] = str(physics)
        with open("/tmp/aien-gb10.lock", "w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            command(["make", "clean"], OMEGA)
            command(["make", "-j4", f"PHYSICS_DIR={physics}"], OMEGA,
                    run_dir / "build.log", environment)
            binary = OMEGA / "build/omegatool"
            binary_sha = file_digest(binary)
            suites = []
            for name, source in (("nvrm_lifecycle", "nvrm/lifecycle_gates.c"),
                                 ("m16_requalify", "m16/m16_requalify.c"),
                                 ("m16_concurrent", "m16/m16_concurrent.c")):
                target = run_dir / name
                physics_binary(physics, target, source)
                output = command([str(target)], physics, run_dir / f"{name}.log", environment)
                suites.append((name, output))
            for name, flag in (("m15", "--run-m15-gates"),
                               ("world_lifecycle", "--run-world-lifecycle-gates"),
                               ("m19", "--run-m19-gates")):
                output = command([str(binary), flag], OMEGA,
                                 run_dir / f"{name}.log", environment)
                suites.append((name, output))
            if not args.quick:
                output = command([str(binary), "--run-m19r-soak"], OMEGA,
                                 run_dir / "m19r_soak.log", environment)
                suites.append(("m19r_soak", output))
            dynamic = command(["readelf", "-d", str(binary)], OMEGA,
                              run_dir / "readelf.log")
            symbols = command(["nm", "-u", str(binary)], OMEGA,
                              run_dir / "nm.log")
            if CUDA.search(dynamic + symbols):
                raise RuntimeError("CUDA linkage or undefined symbol found")
            hardware_raw = command(["nvidia-smi", "--query-gpu=pci.bus_id,pci.device_id,driver_version,name,uuid",
                                    "--format=csv,noheader"], OMEGA).strip()
            hardware = {"nvidia_smi_raw": hardware_raw,
                        "driver_proc_version": Path("/proc/driver/nvidia/version").read_text().strip()}
            hardware_digest = digest(canonical(hardware))
        after = {p: file_digest(OMEGA / p) for p in historical}
        if historical != after:
            raise RuntimeError("historical evidence changed during qualification")
        all_events = [item for name, output in suites for item in events(output, name)]
        require_passed(all_events)
        manifest = {"commands": commands_manifest,
                    "observed_test_ids": [f"{item['suite']}:{item['id']}" for item in all_events]}
        manifest_digest = digest(canonical(manifest))
        m19 = tagged_json(dict(suites)["m19"], "M19_OBSERVED_JSON")[-1]
        if m19["m19_gates_completed"] != 18 or m19["m19_gates_passed"] != m19["m19_gates_completed"] or \
           m19["regression_gates_passed"] != m19["regression_gates_completed"]:
            raise RuntimeError("M19 did not complete all 18 gates and regressions")
        if args.quick:
            results.update({"status": "QUICK_PASS", "events": all_events, "m19": m19})
            return 0
        soak_output = dict(suites)["m19r_soak"]
        soak = tagged_json(soak_output, "M19R_SOAK_JSON")[-1]
        samples = tagged_json(soak_output, "M19R_RESOURCE_JSON")
        if not soak["passed"] or soak["cycles"] < 100000 or \
           soak["bytes_churned"] <= 2 * soak["physical_memory_bytes"] or len(samples) != 3:
            raise RuntimeError("long soak criterion failed")
        if [s["phase"] for s in samples] != ["start", "end", "post_destroy"]:
            raise RuntimeError("soak resource snapshots are incomplete")
        predecessor = OMEGA / "evidence/omega_accelerator_world_qualification_receipt.json"
        counts = observed_counts(all_events)
        body = {
            "schema": "AIEN_M19R_QUALIFICATION_V1", "lineage": "M19_REPAIR_BASELINE",
            "run_id": run_id, "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
            "candidate_git_commit": args.omega_candidate.lower(),
            "physics_candidate_git_commit": args.physics_candidate.lower(),
            "candidate_trees_clean": {"omega": True, "physics": True},
            "candidate_binary_sha256": binary_sha,
            "test_manifest_sha256": manifest_digest,
            "test_manifest": manifest,
            "test_results": all_events,
            "observed_gate_results_count": counts["completed"],
            "observed_pass_count": counts["passed"],
            "observed_fail_count": counts["failed"],
            "m19_observations": m19,
            "soak": soak, "resource_samples": samples,
            "hardware_probe": hardware, "hardware_probe_sha256": hardware_digest,
            "predecessor_qualification_sha256": file_digest(predecessor),
            "historical_evidence_sha256": historical,
            "zero_libcuda_linkage": not bool(CUDA.search(dynamic + symbols)),
            "driver_handle_count_limitation": "No authoritative live-handle query is exposed by the current RM interface; driver-acknowledged RM alloc/free balance and process mappings are the strongest available observables.",
        }
        receipt_digest = digest(canonical(body))
        receipt = dict(body, receipt_digest=receipt_digest)
        if args.record:
            target = OMEGA / "evidence/M19R" / f"{receipt_digest}.json"
            write_immutable_receipt(target, receipt)
            results["permanent_receipt"] = str(target)
        else:
            (run_dir / "receipt-preview.json").write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
        results.update({"status": "PASS", "receipt_digest": receipt_digest,
                        "events": all_events, "m19": m19, "soak": soak, "resource_samples": samples})
        return 0
    except Exception as exc:
        results["error"] = str(exc)
        print(f"M19R qualification failed: {exc}", file=sys.stderr)
        return 1
    finally:
        (run_dir / "run.json").write_text(json.dumps(results, sort_keys=True, indent=2) + "\n")
        print(f"M19R run evidence: {run_dir / 'run.json'}")


if __name__ == "__main__":
    raise SystemExit(main())
