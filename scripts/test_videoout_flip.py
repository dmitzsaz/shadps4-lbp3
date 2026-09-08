#!/usr/bin/env python3
"""Run the exact production Flip body with a blocking host and a guest waiter."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--before', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
src = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
current = src / 'src/core/libraries/videoout/driver.cpp'


def method(path):
    source = path.read_text()
    begin = source.index('void VideoOutDriver::Flip(const Request& req) {')
    end = source.index('void VideoOutDriver::DrawBlankFrame()', begin)
    body = source[begin:end].strip()
    assert body.endswith('}')
    return body + '\n'


results = {}
for name, source in [('current', current), ('before', args.before.resolve())]:
    folder = out / name
    folder.mkdir(exist_ok=True)
    include = folder / 'videoout_flip_under_test.inc'
    include.write_text(method(source))
    binary = folder / 'videoout-flip-test'
    command = ['xcrun', 'clang++', '-arch', 'x86_64', '-std=c++20', '-O2', '-pthread',
               '-Wall', '-Wextra', '-Werror', '-I', str(src / 'src'), '-I', str(folder),
               str(src / 'tests/videoout_flip.cpp'), '-o', str(binary)]
    with (folder / 'compile.log').open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    (folder / 'result.log').write_text(result.stdout + result.stderr)
    results[name] = {'command': command, 'source': str(source),
                     'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
                     'included_method_sha256': hashlib.sha256(include.read_bytes()).hexdigest(),
                     'returncode': result.returncode, 'output': result.stdout + result.stderr}
    print(name, result.stdout + result.stderr, end='')
    if name == 'current':
        result.check_returncode()
    else:
        assert result.returncode == 1
        assert 'guest work waited for host presentation to return' in result.stderr

results['method'] = (
    'Exact production Flip method compiled as x86_64; test-only collaborators hold '
    'host presentation and model guest/equeue/port state. Previous production body '
    'is the negative control. No full VideoOutDriver constructor, window or game.')
results['test_source_sha256'] = {
    p: hashlib.sha256((src / p).read_bytes()).hexdigest()
    for p in ('tests/videoout_flip.cpp', 'scripts/test_videoout_flip.py',
              'src/core/performance_telemetry.h')}
(out / 'result.json').write_text(json.dumps(results, indent=2) + '\n')
