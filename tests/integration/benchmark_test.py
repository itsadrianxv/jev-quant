"""Check benchmark business work and independently recompute reported quantiles."""
import csv
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile

bench = sys.argv[1]
scenarios = ["snapshot-1", "snapshot-5", "snapshot-boundary", "incremental-32",
             "incremental-512", "incremental-8192", "incremental-boundary",
             "pipeline-hold", "pipeline-cancel", "pipeline-fill"]

with tempfile.TemporaryDirectory() as temporary:
    root = Path(temporary)
    index = 0

    def run(scenario, mode, extra=(), success=True):
        global index
        index += 1
        out = root / str(index)
        result = subprocess.run([bench, "--scenario", scenario, "--mode", mode,
                                 "--updates", "32", "--warmup", "8", "--revision", "test-fixture",
                                 "--out", str(out), *extra], capture_output=True, text=True, timeout=60)
        if not success:
            assert result.returncode != 0, result.stdout
            assert not (out / "summary.json").exists()
            return result.stderr
        assert result.returncode == 0, result.stderr
        summary = json.loads((out / "summary.json").read_text())
        assert summary["valid"] and summary["completed"] == 32
        assert summary["samples_recorded"] == summary["samples_expected"]
        if mode in ("latency", "stages"):
            groups = {}
            with (out / "samples.csv").open() as stream:
                rows = list(csv.DictReader(stream))
            assert len(rows) == summary["samples_recorded"]
            for row in rows:
                groups.setdefault(row["metric"], []).append(int(row["elapsed_ns"]))
            assert groups.keys() == summary["distributions"].keys()
            for name, values in groups.items():
                values.sort()
                report = summary["distributions"][name]
                assert report["count"] == len(values)
                assert math.isclose(report["mean_ns"], sum(values) / len(values))
                for label, q in [("p50_ns", .5), ("p90_ns", .9), ("p99_ns", .99)]:
                    assert report[label] == values[math.ceil(len(values) * q) - 1]
                assert report["max_ns"] == values[-1]
        return summary

    result_checksums = {}
    for scenario in scenarios:
        reports = [run(scenario, mode) for mode in
                   (["throughput", "latency", "stages"] if scenario.startswith("pipeline-")
                    else ["throughput", "latency"])]
        assert len({r["result_checksum"] for r in reports}) == 1
        result_checksums[scenario] = reports[0]["result_checksum"]
        assert len({r["workload_hash64"] for r in reports}) == 1
        counts = reports[0]["counts"]
        if scenario == "pipeline-hold":
            assert counts["requests"] == counts["filled"] == 0
            assert reports[-1]["samples_recorded"] == 64
        if scenario == "pipeline-cancel":
            assert counts["new"] == counts["cancel"] == counts["accepted"] == counts["canceled"] == 16
            assert reports[-1]["samples_recorded"] == 96
        if scenario == "pipeline-fill":
            assert counts["requests"] == counts["accepted"] == counts["filled"] == 32
            assert counts["cancel"] == 0
            assert reports[-1]["samples_recorded"] == 128
            assert reports[-1]["distributions"]["decision.CLOSE.NEW"]["count"] == 16
        if scenario.startswith("incremental-") and scenario != "incremental-boundary":
            assert counts == {"ADD": 8, "CANCEL": 8, "MODIFY": 16}
    # The same BBO with deeper populated levels must remain observable.
    assert result_checksums["snapshot-1"] != result_checksums["snapshot-5"]
    assert "completeness" in run("pipeline-fill", "stages", ["--sample-capacity", "1"], success=False)
    assert "align" in run("pipeline-fill", "latency", ["--updates", "3"], success=False)
    assert "unsigned" in run("snapshot-1", "latency", ["--seed", "-1"], success=False)
    assert "stages requires" in run("snapshot-1", "stages", success=False)
    assert "callgrind" in run("snapshot-1", "callgrind", success=False).lower()
print("benchmark workload, mode equivalence, quantiles, and failure checks passed")
