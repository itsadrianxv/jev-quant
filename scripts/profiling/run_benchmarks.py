#!/usr/bin/env python3
"""Run isolated, CPU-pinned benchmark repetitions and retain evidence."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys

SCENARIOS = ["snapshot-1", "snapshot-5", "snapshot-boundary", "incremental-32",
             "incremental-512", "incremental-8192", "incremental-boundary",
             "pipeline-hold", "pipeline-cancel", "pipeline-fill"]


def capture(command):
    result = subprocess.run(command, text=True, capture_output=True, check=False)
    return {"command": command, "returncode": result.returncode,
            "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def callgrind_counts(run_dir, completed):
    dumps = [p for p in run_dir.glob("callgrind.out.*")
             if "measured-batch" in p.read_text(encoding="utf-8")]
    if len(dumps) != 1:
        raise RuntimeError("expected exactly one measured-batch Callgrind dump")
    lines = dumps[0].read_text(encoding="utf-8").splitlines()
    events = next(line.split()[1:] for line in lines if line.startswith("events:"))
    totals = [line.split()[1:] for line in lines if line.startswith("totals:")]
    if not totals:
        totals = [line.split()[1:] for line in lines if line.startswith("summary:")]
    counts = dict(zip(events, map(int, totals[-1])))
    if counts.get("Ir", 0) <= 0:
        raise RuntimeError("empty measured instruction count")
    with (run_dir / "callgrind-annotate.txt").open("w", encoding="utf-8") as stream:
        subprocess.run(["callgrind_annotate", "--inclusive=yes", "--threshold=99", str(dumps[0])],
                       stdout=stream, stderr=subprocess.STDOUT, check=True)
    return {"events": counts, "ir_per_operation": counts["Ir"] / completed,
            "measured_dump": dumps[0].name}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--revision", required=True,
                        help="source revision supplied before copying to a .git-free build")
    parser.add_argument("--scenarios", nargs="+", choices=SCENARIOS, default=SCENARIOS)
    parser.add_argument("--modes", nargs="+", choices=["throughput", "latency", "stages", "callgrind"],
                        default=["throughput", "latency", "stages"])
    parser.add_argument("--updates", type=int, default=100000)
    parser.add_argument("--warmup", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--cpu", type=int, default=None)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.repetitions < 1 or args.timeout < 1:
        parser.error("repetitions and timeout must be positive")
    args.bench = args.bench.resolve(strict=True)
    args.out = args.out.resolve()
    allowed_cpus = sorted(os.sched_getaffinity(0))
    cpu = args.cpu if args.cpu is not None else allowed_cpus[-1]
    if cpu not in allowed_cpus:
        parser.error("CPU is not in the current affinity mask")
    if "callgrind" in args.modes:
        for tool in ["valgrind", "callgrind_annotate"]:
            if not shutil.which(tool):
                parser.error(f"required tool missing: {tool}")
    args.out.mkdir(parents=True, exist_ok=False)
    cache = args.bench.parent / "CMakeCache.txt"
    cache_text = cache.read_text(encoding="utf-8") if cache.exists() else ""
    manifest = {"schema_version": 1, "revision": args.revision,
                "binary": str(args.bench), "binary_sha256": digest(args.bench),
                "platform": platform.platform(), "kernel": platform.release(),
                "cpu": cpu, "available_cpus": allowed_cpus,
                "lscpu": capture(["lscpu"]), "python": sys.version,
                "arguments": {k: str(v) if isinstance(v, Path) else v for k, v in vars(args).items()},
                "build_configuration": [line for line in cache_text.splitlines()
                                        if line.startswith(("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "CMAKE_CXX_FLAGS"))]}
    for tool in ["valgrind", "perf"]:
        if shutil.which(tool):
            manifest[tool] = capture([tool, "--version"])
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    rows = []
    identities = {}
    for scenario in args.scenarios:
        for mode in args.modes:
            if mode == "stages" and not scenario.startswith("pipeline-"):
                continue
            for repetition in range(1, args.repetitions + 1):
                run_dir = args.out / scenario / mode / str(repetition)
                run_dir.mkdir(parents=True)
                command = ["taskset", "-c", str(cpu)]
                if mode == "callgrind":
                    command += ["valgrind", "--tool=callgrind", "--collect-atstart=no",
                                "--branch-sim=yes", f"--callgrind-out-file={run_dir / 'callgrind.out'}"]
                command += [str(args.bench), "--scenario", scenario, "--mode", mode,
                            "--updates", str(args.updates), "--warmup", str(args.warmup),
                            "--seed", str(args.seed), "--revision", args.revision,
                            "--out", str(run_dir / "data")]
                (run_dir / "command.json").write_text(json.dumps(command, indent=2) + "\n", encoding="utf-8")
                with (run_dir / "stdout.json").open("w", encoding="utf-8") as stdout, \
                        (run_dir / "stderr.txt").open("w", encoding="utf-8") as stderr:
                    subprocess.run(command, stdout=stdout, stderr=stderr, timeout=args.timeout, check=True)
                summary = json.loads((run_dir / "data" / "summary.json").read_text(encoding="utf-8"))
                if not summary["valid"] or summary["samples_expected"] != summary["samples_recorded"]:
                    raise RuntimeError("invalid benchmark summary")
                identity = (summary["workload_hash64"], summary["result_checksum"], summary["counts"])
                if scenario in identities and identity != identities[scenario]:
                    raise RuntimeError(f"business workload changed across modes/repetitions: {scenario}")
                identities[scenario] = identity
                prefix = {"scenario": scenario, "mode": mode, "repetition": repetition}
                if mode == "callgrind":
                    profile = callgrind_counts(run_dir, summary["completed"])
                    (run_dir / "profile.json").write_text(json.dumps(profile, indent=2) + "\n", encoding="utf-8")
                    rows.append(dict(prefix, metric="Ir/operation", value=profile["ir_per_operation"]))
                if mode == "throughput":
                    for metric in ["operations_per_second", "ns_per_operation"]:
                        rows.append(dict(prefix, metric=metric, value=summary[metric]))
                for metric, values in summary["distributions"].items():
                    for statistic in ["mean_ns", "p50_ns", "p90_ns", "p99_ns", "max_ns"]:
                        rows.append(dict(prefix, metric=f"{metric}.{statistic}", value=values[statistic]))
                print(f"validated {scenario} {mode} repetition={repetition}", flush=True)
    with (args.out / "runs.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=["scenario", "mode", "repetition", "metric", "value"], delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)
    grouped = {}
    for row in rows:
        grouped.setdefault((row["scenario"], row["mode"], row["metric"]), []).append(row["value"])
    with (args.out / "comparison.tsv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, delimiter="\t")
        writer.writerow(["scenario", "mode", "metric", "runs", "median", "min", "max"])
        for key, values in sorted(grouped.items()):
            writer.writerow([*key, len(values), statistics.median(values), min(values), max(values)])
    (args.out / "COMPLETE").write_text("All requested runs validated.\n", encoding="utf-8")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"benchmark experiment failed: {error}", file=sys.stderr)
        sys.exit(1)
