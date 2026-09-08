#!/usr/bin/env python3
"""Exercise the real x86_64 KosmicKrisp ICD without launching an emulator."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mesa-build', required=True, type=Path)
parser.add_argument('--loader-dir', required=True, type=Path)
parser.add_argument('--icd', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
args = parser.parse_args()
build = args.mesa_build.resolve()
loader = args.loader_dir.resolve()
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
entry = next(e for e in json.loads((build / 'compile_commands.json').read_text())
             if e['file'].endswith('/kk_cmd_pool.c'))
command = shlex.split(entry['command'])
command = command[:command.index('-MD')]
binary = output / 'command-pool-regression'
command += [str(Path(__file__).with_name('command_pool_regression.c').resolve()),
            '-L' + str(loader), '-lvulkan', '-Wl,-rpath,' + str(loader),
            '-o', str(binary)]
subprocess.run(command, cwd=build, check=True)
subprocess.run(['install_name_tool', '-change', '@rpath/libvulkan.1.dylib',
                '@rpath/libvulkan.dylib', str(binary)], check=True)
env = {k: os.environ[k] for k in ('HOME', 'PATH', 'TMPDIR', 'USER') if k in os.environ}
env['VK_DRIVER_FILES'] = str(args.icd.resolve())
result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=90)
(output / 'result.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
raise SystemExit(result.returncode)
