#!/usr/bin/env python3
"""Link existing production scheduler objects to a bounded, headless real-KK regression."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, required=True)
parser.add_argument('--app', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
parser.add_argument('--driver', type=Path)
args = parser.parse_args()
src = Path(__file__).resolve().parents[1]
build = args.build.resolve()
app = args.app.resolve()
assert app.name == 'shadPS4-lbp3.app'
output = args.output.resolve()
output.mkdir(parents=True, exist_ok=True)
macos = app / 'Contents/MacOS'
selected_driver = args.driver.resolve() if args.driver else macos / 'libvulkan_kosmickrisp.dylib'
icd = json.loads((macos / 'kosmickrisp_mesa_icd.json').read_text())
icd['ICD']['library_path'] = str(selected_driver)
(output / 'test-icd.json').write_text(json.dumps(icd) + '\n')
ninja = (build / 'build.ninja').read_text()
object_dir = Path('CMakeFiles/shadps4.dir/src/video_core/renderer_vulkan')
object_names = ['vk_scheduler.cpp.o', 'vk_resource_pool.cpp.o', 'vk_master_semaphore.cpp.o',
                'vk_common.cpp.o', 'vk_gpu_timing.cpp.o']
objects = [build / object_dir / name for name in object_names]
objects += [build / 'CMakeFiles/shadps4.dir/src' / name for name in (
    'core/gpu_wait_telemetry.cpp.o', 'video_core/buffer_cache/buffer.cpp.o')]
start = ninja.index(f'build {object_dir}/vk_scheduler.cpp.o:')
block = ninja[start:].split('\nbuild ', 1)[0]
flags = {k: shlex.split(v) for k, v in re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M)}
compiler = re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$', (build / 'CMakeCache.txt').read_text(), re.M).group(1)
binary = output / 'scheduler-submission-regression'
command = [compiler, *flags['DEFINES'], *flags['FLAGS'], *flags['INCLUDES'],
           str(src / 'tests/scheduler_submission.cpp'), *map(str, objects),
           str(build / 'externals/spdlog/libspdlog.a'),
           str(build / '_deps/fmt-build/libfmt.a'), '-Wl,-dead_strip', '-o', str(binary)]
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
provenance = {'command': command, 'production_objects_sha256': {str(p): digest(p) for p in objects},
              'scheduler_source_sha256': digest(src / 'src/video_core/renderer_vulkan/vk_scheduler.cpp'),
              'test_sources_sha256': {str(src / p): digest(src / p) for p in (
                  'tests/scheduler_submission.cpp', 'src/video_core/renderer_vulkan/vk_presenter.h',
                  'src/video_core/renderer_vulkan/vk_scheduler.h',
                  'src/video_core/renderer_vulkan/vk_master_semaphore.h',
                  'src/video_core/renderer_vulkan/vk_master_semaphore.cpp',
                  'src/video_core/buffer_cache/buffer.h', 'src/video_core/buffer_cache/buffer.cpp',
                  'src/core/gpu_wait_telemetry.h', 'src/core/gpu_wait_telemetry.cpp',
                  'src/core/gpu_wait_log.h', 'src/core/performance_telemetry.h')},
              'driver_sha256': digest(selected_driver),
              'method': 'Production objects; real ICD; test-only headless instance, UI/log/telemetry support. No game.'}
(output / 'command.json').write_text(json.dumps(provenance, indent=2) + '\n')
with (output / 'compile.log').open('w') as log:
    subprocess.run(command, cwd=build, check=True, stdout=log, stderr=subprocess.STDOUT)
env = {k: os.environ[k] for k in ('HOME', 'PATH', 'TMPDIR', 'USER') if k in os.environ}
env['VK_DRIVER_FILES'] = str(output / 'test-icd.json')
env['SHAD_TEST_VULKAN_LOADER'] = str(macos / 'libvulkan.dylib')
result = subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=30)
(output / 'result.log').write_text(result.stdout + result.stderr)
print(result.stdout + result.stderr, end='')
raise SystemExit(result.returncode)
