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
READINESS_TIMEOUT = 10


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
    if server_config.get("phase_override") != "CONTINUOUS":
        raise ValueError("Live validation requires phase_override=CONTINUOUS")
    if not server_config.get("participant_simulator", {}).get("enabled", False):
        raise ValueError("Live validation requires an enabled participant simulator")
    if args.duration + 3 * READINESS_TIMEOUT > server_config.get("max_run_seconds", 28800):
        raise ValueError("Server lifetime must cover readiness, the live run, and shutdown")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config["runtime"]["run_seconds"] = 0
    config.setdefault("logging", {})["output_directory"] = str(output / "components")
    if args.capture:
        # Opt-in transport-boundary diagnostic capture; the binary records the
        # exact request/response bytes per HTTP attempt under this directory.
        config.setdefault("jev", {})["capture"] = {"directory": str(output / "captures")}
    run_config = output / "config.json"
    run_config.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    server_log = output / "simex.log"
    trading_log = output / "jev.log"
    server = trading = None
    report = {"status": "failed", "output": str(output)}
    exit_code = 1
    working_dir = Path(args.working_dir).resolve() if args.working_dir else ROOT
    try:
        with server_log.open("wb") as server_stream, trading_log.open("wb") as trading_stream:
            server = subprocess.Popen([str(args.server.resolve()), str(args.server_config.resolve())],
                                      cwd=working_dir, stdout=server_stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + READINESS_TIMEOUT
            while True:
                ready = re.search(r"event=simex_ready ([^\r\n]*)", server_log.read_text(encoding="utf-8", errors="replace"))
                if ready:
                    if not re.search(r"(?:^| )phase=CONTINUOUS(?: |$)", ready.group(1)) or not re.search(
                            r"(?:^| )override=1(?: |$)", ready.group(1)):
                        raise RuntimeError("Simex did not start with the requested continuous phase override")
                    break
                if server.poll() is not None:
                    raise RuntimeError("Simex exited before readiness; see simex.log")
                if time.monotonic() >= deadline:
                    raise RuntimeError("Simex readiness timeout")
                time.sleep(0.05)
            trading = subprocess.Popen([str(args.trading.resolve()), str(run_config)], cwd=working_dir,
                                       stdout=trading_stream, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + READINESS_TIMEOUT
            market_ready = False
            while time.monotonic() < deadline:
                if "event=simex_market_state state=ready" in trading_log.read_text(encoding="utf-8", errors="replace"):
                    market_ready = True
                    break
                if trading.poll() is not None:
                    raise RuntimeError(f"Trading process exited with code {trading.returncode}; see jev.log")
                if server.poll() is not None:
                    raise RuntimeError("Simex exited before market readiness; see simex.log")
                time.sleep(0.05)
            if market_ready:
                deadline = time.monotonic() + args.duration
                worker_log = output / "components" / "jev-worker.log"
                while time.monotonic() < deadline:
                    if trading.poll() is not None:
                        raise RuntimeError(f"Trading process exited with code {trading.returncode}; see jev.log")
                    if server.poll() is not None:
                        raise RuntimeError("Simex exited during live validation; see simex.log")
                    if args.max_provider_calls and worker_log.exists():
                        observations = worker_log.read_text(encoding="utf-8", errors="replace").count("event=evaluation_batch_drained")
                        if observations >= args.max_provider_calls:
                            report["stop_reason"] = "provider_call_limit"
                            break
                    time.sleep(0.05)
            stop(trading)
            if trading.returncode != 0:
                raise RuntimeError(f"Trading process exited with code {trading.returncode}; see jev.log")
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
            if not market_ready or not market:
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
    parser.add_argument("--max-provider-calls", type=int, default=0,
                        help="Stop the observation after this many drained evaluations (0 = duration only)")
    parser.add_argument("--capture", action="store_true",
                        help="Enable opt-in request/response capture under OUTPUT/captures")
    parser.add_argument("--working-dir", default=None,
                        help="Working directory for both processes (default: repo root). "
                             "The trading process reads its .env from this directory.")
    parser.add_argument("--output", type=Path, default=ROOT / ".scratch/simex-venue-adapter" / time.strftime("live-%Y%m%d-%H%M%S"))
    raise SystemExit(run(parser.parse_args()))
