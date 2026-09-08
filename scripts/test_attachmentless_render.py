#!/usr/bin/env python3
"""Verify attachmentless render area optimization with real vertex storage writes."""
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
args = parser.parse_args()
src = Path(__file__).resolve().parents[1]
build, app, out = args.build.resolve(), args.app.resolve(), args.output.resolve()
assert app.name == 'shadPS4-lbp3.app'
out.mkdir(parents=True, exist_ok=True)
driver = app / 'Contents/MacOS/libvulkan_kosmickrisp.dylib'
icd = json.loads((app / 'Contents/MacOS/kosmickrisp_mesa_icd.json').read_text())
icd['ICD']['library_path'] = str(driver)
(out / 'precise-icd.json').write_text(json.dumps(icd) + '\n')
ninja = (build / 'build.ninja').read_text()
start = ninja.index('build CMakeFiles/shadps4.dir/src/video_core/renderer_vulkan/vk_scheduler.cpp.o:')
block = ninja[start:].split('\nbuild ', 1)[0]
flags = {key: shlex.split(value) for key, value in
         re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M)}
compiler = re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$',
                     (build / 'CMakeCache.txt').read_text(), re.M).group(1)
objects = [build / 'CMakeFiles/shadps4.dir/src' / name for name in (
    'video_core/renderer_vulkan/vk_scheduler.cpp.o',
    'video_core/renderer_vulkan/vk_gpu_timing.cpp.o',
    'video_core/renderer_vulkan/vk_resource_pool.cpp.o',
    'video_core/renderer_vulkan/vk_master_semaphore.cpp.o',
    'video_core/renderer_vulkan/vk_common.cpp.o',
    'video_core/buffer_cache/buffer.cpp.o', 'core/gpu_wait_telemetry.cpp.o')]
shader = src / 'tests/attachmentless_side_effect.vert.spvasm'
subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(shader),
                '-o', str(out / 'attachmentless.spv')], check=True)
subprocess.run(['spirv-val', '--target-env', 'vulkan1.3',
                str(out / 'attachmentless.spv')], check=True)
data = (out / 'attachmentless.spv').read_bytes()
words = struct.unpack(f'<{len(data) // 4}I', data)
include = out / 'attachmentless_shader.inc'
include.write_text('constexpr u32 AttachmentlessShader[]{' +
                   ','.join(hex(word) for word in words) + '};\n')
binary = out / 'attachmentless-render-test'
command = [compiler, *flags['DEFINES'], *flags['FLAGS'], *flags['INCLUDES'],
           '-I', str(src / 'tests'), '-I', str(out),
           str(src / 'tests/attachmentless_render.cpp'), *map(str, objects),
           str(build / 'externals/spdlog/libspdlog.a'),
           str(build / '_deps/fmt-build/libfmt.a'), '-Wl,-dead_strip', '-o', str(binary)]
with (out / 'compile.log').open('w') as log:
    subprocess.run(command, cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
env = {key: os.environ[key] for key in ('HOME', 'PATH', 'TMPDIR', 'USER') if key in os.environ}
env['VK_DRIVER_FILES'] = str(out / 'precise-icd.json')
env['SHAD_TEST_VULKAN_LOADER'] = str(app / 'Contents/MacOS/libvulkan.dylib')
result = subprocess.run([str(binary)], env=env, text=True, capture_output=True, timeout=30)
output = result.stdout + result.stderr
(out / 'result.log').write_text(output)
paths = [*objects, src / 'tests/attachmentless_render.cpp',
         src / 'tests/scheduler_submission.cpp', shader, include,
         src / 'src/video_core/renderer_vulkan/vk_scheduler.h', Path(__file__), driver]
(out / 'result.json').write_text(json.dumps({
    'passed': result.returncode == 0, 'returncode': result.returncode,
    'output': output, 'command': command, 'driver': str(driver),
    'sha256': {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in paths},
    'scope': 'Production Scheduler/RenderState/VMA with real ICD; headless instance. No game.'
}, indent=2) + '\n')
print(output, end='')
assert result.returncode == 0, result.returncode
