#!/usr/bin/env python3
"""Summarize complete SimNow submissions; never interpret Callgrind timing."""
import argparse
import csv
import json
import math
from pathlib import Path


def summarize(directory):
    directory = Path(directory)
    meta = json.loads((directory / 'capture.json').read_text())
    if meta['mode'] != 'native':
        raise ValueError('Callgrind timing is not native latency; inspect instruction profiles instead')
    if not meta['healthy'] or meta['overflow'] or not meta['window_completed']:
        raise ValueError('Capture failed, overflowed, or ended before the grace deadline')
    groups = {}
    seen = set()
    with (directory / 'samples.csv').open(newline='') as file:
        rows = list(csv.DictReader(file))
    if len(rows) != meta['samples']:
        raise ValueError('Sample count mismatch')
    new_count = 0
    for row in rows:
        kind = row['type']
        if kind not in ('NEW', 'CANCEL'):
            raise ValueError('Unknown request type')
        key = (kind, row['ticker_id'], row['order_id'])
        if key in seen:
            raise ValueError('Duplicate submission identity')
        seen.add(key)
        times = [int(row[f't{i}']) for i in range(1, 9)]
        first = 0 if kind == 'NEW' else 4
        if any(t <= 0 for t in times[first:]) or any(b < a for a, b in zip(times[first:], times[first+1:])):
            raise ValueError('Missing or reversed timestamp')
        if kind == 'NEW':
            new_count += 1
            if int(row['source_id']) <= 0 or int(row['evaluation_id']) <= 0:
                raise ValueError('Missing source correlation')
        start = meta['start_ns']
        if not start <= times[first] < start + meta['window_ns'] or times[-1] >= start + meta['window_ns'] + meta['grace_ns']:
            raise ValueError('Sample outside capture bounds')
        group = groups.setdefault(f"{kind}/{row['offset']}", {})
        for i in range(first, 7):
            group.setdefault(f't{i+2}-t{i+1}', []).append(times[i+1]-times[i])
        group.setdefault(f't8-t{first+1}', []).append(times[7]-times[first])
    if new_count != meta['new_samples']:
        raise ValueError('New-order count mismatch')
    if not new_count:
        raise ValueError('No complete new-order samples; live acceptance remains pending')
    result = {'unit': 'ns', 'population': 'complete submissions with immediate CTP return code zero', 'groups': {}}
    for name, metrics in groups.items():
        result['groups'][name] = {}
        for metric, values in metrics.items():
            values.sort()
            count = len(values)
            result['groups'][name][metric] = {
                'n': count, 'min': values[0], 'max': values[-1],
                'p50': values[math.ceil(count*.50)-1],
                'p95': values[math.ceil(count*.95)-1],
                'p99': values[math.ceil(count*.99)-1],
                'tail_sample_warning': count < 1000,
            }
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(summarize(args.directory), indent=2))
    except (ValueError, KeyError, OSError) as error:
        parser.exit(2, f'{error}\n')
