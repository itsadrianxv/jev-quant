#!/usr/bin/env python3
"""Run the real SimNow application with a bounded performance capture."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import tempfile


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--config', required=True, type=Path)
    parser.add_argument('--revision', required=True)
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--mode', choices=('native', 'callgrind'), default='native')
    parser.add_argument('--window', type=int, default=300)
    parser.add_argument('--grace', type=int, default=120)
    parser.add_argument('--capacity', type=int, default=10000)
    args = parser.parse_args()
    if not 0 < args.window <= 86400 or not 0 < args.grace <= 86400 or not 0 < args.capacity <= 1000000:
        parser.error('Invalid capture bounds')
    config = json.loads(args.config.read_text())
    if config.get('venue', {}).get('type') != 'simnow':
        parser.error('The experiment requires the real SimNow venue')
    duration = config.get('runtime', {}).get('run_seconds', 0)
    if duration and duration < args.window + args.grace:
        parser.error('runtime.run_seconds must be zero or cover window + grace')
    binary = args.binary.resolve(strict=True)
    output = args.out.resolve()
    output.mkdir(parents=True, exist_ok=False)
    config['performance'] = {'enabled': True, 'mode': args.mode, 'window_seconds': args.window,
                             'grace_seconds': args.grace, 'capacity': args.capacity,
                             'output_directory': str(output)}
    config.setdefault('logging', {})['output_directory'] = str(output / 'logs')
    metadata = {'revision': args.revision, 'binary_sha256': sha256(binary),
                'source_config_sha256': sha256(args.config), 'mode': args.mode,
                'utc_start': datetime.now(timezone.utc).isoformat(),
                'platform': platform.platform(), 'cpu': platform.processor(),
                'affinity': sorted(os.sched_getaffinity(0)),
                'window_seconds': args.window, 'grace_seconds': args.grace,
                'capacity': args.capacity,
                'runtime': {key: config.get('runtime', {}).get(key) for key in
                            ('client_id', 'evaluation_interval_ms', 'decision_order_quantity', 'queue_capacity', 'run_seconds')},
                'risk': {key: config.get('risk', {}).get(key) for key in ('max_order_size', 'max_position', 'max_loss')},
                'logging': {'verbose_market_data': config['logging'].get('verbose_market_data', False)},
                'live_acceptance': 'pending_manual_evidence_review'}
    # Never archive the full configuration or environment: either may contain secrets.
    prefix = []
    if args.mode == 'callgrind':
        metadata['valgrind_version'] = subprocess.check_output(['valgrind', '--version'], text=True).strip()
        prefix = ['valgrind', '--tool=callgrind', '--collect-atstart=no', '--separate-threads=yes',
                  '--cache-sim=no', '--branch-sim=no', f'--callgrind-out-file={output}/callgrind.out.%p']
    manifest = output / 'run.json'
    manifest.write_text(json.dumps(metadata, indent=2) + '\n')
    path = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', suffix='.json', prefix='simnow-performance-', delete=False) as file:
            path = Path(file.name)
            json.dump(config, file)
        with (output / 'console.log').open('w') as log:
            process = subprocess.Popen(prefix + [str(binary), str(path)], stdout=log, stderr=subprocess.STDOUT,
                                       start_new_session=True)
            try:
                code = process.wait()
            except KeyboardInterrupt:
                os.killpg(process.pid, signal.SIGINT)
                code = process.wait()
        metadata['returncode'] = code
        return code
    finally:
        if path is not None:
            path.unlink(missing_ok=True)
        metadata['utc_end'] = datetime.now(timezone.utc).isoformat()
        manifest.write_text(json.dumps(metadata, indent=2) + '\n')


if __name__ == '__main__':
    raise SystemExit(main())
