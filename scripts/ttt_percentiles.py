#!/usr/bin/env python3
"""Turn a TTT latency log into per-segment percentile statistics.

Reads the dedicated TTT producer log (one `event=<kind> ... elapsed_ns=<ns>`
line per consumed event, plus drop records without elapsed_ns) and prints a
TSV table: segment, count, mean_ns, p50_ns, p90_ns, p99_ns, max_ns.

Percentiles use the nearest-rank method over ascending-sorted samples:
index = max(1, ceil(q * n)) - 1. Drop records are counted per segment but
carry no latency statistics (printed as "-").

Usage:
  ttt_percentiles.py <ttt-log> [--check <expected-tsv>]

--check compares the emitted table against the expected TSV (numeric fields
within 0.01) and exits non-zero on mismatch. Standard library only.
"""

import argparse
import math
import sys

HEADER = ["segment", "count", "mean_ns", "p50_ns", "p90_ns", "p99_ns", "max_ns"]
PERCENTILES = (("p50_ns", 0.50), ("p90_ns", 0.90), ("p99_ns", 0.99))


def extract_field(line, prefix):
    start = line.find(prefix)
    if start < 0:
        return None
    start += len(prefix)
    end = start
    while end < len(line) and not line[end].isspace():
        end += 1
    return line[start:end]


def parse_log(path):
    counts = {}
    latencies = {}
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            event = extract_field(line, "event=")
            if event is None or not event.startswith("ttt_"):
                continue
            counts[event] = counts.get(event, 0) + 1
            elapsed = extract_field(line, "elapsed_ns=")
            if elapsed is None:
                continue
            try:
                latencies.setdefault(event, []).append(int(elapsed))
            except ValueError:
                print(f"malformed elapsed_ns value: {elapsed!r}", file=sys.stderr)
    return counts, latencies


def nearest_rank(sorted_values, quantile):
    rank = max(1, math.ceil(quantile * len(sorted_values)))
    return sorted_values[rank - 1]


def build_rows(counts, latencies):
    rows = []
    for segment in sorted(counts):
        samples = sorted(latencies.get(segment, []))
        row = {"segment": segment, "count": str(counts[segment])}
        if samples:
            row["mean_ns"] = f"{sum(samples) / len(samples):.2f}"
            for name, quantile in PERCENTILES:
                row[name] = str(nearest_rank(samples, quantile))
            row["max_ns"] = str(samples[-1])
        else:
            for name in HEADER[2:]:
                row[name] = "-"
        rows.append(row)
    return rows


def format_table(rows):
    lines = ["\t".join(HEADER)]
    lines.extend("\t".join(row[column] for column in HEADER) for row in rows)
    return lines


def parse_expected(path):
    table = {}
    with open(path, encoding="utf-8") as stream:
        header = stream.readline().rstrip("\n").split("\t")
        if header != HEADER:
            raise SystemExit(f"expected header mismatch in {path}: {header}")
        for line in stream:
            fields = line.rstrip("\n").split("\t")
            table[fields[0]] = fields[1:]
    return table


def check(rows, expected):
    failures = []
    for row in rows:
        want = expected.get(row["segment"])
        if want is None:
            failures.append(f"unexpected segment {row['segment']}")
            continue
        got = [row[column] for column in HEADER[1:]]
        if len(want) != len(got):
            failures.append(f"column count mismatch for {row['segment']}")
            continue
        for name, left, right in zip(HEADER[1:], got, want):
            try:
                if abs(float(left) - float(right)) > 0.01:
                    failures.append(
                            f"{row['segment']}.{name}: got {left}, want {right}")
            except ValueError:
                if left != right:
                    failures.append(
                            f"{row['segment']}.{name}: got {left}, want {right}")
    for segment in sorted(set(expected) - {row["segment"] for row in rows}):
        failures.append(f"missing segment {segment}")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ttt_log", help="TTT producer log produced by the instrumentation")
    parser.add_argument("--check", metavar="EXPECTED_TSV",
                                            help="compare the table against an expected TSV")
    args = parser.parse_args()

    counts, latencies = parse_log(args.ttt_log)
    rows = build_rows(counts, latencies)
    table = format_table(rows)
    print("\n".join(table))

    if args.check is None:
        return 0

    failures = check(rows, parse_expected(args.check))
    if failures:
        for failure in failures:
            print(f"mismatch: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
