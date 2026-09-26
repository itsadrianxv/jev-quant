#!/usr/bin/env python3
"""Run the real simex process and OpenRouter-backed Jev, retaining validation evidence."""

import argparse
import json
from pathlib import Path
import re
import signal
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


def stop(process):
    if process is None or process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run(args):
    if not 1 <= args.duration <= 86400:
        raise ValueError("Duration must be between 1 and 86400 seconds")
    config = json.loads(args.config.read_text(encoding="utf-8"))
    server_config = json.loads(args.server_config.read_text(encoding="utf-8"))
    venue = config.get("venue", {})
    if venue.get("type") != "simex":
        raise ValueError("The live runner requires venue.type=simex")
    for field, expected in (("tcp_port", server_config["tcp_port"]),
                            ("udp_port", server_config["udp_destination_port"])):
        if venue.get(field) != expected:
            raise ValueError(f"Client/server {field} mismatch")
    if config["runtime"]["client_id"] != server_config["client_id"]:
        raise ValueError("Client/server participant mismatch")
    if args.duration + 10 > server_config.get("max_run_seconds", 28800):
        raise ValueError("Server lifetime must exceed the live run by at least 10 seconds")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config["runtime"]["run_seconds"] = args.duration
    config.setdefault("logging", {})["output_directory"] = str(output / "components")
    run_config = output / "config.json"
    run_config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    server_log = output / "simex.log"
    trading_log = output / "jev.log"
    server = trading = None
    report = {"status": "failed", "output": str(output)}
    exit_code = 1
    try:
        with server_log.open("wb") as server_stream, trading_log.open("wb") as trading_stream:
            # TODO(simex-counterparty): Once scenario startup is defined, decide
            # how this runner selects a scenario and observes counterparty readiness.
            # Transport readiness alone must not be treated as available liquidity.
            server = subprocess.Popen([str(args.server.resolve()), str(args.server_config.resolve())],
                                      cwd=ROOT, stdout=server_stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 10
            while "event=simex_ready " not in server_log.read_text(encoding="utf-8", errors="replace"):
                if server.poll() is not None:
                    raise RuntimeError("Simex exited before readiness; see simex.log")
                if time.monotonic() >= deadline:
                    raise RuntimeError("Simex readiness timeout")
                time.sleep(0.05)
            # Readiness is signaled through the log, not a probe connection that
            # would consume the v1 server's single participant connection.
            trading = subprocess.Popen([str(args.trading.resolve()), str(run_config)], cwd=ROOT,
                                       stdout=trading_stream, stderr=subprocess.STDOUT)
            timeout = args.duration + config.get("jev", {}).get("timeout_ms", 4000) / 1000 + 20
            try:
                returncode = trading.wait(timeout=timeout)
            except subprocess.TimeoutExpired as error:
                raise RuntimeError("Trading process exceeded its bounded lifetime") from error
            if returncode != 0:
                raise RuntimeError(f"Trading process exited with code {returncode}; see jev.log")
            text = trading_log.read_text(encoding="utf-8", errors="replace")
            result = re.search(r"event=simex_live_result market_ready=(\d+) decisions=(\d+) executions=(\d+) provider_failures=(\d+)", text)
            if not result:
                raise RuntimeError("Trading process did not report validation evidence")
            market, decisions, executions, failures = map(int, result.groups())
            report.update(market_ready=bool(market), decisions=decisions,
                          executions=executions, provider_failures=failures)
            # TODO(simex-counterparty): Choose live scenario coverage and observation
            # windows for fills, partial fills, and cancel races after liquidity is
            # available. Keep hold-only runs unverified for execution; do not inject
            # orders or override real Jev decisions to satisfy a scenario.
            if not market:
                report["status"], exit_code = "waiting_for_market_data", 2
            elif not decisions:
                report["status"], exit_code = "decision_chain_unverified", 3
            elif not executions:
                report["status"], exit_code = "decision_chain_passed_trading_loop_unverified", 4
            else:
                report["status"], exit_code = "trading_loop_passed", 0
    except (OSError, RuntimeError) as error:
        report["error"] = str(error)
    finally:
        stop(trading)
        stop(server)
        (output / "result.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return exit_code


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=ROOT.parent / "simex/build/jev-integration/simex_server")
    parser.add_argument("--server-config", type=Path, default=ROOT.parent / "simex/server.json")
    parser.add_argument("--trading", type=Path, default=ROOT / "build/simex-integration/jev_trading")
    parser.add_argument("--config", type=Path, default=ROOT / "config.simex.json")
    parser.add_argument("--duration", type=int, default=60)
    parser.add_argument("--output", type=Path, default=ROOT / ".scratch/simex-venue-adapter" / time.strftime("live-%Y%m%d-%H%M%S"))
    raise SystemExit(run(parser.parse_args()))
