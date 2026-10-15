#!/usr/bin/env python3
"""Run a ten-minute real-Jev SimNow cycle test and retain HTTP evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile


def summarize(console, returncode):
    positions = [int(value) for value in re.findall(
        r'event=live_position_updated order_id=\d+ position=(-?\d+)', console)]
    previous = 0
    cycles = 0
    valid_sequence = True
    for position in positions:
        if previous == 0 and abs(position) == 1:
            pass
        elif abs(previous) == 1 and position == 0:
            cycles += 1
        else:
            valid_sequence = False
        previous = position
    results = re.findall(
        r'event=simnow_live_result market_ready=(\d+) decisions=(\d+) executions=(\d+) '
        r'provider_failures=(\d+) final_position=(-?\d+)', console)
    final = list(map(int, results[-1])) if results else None
    passed = bool(returncode == 0 and final and final[0] == 1 and final[3] == 0
                  and final[4] == 0 and previous == 0 and valid_sequence
                  and final[2] == len(positions) and cycles >= 20 and len(positions) >= 40)
    return {'passed': passed, 'completed_cycles': cycles, 'fill_events': len(positions),
            'valid_one_contract_sequence': valid_sequence,
            'final_position': final[4] if final else None,
            'provider_failures': final[3] if final else None,
            'returncode': returncode, 'target_cycles': 20, 'target_fill_events': 40,
            'run_seconds': 600, 'holding_seconds_target': [5, 15],
            'holding_duration_verified': False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--config', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text(encoding='utf-8'))
    if config.get('venue', {}).get('type') != 'simnow':
        parser.error('Cycle testing requires SimNow')
    # Preserve all risk limits; the application validates the one-contract contract.
    runtime = config.setdefault('runtime', {})
    if runtime.get('decision_order_quantity', 1) != 1:
        parser.error('Cycle testing requires one-contract orders')
    runtime.update(run_seconds=600, evaluation_interval_ms=5000)
    config.pop('performance', None)
    binary = args.binary.resolve(strict=True)
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config.setdefault('jev', {}).update(simnow_cycle_test=True,
                                       capture={'directory': str(output / 'captures')})
    config.setdefault('logging', {})['output_directory'] = str(output / 'logs')
    path = None
    interrupted = False
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', suffix='.json',
                                         prefix='simnow-cycle-', delete=False) as file:
            path = Path(file.name)
            json.dump(config, file)
        with (output / 'console.log').open('w', encoding='utf-8') as log:
            process = subprocess.Popen([str(binary), str(path)], stdout=log,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            try:
                code = process.wait(timeout=720)
            except (KeyboardInterrupt, subprocess.TimeoutExpired):
                interrupted = True
                os.killpg(process.pid, signal.SIGINT)
                try:
                    code = process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    code = process.wait()
        result = summarize((output / 'console.log').read_text(encoding='utf-8'), code)
        result['interrupted'] = interrupted
        result['passed'] = result['passed'] and not interrupted
        result['http_capture_present'] = any((output / 'captures').glob('*/*/request.json'))
        result['passed'] = result['passed'] and result['http_capture_present']
        (output / 'summary.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
        print(json.dumps(result))
        return 0 if result['passed'] else 1
    finally:
        if path is not None:
            path.unlink(missing_ok=True)


if __name__ == '__main__':
    raise SystemExit(main())
