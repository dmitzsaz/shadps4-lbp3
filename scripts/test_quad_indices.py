#!/usr/bin/env python3
"""Compile and exercise the production in-place index expansion on x86_64."""
import argparse
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
src = Path(__file__).resolve().parents[1]
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
binary = out / 'quad-indices-test'
command = ['/usr/bin/clang++', '-std=c++23', '-arch', 'x86_64', '-O2', '-Wall', '-Wextra',
           '-Werror', '-UNDEBUG', '-I', str(src / 'src'), str(src / 'tests/quad_indices.cpp'),
           '-o', str(binary)]
subprocess.run(command, check=True)
result = subprocess.run([str(binary)], capture_output=True, text=True, check=True)
(out / 'result.json').write_text(json.dumps({'passed': True, 'command': command,
                                            'output': result.stdout}, indent=2) + '\n')
print(result.stdout, end='')
