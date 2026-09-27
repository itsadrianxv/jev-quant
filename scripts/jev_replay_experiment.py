#!/usr/bin/env python3
"""Replay captured Jev requests against the real provider, current vs previous wording.

Consumes the opt-in transport captures produced by scripts/simex_live.py --capture
and the jev_trading binary, then replays the exact request bytes to the same
endpoint/model.  The previous-wording variant reverts only the wording fields
changed by commit 381cd97 (see src/tools/jev_variant.cpp).  A global call cap
and wall-clock limit are enforced; every attempt is recorded, including
failures, and responses never enter the TradeEngine.
"""

import argparse
import json
import re
import subprocess
import time
from pathlib import Path


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def discover_captures(captures_root, wanted_states):
    """Select flat, ready, successfully parsed live captures, one per evaluation id."""
    selected = {}
    for run_dir in sorted(p for p in Path(captures_root).iterdir() if p.is_dir()):
        for attempt_dir in sorted(p for p in run_dir.iterdir() if p.is_dir()):
            request_path = attempt_dir / "request.json"
            metadata_path = attempt_dir / "metadata.json"
            response_path = attempt_dir / "response.raw"
            if not (request_path.exists() and metadata_path.exists() and response_path.exists()):
                continue
            metadata = read_json(metadata_path)
            if metadata.get("role") != "live" or metadata.get("parse_outcome") != "ok":
                continue
            request = read_json(request_path)
            state = request["state"]
            if state["position"]["net"] != 0:
                continue
            if not state["depth"]["bids"] or not state["depth"]["asks"]:
                continue
            evaluation_id = state["evaluation_id"]
            if evaluation_id in selected:
                continue
            selected[evaluation_id] = {
                "capture_id": f"{run_dir.name}/{attempt_dir.name}",
                "directory": str(attempt_dir),
                "evaluation_id": evaluation_id,
                "endpoint": metadata.get("endpoint"),
                "model": metadata.get("model"),
                "timeout_ms": metadata.get("timeout_ms"),
                "sequence": metadata.get("sequence"),
            }
            if len(selected) >= wanted_states:
                return list(selected.values())
    return list(selected.values())


def local_status(captures_root):
    """Applied vs stale-dropped evaluation ids, from the TradeEngine log."""
    applied, stale = set(), set()
    engine_log = Path(captures_root).parent / "components" / "trade-engine-1.log"
    if not engine_log.exists():
        return {"applied": [], "stale": []}
    text = engine_log.read_text(encoding="utf-8", errors="replace")
    for match in re.finditer(r"event=decision_consumed client_id=\d+ evaluation_id=(\d+)", text):
        applied.add(int(match.group(1)))
    for match in re.finditer(r"event=stale_decision_dropped client_id=\d+ evaluation_id=(\d+)", text):
        stale.add(int(match.group(1)))
    return {"applied": sorted(applied), "stale": sorted(stale)}


def probabilities_text(answer):
    if not isinstance(answer, dict):
        return "-"
    probabilities = answer.get("probabilities")
    if not isinstance(probabilities, dict):
        return "-"
    parts = [f"{key}={probabilities[key]}" for key in sorted(probabilities)]
    return " ".join(parts) + f" conf={answer.get('confidence', '-')}"


def choice_text(answer):
    if not isinstance(answer, dict):
        return "-"
    return answer.get("choice", "-")


def run_replay(jev_replay, capture, variant, out, label, endpoint, model, timeout_ms):
    command = [str(jev_replay), "--capture", str(capture), "--out", str(out),
               "--label", label, "--variant", variant]
    if endpoint:
        command += ["--endpoint", endpoint]
    if model:
        command += ["--model", model]
    if timeout_ms:
        command += ["--timeout-ms", str(timeout_ms)]
    started = time.monotonic()
    completed = subprocess.run(command, capture_output=True, text=True)
    wall_seconds = time.monotonic() - started
    summary = None
    for line in completed.stdout.splitlines():
        stripped = line.strip()
        if stripped.startswith("{"):
            summary = json.loads(stripped)
    if summary is None:
        summary = {"mode": "replay", "label": label, "variant": variant,
                   "capture": str(capture), "error": "no summary line",
                   "stderr": completed.stderr[-400:], "exit_code": completed.returncode}
    summary["orchestrator_wall_seconds"] = round(wall_seconds, 3)
    return summary


def write_comparison(path, rows, caps):
    def cell(value):
        return str(value).replace("|", "\\|")
    lines = [
        "# Jev exact-replay comparison",
        "",
        "Derived artifact; original capture bytes are preserved untouched.",
        f"Call cap: {caps['max_calls']}, used: {caps['used_calls']}, "
        f"truncated: {caps['truncated']}, wall-clock limit: {caps['time_limit_seconds']}s.",
        "",
        "| capture | variant | repeat | request sha256 | HTTP | bias choice/probabilities/conf | "
        "intent choice/probabilities/conf | parsed intent | local status | latency ms |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(
            cell(row[column]) for column in (
                "capture_id", "variant", "repeat", "request_sha256", "http_outcome",
                "bias", "intent", "parsed_intent", "local_status", "latency_ms")) + " |")
    Path(path).write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--captures-root", type=Path, required=True,
                        help="Observation directory containing captures/<run_id>/<attempt>/")
    parser.add_argument("--jev-replay", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--states", type=int, default=2)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--variants", default="current,previous")
    parser.add_argument("--max-calls", type=int, default=20,
                        help="Total provider-call budget including nothing else (captures excluded)")
    parser.add_argument("--time-limit-seconds", type=int, default=1800)
    parser.add_argument("--endpoint", default=None)
    parser.add_argument("--model", default=None)
    parser.add_argument("--timeout-ms", type=int, default=None)
    args = parser.parse_args()

    variants = [variant.strip() for variant in args.variants.split(",") if variant.strip()]
    captures = discover_captures(args.captures_root, args.states)
    if len(captures) < args.states:
        print(json.dumps({"error": "insufficient captures", "found": len(captures),
                          "wanted": args.states}))
        return 2
    status = local_status(args.captures_root)

    args.out.mkdir(parents=True, exist_ok=False)
    parse_only = {}
    for capture in captures:
        label = f"parse-only-s{capture['sequence']:03d}"
        completed = subprocess.run(
                [str(args.jev_replay), "--capture", capture["directory"],
                 "--out", str(args.out), "--label", label, "--parse-only"],
                capture_output=True, text=True)
        for line in completed.stdout.splitlines():
            if line.strip().startswith("{"):
                parse_only[capture["capture_id"]] = json.loads(line.strip())
                break

    trials = []
    used_calls = 0
    truncated = False
    deadline = time.monotonic() + args.time_limit_seconds
    for repeat in range(1, args.repeats + 1):
        for capture in captures:
            for variant in variants:
                if used_calls >= args.max_calls or time.monotonic() >= deadline:
                    truncated = True
                    break
                label = f"r{repeat}-s{capture['sequence']:03d}-v{variant[0]}"
                summary = run_replay(args.jev_replay, capture["directory"], variant, args.out,
                                     label, args.endpoint or capture["endpoint"],
                                     args.model or capture["model"], args.timeout_ms or capture["timeout_ms"])
                summary["repeat"] = repeat
                summary["local_status"] = (
                        "applied" if capture["evaluation_id"] in status["applied"]
                        else "stale" if capture["evaluation_id"] in status["stale"] else "unknown")
                trials.append(summary)
                used_calls += 1
            if truncated:
                break
        if truncated:
            break

    rows = []
    for capture in captures:
        decoded = parse_only.get(capture["capture_id"], {})
        raw = decoded.get("raw") or {}
        bias = raw.get("bias", {})
        intent = raw.get("intent", {})
        rows.append({
            "capture_id": capture["capture_id"], "variant": "live", "repeat": 0,
            "request_sha256": (decoded.get("request_sha256") or "-")[:12],
            "http_outcome": "live-ok", "bias": probabilities_text(bias),
            "intent": probabilities_text(intent),
            "parsed_intent": (decoded.get("parsed") or {}).get("intent", "-"),
            "local_status": ("applied" if capture["evaluation_id"] in status["applied"]
                             else "stale" if capture["evaluation_id"] in status["stale"]
                             else "unknown"),
            "latency_ms": "-",
        })
    for trial in trials:
        raw = trial.get("raw") or {}
        verification = trial.get("verification") or {}
        rows.append({
            "capture_id": trial.get("capture_id", "-"), "variant": trial.get("variant", "-"),
            "repeat": trial.get("repeat", "-"),
            "request_sha256": (trial.get("request_sha256") or "-")[:12],
            "http_outcome": f"curl={trial.get('curl_outcome', '-')} status={trial.get('http_status', '-')}",
            "bias": probabilities_text(raw.get("bias", {})),
            "intent": probabilities_text(raw.get("intent", {})),
            "parsed_intent": (trial.get("parsed") or {}).get("intent", "-"),
            "local_status": trial.get("local_status", "-"),
            "latency_ms": trial.get("elapsed_ms", "-"),
        })

    caps = {"max_calls": args.max_calls, "used_calls": used_calls, "truncated": truncated,
            "time_limit_seconds": args.time_limit_seconds}
    write_comparison(args.out / "comparison.md", rows, caps)
    (args.out / "trials.json").write_text(json.dumps({
        "captures": captures, "parse_only": parse_only, "local_status": status,
        "trials": trials, "caps": caps}, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"trials": len(trials), "used_calls": used_calls,
                      "truncated": truncated, "out": str(args.out)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
