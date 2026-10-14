#!/usr/bin/env python3
"""Check that client collection scopes exclude other threads and out-of-scope work."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, required=True)
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='callgrind-scope-') as directory:
    root = Path(directory)
    subprocess.run(['valgrind', '--tool=callgrind', '--collect-atstart=no', '--separate-threads=yes',
                    f'--callgrind-out-file={root}/profile.%p', str(args.binary.resolve())], check=True)
    annotated = []
    for path in sorted(root.glob('profile.*')):
        # Separate-thread mode can leave an empty unsuffixed placeholder.
        if 'events: Ir' not in path.read_text():
            continue
        result = subprocess.run(['callgrind_annotate', '--threshold=100', '--auto=no', str(path)],
                                text=True, capture_output=True, check=True)
        annotated.append(result.stdout)
    text = '\n'.join(annotated)
    if 'includedMarker' not in text or 'excludedMarker' in text:
        raise SystemExit('Thread-local collection isolation failed:\n' + text)
    print('PASS: included marker collected; background and out-of-scope markers excluded')
