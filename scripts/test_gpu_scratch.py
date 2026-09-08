#!/usr/bin/env python3
"""Real-ICD scratch reuse regression plus the unchanged StreamBuffer as a blocking control."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--app', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--driver', type=Path)
args = parser.parse_args()
src = Path(__file__).resolve().parents[1]
build, app, out = args.build.resolve(), args.app.resolve(), args.output.resolve()
assert app.name == 'shadPS4-lbp3.app'
out.mkdir(parents=True, exist_ok=True)
selected_driver = args.driver.resolve() if args.driver else app / 'Contents/MacOS/libvulkan_kosmickrisp.dylib'
icd = json.loads((app / 'Contents/MacOS/kosmickrisp_mesa_icd.json').read_text())
icd['ICD']['library_path'] = str(selected_driver)
(out / 'test-icd.json').write_text(json.dumps(icd) + '\n')
shader = src / 'tests/gpu_scratch_copy.spvasm'
spirv = out / 'scratch.spv'
subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(shader), '-o', str(spirv)], check=True)
subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(spirv)], check=True)
words = struct.unpack(f'<{spirv.stat().st_size // 4}I', spirv.read_bytes())
(out / 'scratch_shader.inc').write_text('constexpr u32 ScratchCopyShader[]{\n' +
    ','.join(hex(word) for word in words) + '\n};\n')
obj_root = build / 'CMakeFiles/shadps4.dir/src'
objects = [obj_root / name for name in (
    'video_core/renderer_vulkan/vk_scheduler.cpp.o',
    'video_core/renderer_vulkan/vk_gpu_timing.cpp.o',
    'video_core/renderer_vulkan/vk_resource_pool.cpp.o',
    'video_core/renderer_vulkan/vk_master_semaphore.cpp.o',
    'video_core/renderer_vulkan/vk_common.cpp.o',
    'video_core/buffer_cache/buffer.cpp.o', 'core/gpu_wait_telemetry.cpp.o')]
ninja = (build / 'build.ninja').read_text()
start = ninja.index('build CMakeFiles/shadps4.dir/src/video_core/renderer_vulkan/vk_scheduler.cpp.o:')
block = ninja[start:].split('\nbuild ', 1)[0]
flags = {k: shlex.split(v) for k, v in re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M)}
compiler = re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$', (build / 'CMakeCache.txt').read_text(), re.M).group(1)
env = {key: os.environ[key] for key in ('HOME', 'PATH', 'TMPDIR', 'USER') if key in os.environ}
env['VK_DRIVER_FILES'] = str(out / 'test-icd.json')
env['SHAD_TEST_VULKAN_LOADER'] = str(app / 'Contents/MacOS/libvulkan.dylib')
results = {}
for name, defines, expected in [('current', [], 0), ('old_control', ['-DSHAD_SCRATCH_OLD_CONTROL'], 3)]:
    binary = out / f'scratch-{name}'
    command = [compiler, *flags['DEFINES'], *flags['FLAGS'], *flags['INCLUDES'], *defines,
               '-I', str(out), str(src / 'tests/gpu_scratch.cpp'), *map(str, objects),
               str(build / 'externals/spdlog/libspdlog.a'), str(build / '_deps/fmt-build/libfmt.a'),
               '-Wl,-dead_strip', '-o', str(binary)]
    with (out / f'{name}-compile.log').open('w') as log:
        subprocess.run(command, cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
    result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=30)
    output = result.stdout + result.stderr
    (out / f'{name}-result.log').write_text(output)
    print(name, output, sep='\n', end='')
    assert result.returncode == expected, result.returncode
    assert 'verified GPU words: 99200; capacity: 4096 bytes' in output, output
    if name == 'current':
        assert 'writers before gate: 6/6; waits: 0; queued submissions: 2' in output
        assert 'PASS: GPU scratch reuse' in output
    else:
        assert 'FAIL: CPU blocked on GPU scratch reuse' in output
        assert 'Scratch recorded before GPU gate: NO; writers before gate: 0/6' in output
    results[name] = {'command': command, 'returncode': result.returncode, 'output': output}
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
paths = [*objects, shader, out / 'scratch_shader.inc', src / 'tests/gpu_scratch.cpp',
         src / 'tests/scheduler_submission.cpp', Path(__file__),
         src / 'src/video_core/buffer_cache/buffer.cpp', src / 'src/video_core/buffer_cache/buffer.h',
         selected_driver]
(out / 'result.json').write_text(json.dumps({'passed': True, 'cases': results,
    'sha256': {str(path): digest(path) for path in paths},
    'scope': 'Production GPU scratch / StreamBuffer / scheduler / VMA, real installed KosmicKrisp; '
             'small synthetic compute shader and transfers, test texture mutex and writer callbacks. '
             'No full TextureCache, window, or game.'}, indent=2) + '\n')
