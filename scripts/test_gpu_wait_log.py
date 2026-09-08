#!/usr/bin/env python3
"""Exercise the Release GPU-wait journal object with concurrent bounded producers. No game/GPU."""
import argparse
import csv
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
src = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
obj = args.build.resolve() / 'CMakeFiles/shadps4.dir/src/core/gpu_wait_telemetry.cpp.o'
binary = out / 'gpu-wait-log-regression'
test = src / 'tests/gpu_wait_log.cpp'
command = ['xcrun', 'clang++', '-arch', 'x86_64', '-mmacosx-version-min=26.0', '-std=c++23',
           '-O2', '-pthread', '-Wall', '-Wextra', '-Werror', '-I', str(src / 'src'),
           str(test), str(obj), '-Wl,-dead_strip', '-o', str(binary)]
with (out / 'compile.log').open('w') as stream:
    subprocess.run(command, check=True, stdout=stream, stderr=subprocess.STDOUT)
result = subprocess.run([str(binary), str(out)], capture_output=True, text=True, timeout=15)
(out / 'result.log').write_text(result.stdout + result.stderr)
result.check_returncode()
records = {}
for name in ('concurrent', 'rotation', 'restart'):
    status = dict(line.split('=', 1) for line in
                  (out / f'{name}_status.txt').read_text().splitlines())
    status = {k: int(v) for k, v in status.items()}
    paths = [p for p in (out / f'{name}.previous.csv', out / f'{name}.csv') if p.exists()]
    rows = []
    for path in paths:
        assert path.stat().st_size <= 8192, path
        with path.open() as stream:
            items = list(csv.DictReader(stream))
        assert all(None not in row and all(v is not None for v in row.values()) for row in items)
        rows.extend(items)
    assert status['io_failed'] == status['dropped_io'] == status['accepting'] == 0, status
    assert status['current_events'] + status['previous_events'] == len(rows), status
    assert status['written_events'] == len(rows) + status['evicted_events'], status
    assert [int(row['event']) for row in rows] == list(range(
        status['evicted_events'] + 1, status['written_events'] + 1))
    assert all(row['resource'] == 'tile_scratch' and row['source'] == 'stream_reuse' and
               'Event(' in row['caller_function'] for row in rows)
    if name == 'concurrent':
        assert status['written_events'] + status['dropped_busy'] + status['dropped_full'] == 32000
    elif name == 'rotation':
        assert status['rotations'] >= 2 and rows[-1]['request_bytes'] == '424242', status
    else:
        assert len(rows) == 1 and rows[0]['request_bytes'] == '123'
    records[name] = {'status': status, 'retained_bytes': sum(p.stat().st_size for p in paths)}
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
provenance = {'command': command, 'passed': True, 'cases': records,
              'sha256': {str(p): digest(p) for p in (obj, test, Path(__file__),
                  src / 'src/core/gpu_wait_telemetry.cpp', src / 'src/core/gpu_wait_telemetry.h',
                  src / 'src/core/gpu_wait_log.h')}, 'game_launched': False}
(out / 'result.json').write_text(json.dumps(provenance, indent=2) + '\n')
print(result.stdout, end='')
print(json.dumps(records, indent=2))
