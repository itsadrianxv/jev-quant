"""Process-level checks for the bounded simex live runner."""

import json
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

from scripts import simex_live


SERVER = """#!/usr/bin/env python3
import signal
import time

signal.signal(signal.SIGTERM, lambda *_: exit(0))
print('event=simex_ready phase=CONTINUOUS override=1', flush=True)
while True:
    time.sleep(0.1)
"""

TRADING = """#!/usr/bin/env python3
import signal
import sys
import time

config = __import__('json').load(open(sys.argv[1], encoding='utf-8'))
assert config['runtime']['run_seconds'] == 0
signal.signal(signal.SIGTERM, lambda *_: (print('event=simex_live_result market_ready={ready} decisions=0 executions=0 provider_failures=0', flush=True), exit(0)))
time.sleep({delay})
{ready_line}
while True:
    time.sleep(0.1)
"""


class SimexLiveRunnerTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.server = self.root / "server"
        self.server.write_text(SERVER, encoding="utf-8")
        self.server.chmod(0o755)
        self.trading = self.root / "trading"
        self.config = self.root / "config.json"
        self.server_config = self.root / "server.json"
        self.config.write_text(json.dumps({"venue": {"type": "simex", "tcp_port": 1, "udp_port": 2},
                                           "runtime": {"client_id": 3}}), encoding="utf-8")
        self.server_config.write_text(json.dumps({"tcp_port": 1, "udp_destination_port": 2,
                                                  "client_id": 3, "max_run_seconds": 60,
                                                  "phase_override": "CONTINUOUS",
                                                  "participant_simulator": {"enabled": True}}), encoding="utf-8")
        self.args = type("Args", (), {"server": self.server, "server_config": self.server_config,
                                        "trading": self.trading, "config": self.config,
                                        "duration": 1, "output": self.root / "output"})()

    def trading_script(self, delay, ready):
        self.trading.write_text(TRADING.format(delay=delay, ready=int(ready),
                                               ready_line="print('event=simex_market_state state=ready', flush=True)"
                                               if ready else "pass"), encoding="utf-8")
        self.trading.chmod(0o755)

    def test_rejects_missing_phase_override(self):
        config = json.loads(self.server_config.read_text(encoding="utf-8"))
        del config["phase_override"]
        self.server_config.write_text(json.dumps(config), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "phase_override=CONTINUOUS"):
            simex_live.run(self.args)
        self.assertFalse(self.args.output.exists())

    def test_rejects_mismatched_server_phase(self):
        self.server.write_text(SERVER.replace("phase=CONTINUOUS", "phase=CLOSED"), encoding="utf-8")
        self.trading_script(0, True)
        self.assertEqual(simex_live.run(self.args), 1)
        result = json.loads((self.args.output / "result.json").read_text(encoding="utf-8"))
        self.assertIn("continuous phase override", result["error"])

    def test_observation_duration_begins_after_market_ready(self):
        self.trading_script(0.4, True)
        started = time.monotonic()
        with patch.object(simex_live, "READINESS_TIMEOUT", 1):
            self.assertEqual(simex_live.run(self.args), 3)
        self.assertGreaterEqual(time.monotonic() - started, 1.35)
        result = json.loads((self.args.output / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(result["status"], "decision_chain_unverified")
        self.assertTrue(result["market_ready"])

    def test_market_readiness_timeout(self):
        self.trading_script(0, False)
        with patch.object(simex_live, "READINESS_TIMEOUT", 1):
            self.assertEqual(simex_live.run(self.args), 2)
        result = json.loads((self.args.output / "result.json").read_text(encoding="utf-8"))
        self.assertEqual(result["status"], "waiting_for_market_data")
        self.assertFalse(result["market_ready"])


if __name__ == "__main__":
    unittest.main()
