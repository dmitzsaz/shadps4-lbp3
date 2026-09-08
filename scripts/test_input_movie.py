#!/usr/bin/env python3
"""Exercise the production controller movie object without launching the emulator/game."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import importlib.util

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
SRC = Path(__file__).resolve().parents[1]
BUILD, OUT = args.build.resolve(), args.output.resolve()
OUT.mkdir(parents=True, exist_ok=True)
assert not (OUT / 'record').exists(), 'Use a fresh test output directory'
ninja = (BUILD / 'build.ninja').read_text()
start = ninja.index('build CMakeFiles/shadps4.dir/src/video_core/renderer_vulkan/vk_scheduler.cpp.o:')
block = ninja[start:].split('\nbuild ', 1)[0]
flags = {k: shlex.split(v) for k, v in re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M)}
compiler = re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$', (BUILD / 'CMakeCache.txt').read_text(), re.M).group(1)
objects = [BUILD / 'CMakeFiles/shadps4.dir/src/input/input_movie.cpp.o']
binary = OUT / 'input-movie-test'
command = [compiler, *flags['DEFINES'], *flags['FLAGS'], *flags['INCLUDES'],
           str(SRC / 'tests/input_movie.cpp'), *map(str, objects), '-Wl,-dead_strip', '-o', str(binary)]
with (OUT / 'compile.log').open('w') as log:
    subprocess.run(command, cwd=BUILD, check=True, stdout=log, stderr=subprocess.STDOUT)
result = subprocess.run([str(binary), str(OUT)], capture_output=True, text=True, timeout=30)
(OUT / 'result.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
assert result.returncode == 0, result.returncode
spec = importlib.util.spec_from_file_location('recorder', SRC / 'scripts/record-lbp3-performance.py')
recorder = importlib.util.module_from_spec(spec); spec.loader.exec_module(recorder)
movie = recorder.read_input_movie(OUT / 'record/controller.bin')
assert movie['states'] == 7 and movie['end_frame'] == 4, movie
assert movie['states_per_slot'] == [6, 0, 1, 0, 0]
for name in ('truncated', 'corrupt', 'capped/controller.bin', 'overflow/controller.bin'):
    try:
        recorder.read_input_movie(OUT / name)
        raise AssertionError('Python accepted invalid movie: ' + name)
    except RuntimeError:
        pass
paths = [*objects, SRC / 'src/input/input_movie.cpp', SRC / 'src/input/input_movie.h',
         SRC / 'tests/input_movie.cpp', Path(__file__), SRC / 'scripts/record-lbp3-performance.py']
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
(OUT / 'result.json').write_text(json.dumps({'passed': True, 'command': command,
    'output': result.stdout, 'movie': movie,
    'sha256': {str(path): digest(path) for path in paths}}, indent=2) + '\n')
print('PASS: Python and C++ agree on format, timestamps/frames, checksum and invalid recording rejection')
