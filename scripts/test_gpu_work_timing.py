#!/usr/bin/env python3
"""Headless production GPU timestamps: availability, ordering, scope lifetime and bounded output."""
import argparse
import csv
import importlib.util
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', required=True, type=Path)
parser.add_argument('--app', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--driver', type=Path)
args = parser.parse_args()
SRC = Path(__file__).resolve().parents[1]
OUT, BUILD, APP = args.output.resolve(), args.build.resolve(), args.app.resolve()
assert APP.name == 'shadPS4-lbp3.app'
OUT.mkdir(parents=True, exist_ok=True)
assert not (OUT / 'capture').exists(), 'Use a fresh test output directory'
DRIVER = args.driver.resolve() if args.driver else APP / 'Contents/MacOS/libvulkan_kosmickrisp.dylib'
icd = json.loads((APP / 'Contents/MacOS/kosmickrisp_mesa_icd.json').read_text())
icd['ICD']['library_path'] = str(DRIVER)
(OUT / 'precise-icd.json').write_text(json.dumps(icd) + '\n')
ninja = (BUILD / 'build.ninja').read_text()
start = ninja.index('build CMakeFiles/shadps4.dir/src/video_core/renderer_vulkan/vk_scheduler.cpp.o:')
block = ninja[start:].split('\nbuild ', 1)[0]
flags = {k: shlex.split(v) for k, v in re.findall(r'^  (DEFINES|FLAGS|INCLUDES) = (.*)$', block, re.M)}
compiler = re.search(r'^CMAKE_CXX_COMPILER:[^=]+=(.*)$', (BUILD / 'CMakeCache.txt').read_text(), re.M).group(1)
objects = [BUILD / 'CMakeFiles/shadps4.dir/src' / name for name in (
    'video_core/renderer_vulkan/vk_scheduler.cpp.o',
    'video_core/renderer_vulkan/vk_gpu_timing.cpp.o',
    'video_core/renderer_vulkan/vk_resource_pool.cpp.o',
    'video_core/renderer_vulkan/vk_master_semaphore.cpp.o',
    'video_core/renderer_vulkan/vk_common.cpp.o',
    'video_core/buffer_cache/buffer.cpp.o', 'core/gpu_wait_telemetry.cpp.o')]
subprocess.run(['spirv-as', '--target-env', 'vulkan1.3', str(SRC / 'tests/gpu_scratch_copy.spvasm'), '-o', str(OUT / 'scratch.spv')], check=True)
subprocess.run(['spirv-val', '--target-env', 'vulkan1.3', str(OUT / 'scratch.spv')], check=True)
data = (OUT / 'scratch.spv').read_bytes()
words = struct.unpack(f'<{len(data) // 4}I', data)
(OUT / 'scratch_shader.inc').write_text('constexpr u32 ScratchCopyShader[]{' + ','.join(hex(word) for word in words) + '};\n')
binary = OUT / 'gpu-work-timing-test'
command = [compiler, *flags['DEFINES'], *flags['FLAGS'], *flags['INCLUDES'],
           '-I', str(SRC / 'tests'), '-I', str(OUT), str(SRC / 'tests/gpu_work_timing.cpp'), *map(str, objects),
           str(BUILD / 'externals/spdlog/libspdlog.a'), str(BUILD / '_deps/fmt-build/libfmt.a'),
           '-Wl,-dead_strip', '-o', str(binary)]
with (OUT / 'timing-compile.log').open('w') as log:
    subprocess.run(command, cwd=BUILD, check=True, stdout=log, stderr=subprocess.STDOUT)
env = {k: os.environ[k] for k in ('HOME', 'PATH', 'TMPDIR', 'USER') if k in os.environ}
env['VK_DRIVER_FILES'] = str(OUT / 'precise-icd.json')
env['SHADPS4_GPU_TIMING_DIR'] = str(OUT / 'capture/gpu-timing')
env['MESA_KK_PRECISE_COMPUTE_TIMESTAMPS'] = '1'
(OUT / 'capture/gpu-timing').mkdir(parents=True, exist_ok=True)
(OUT / 'capture/gpu-timing/enable').touch()
env['SHAD_TEST_VULKAN_LOADER'] = str(APP / 'Contents/MacOS/libvulkan.dylib')
result = subprocess.run([str(binary)], env=env, text=True, capture_output=True, timeout=30)
output = result.stdout + result.stderr
(OUT / 'timing-result.log').write_text(output)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
print(output, end='')
assert result.returncode == 0, result.returncode
spec = importlib.util.spec_from_file_location('recorder', SRC / 'scripts/record-lbp3-performance.py')
recorder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recorder)
summary = recorder.summarize_gpu_timing(OUT / 'capture')
(OUT / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
files = {item['status']['scheduler']: item for item in summary['files']}
assert set(files) == {'test', 'integrated', 'compute', 'limited', 'idle', 'render'}, files
for name, entry in files.items():
    status = entry['status']
    assert status['failed_queries'] == '0' and status['io_ok'] == '1', entry
    assert status['incomplete_banks_at_close'] == '0' and entry['invalid_rows'] == 0, entry
    assert int(status['written_bytes']) <= int(status['file_limit_bytes']), entry
assert files['test']['status']['skipped_banks'] == '1'
assert files['test']['status']['truncated_scopes'] == '1'
assert files['test']['status']['scope_overflow'] == '3'
assert files['test']['status']['sampled_submissions'] == '14'
assert files['integrated']['status']['sampled_submissions'] == '3'
assert files['idle']['status']['sampled_submissions'] == '0'
assert int(files['limited']['status']['dropped_limit']) >= 1
rows = {}
for role in ('test', 'compute'):
    path = OUT / 'capture/gpu-timing' / files[role]['name']
    rows[role] = list(csv.DictReader(path.open()))
first = [r for r in rows['test'] if r['tick'] == '1']
assert len(first) == 3 and all(r['status'] == 'ok' for r in first), first
assert float(first[2]['gpu_ms']) > 4 * float(first[1]['gpu_ms']) > 0, first
compute = rows['compute']
assert len(compute) == 3 and all(r['status'] == 'ok' for r in compute), compute
assert float(compute[2]['gpu_ms']) > 4 * float(compute[1]['gpu_ms']) > 0, compute
assert all(r['valid_intervals'] for r in summary['submissions'] if r['scheduler'] in ('compute', 'integrated'))
render = [r for r in summary['render_pass_samples'] if r['scheduler'] == 'render']
assert len(render) == 8 and {r['render_pass'] for r in render} == set(range(1, 9)), render
for row in render:
    assert row['gpu_ms'] > 0 and row['draw_calls'] == 2 and row['indirect_calls'] == 1, row
    assert row['shader_changes'] == 1 and row['first_ps'] == 200 and row['last_ps'] == 201, row
    assert row['vertices'] == 6 and row['width'] == row['height'] == 2048, row
integrated_render = [r for r in summary['render_pass_samples'] if r['scheduler'] == 'integrated']
assert len(integrated_render) == 1 and integrated_render[0]['first_ps'] == 456, integrated_render
for row in summary['submissions']:
    if row['scheduler'] == 'render':
        assert row['valid_intervals'] and row['measured_conversion_ms'] == 0, row
        assert row['measured_render_pass_ms'] > 0, row
paths = [*objects, SRC / 'tests/gpu_work_timing.cpp', SRC / 'tests/scheduler_submission.cpp',
    SRC / 'tests/gpu_scratch_copy.spvasm', OUT / 'scratch_shader.inc',
    SRC / 'src/video_core/renderer_vulkan/vk_gpu_timing.cpp',
    SRC / 'src/video_core/renderer_vulkan/vk_gpu_timing.h',
    SRC / 'src/video_core/renderer_vulkan/vk_scheduler.cpp',
    SRC / 'src/video_core/renderer_vulkan/vk_scheduler.h',
    SRC / 'scripts/record-lbp3-performance.py', Path(__file__), DRIVER]
(OUT / 'result.json').write_text(json.dumps({'passed': True, 'returncode': result.returncode,
    'output': output, 'command': command, 'driver': str(DRIVER),
    'sha256': {str(path): digest(path) for path in paths},
    'mixed_transfer_ms': [float(r['gpu_ms']) for r in first],
    'compute_ms': [float(r['gpu_ms']) for r in compute],
    'scope': 'Production Scheduler/GpuTiming/VMA with real ICD; headless test Instance and shaders. No game.'}, indent=2) + '\n')
print('PASS: timestamp values track transfer and compute work; invalid scopes excluded; bounded output and inactive stage verified')
