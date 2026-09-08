#!/usr/bin/env python3
"""One game session: CSV baselines and CPU profile; optional separate GPU trace."""
import argparse
import bisect
import csv
import datetime as dt
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import signal
import subprocess
import struct
import sys
import tempfile
import time
import uuid

SUPPORT = Path.home() / 'Library/Application Support/shadPS4'
INSTRUMENTS = ['GPU', 'Time Profiler']
GIB = 1024 ** 3
MIN_FREE_BYTES = 12 * GIB
STOP_FREE_BYTES = 6 * GIB
HARD_FREE_BYTES = 4 * GIB
TRACE_BYTES_LIMIT = 2 * GIB
FINALIZATION_SECONDS = 60
INPUT_FILE_LIMIT = 64 * 1024 ** 2
HOME_SNAPSHOT_LIMIT = 256 * 1024 ** 2
MOVIE_HEADER = struct.Struct('<8s4I5Q')
MOVIE_EVENT = struct.Struct('<IIQQ88s')


def require_runtime_path(path):
    path = path.expanduser().resolve()
    for folder in ('Desktop', 'Documents', 'Downloads'):
        if path.is_relative_to((Path.home() / folder).resolve()):
            raise RuntimeError(f'Путь игры или записи находится в защищённой папке {folder}: {path}. '
                               'Установи приложение в /Applications и храни записи в Library/Application Support/shadPS4.')
    return path


def bundled_game_paths(app):
    app = require_runtime_path(app)
    eboot = (app / 'Contents/Resources/Game/CUSA00063/eboot.bin').resolve()
    addons = (app / 'Contents/Resources/Addons').resolve()
    if not eboot.is_relative_to(app) or not eboot.is_file():
        raise RuntimeError(f'Не найден встроенный eboot.bin внутри {app}. Внешняя копия игры автоматически не выбирается.')
    if not addons.is_relative_to(app) or not (addons / 'CUSA00063').is_dir():
        raise RuntimeError(f'Не найдены встроенные DLC внутри {app}.')
    return eboot, addons


def movie_hash(data, value=14695981039346656037):
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return value


def read_input_movie(path):
    size = path.stat().st_size
    if not MOVIE_HEADER.size + MOVIE_EVENT.size <= size <= INPUT_FILE_LIMIT or (size - MOVIE_HEADER.size) % MOVIE_EVENT.size:
        raise RuntimeError('Запись контроллера неполная или превышает лимит.')
    data = path.read_bytes()
    magic, version, header_size, event_size, slots, unix_ns, steady_ns, *unused = MOVIE_HEADER.unpack_from(data)
    if (magic, version, header_size, event_size, slots) != (b'SHADPAD1', 1, 64, 112, 5):
        raise RuntimeError('Неподдерживаемый формат записи контроллера.')
    frame = elapsed = states = 0
    flips = [(unix_ns, 0)]
    per_slot = [0] * 5
    for offset in range(header_size, size, event_size):
        kind, slot, current, time_us, pad = MOVIE_EVENT.unpack_from(data, offset)
        if current < frame or time_us < elapsed:
            raise RuntimeError('Нарушен порядок записи контроллера.')
        if kind == 3:
            expected_hash = struct.unpack_from('<Q', pad, 80)[0]
            if offset + event_size != size or current != frame or expected_hash != movie_hash(memoryview(data)[:offset]):
                raise RuntimeError('Нарушена контрольная сумма или окончание записи контроллера.')
            if not states or not frame:
                raise RuntimeError('Запись контроллера не содержит игрового прогона.')
            return {'version': 1, 'sha256': hashlib.sha256(data).hexdigest(), 'bytes': size,
                    'states': states, 'states_per_slot': per_slot, 'end_frame': frame,
                    'duration_us': time_us, 'start_unix_ns': unix_ns, 'start_steady_ns': steady_ns,
                    'flips': flips}
        if kind == 2 and current == frame + 1:
            flips.append((unix_ns + time_us * 1000, current))
        elif kind == 1 and slot < 5 and current == frame and pad[10] <= 1 and pad[56] <= 2:
            states += 1
            per_slot[slot] += 1
        else:
            raise RuntimeError('Недопустимое событие контроллера.')
        frame, elapsed = current, time_us
    raise RuntimeError('Запись контроллера не закрыта: отсутствует footer.')


def input_plan(run):
    details = read_input_movie(run / 'inputs/controller.bin')
    status = json.loads((run / 'inputs/status.json').read_text())
    if status.get('valid') is not True or status.get('complete') is not True:
        raise RuntimeError('Запись контроллера потеряла события или ещё не завершена.')
    marks = [json.loads(line) for line in (run / 'markers.jsonl').read_text().splitlines()]
    flips = details.pop('flips')
    times = [time for time, frame in flips]
    frame_marks = {m['event']: flips[max(0, bisect.bisect_right(times, m['unix_ns']) - 1)][1] for m in marks}
    if not 0 < frame_marks.get('baseline_start', 0) < details['end_frame']:
        raise RuntimeError('Нет пригодной отметки места для автоматического замера.')
    return {**details, 'marker_frames': frame_marks, 'benchmark_frame': frame_marks['baseline_start'],
            'clock': 'guest_flip', 'note': 'Input replay is not an emulator save state; verify the reference screenshots.'}


def home_directory():
    config = json.loads((SUPPORT / 'config.json').read_text())
    return Path(config.get('General', {}).get('home_dir') or SUPPORT / 'home').expanduser().resolve()


def tree_manifest(directory):
    if not directory.is_dir() or directory.is_symlink():
        raise RuntimeError(f'Нет отдельной папки сохранений: {directory}')
    files = {}
    total = 0
    def walk_error(error):
        raise error
    for root, dirs, names in os.walk(directory, followlinks=False, onerror=walk_error):
        for name in [*dirs, *names]:
            path = Path(root) / name
            if path.is_symlink():
                raise RuntimeError(f'Снимок сохранений содержит ссылку: {path}; изоляция повтора не гарантируется.')
        for name in names:
            path = Path(root) / name
            if not path.is_file():
                raise RuntimeError(f'В сохранениях обнаружен специальный файл: {path}')
            total += path.stat().st_size
            if total > HOME_SNAPSHOT_LIMIT:
                raise RuntimeError('Снимок сохранений превышает 256 МиБ; автоматическое копирование остановлено.')
            files[str(path.relative_to(directory))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {'files': files, 'bytes': total}


def snapshot_home(run):
    source = home_directory()
    if not source.is_dir():
        raise RuntimeError(f'Не найдена папка сохранений: {source}')
    before = tree_manifest(source)
    shutil.copytree(source, run / 'initial-home', copy_function=shutil.copy2)
    if tree_manifest(run / 'initial-home') != before or tree_manifest(source) != before:
        raise RuntimeError('Сохранения изменились во время копирования.')
    write_json(run / 'initial-home.json', {'source': str(source), **before})


def user_layout():
    # Identity/order check without putting account names, tokens or credentials in the manifest.
    path = SUPPORT / 'users.json'
    if not path.exists():
        return None
    users = json.loads(path.read_text()).get('Users', {})
    users.pop('commit_hash', None)
    return hashlib.sha256(json.dumps(users, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def prepare_replay(source, run=None):
    plan = input_plan(source)
    manifest = json.loads((source / 'initial-home.json').read_text())
    if tree_manifest(source / 'initial-home') != {k: manifest[k] for k in ('files', 'bytes')}:
        raise RuntimeError('Исходные сохранения записи изменены.')
    configuration = json.loads((source / 'input-environment.json').read_text())
    if configuration['user_layout_sha256'] != user_layout():
        raise RuntimeError('Состав или настройки профилей изменились с момента записи контроллера.')
    settings = json.loads((source / 'settings.json').read_text())
    current = json.loads((SUPPORT / 'config.json').read_text())
    for key in settings:
        if key.lower() in ('gpu', 'vulkan', 'input') and current.get(key) != settings[key]:
            raise RuntimeError(f'Настройки {key} отличаются от записи; сначала согласуй параметры сравнения.')
    if run is not None:
        shutil.copytree(source / 'initial-home', run / 'replay-home', copy_function=shutil.copy2)
        if tree_manifest(run / 'replay-home') != {k: manifest[k] for k in ('files', 'bytes')}:
            raise RuntimeError('Рабочая копия сохранений не прошла проверку.')
        controller = run / 'replay-controller.bin'
        shutil.copyfile(source / 'inputs/controller.bin', controller)
        if hashlib.sha256(controller.read_bytes()).hexdigest() != plan['sha256']:
            controller.unlink()
            raise RuntimeError('Рабочая копия записи контроллера не прошла проверку.')
        write_json(run / 'replay-source.json', {'capture': str(source), 'plan': plan,
                   'save_snapshot_bytes': manifest['bytes'], 'reference_screenshots': str(source / 'screenshots')})
    return plan, settings


def replay_arguments(settings, app):
    config = settings['launcher']
    eboot, addons = bundled_game_paths(app)
    args = ['-g', str(eboot), '--set-addon-folder', str(addons), '--resolution', config['resolution'],
            '--fullscreen', str(config['fullscreen']).lower()]
    for key, flag in [('patch_prize_bubbles', '--lbp3-patch-bubbles'),
                      ('disable_sprite_lights', '--lbp3-disable-sprite-lights'),
                      ('disable_tone_map', '--lbp3-disable-tone-map')]:
        args.extend([flag, str(config[key]).lower()])
    args.append('--show-fps' if config['show_fps'] else '--hide-fps')
    if config['lbp3_online']:
        args.append('--lbp3-online')
    return args


def require_disk_space(paths):
    for path in paths:
        existing = path
        while not existing.exists():
            existing = existing.parent
        free = shutil.disk_usage(existing).free
        if free < MIN_FREE_BYTES:
            raise RuntimeError(f'Недостаточно места для Metal-записи на {existing}: '
                               f'{free / GIB:.1f} ГиБ свободно, нужно минимум {MIN_FREE_BYTES / GIB:.0f} ГиБ.')


def file_sizes(paths):
    result = {}
    for path in paths:
        try:
            result[str(path)] = path.stat().st_size
        except FileNotFoundError:
            pass
    return result


def trace_usage(run, temp_root, original_temp):
    # xctrace writes raw events outside --output. Count growth there as well;
    # never delete shared temporary files or stop other tracing processes.
    temporary = file_sizes(temp_root.glob('instruments*.ktrace'))
    growth = sum(max(0, size - original_temp.get(name, 0))
                 for name, size in temporary.items())
    output = sum(file_sizes(p for p in run.rglob('*') if p.is_file()).values())
    free = min(shutil.disk_usage(run).free, shutil.disk_usage(temp_root).free)
    return {'free_bytes': free, 'trace_and_session_bytes': growth + output,
            'temporary_files': temporary}


def guard_reason(usage):
    if usage['free_bytes'] < STOP_FREE_BYTES:
        return 'Недостаточно свободного места; запись остановлена заранее.'
    if usage['trace_and_session_bytes'] > TRACE_BYTES_LIMIT:
        return 'Достигнут лимит 2 ГиБ диагностических данных; запись остановлена заранее.'
    return None


def monitor_trace(tracer, run, temp_root, original_temp, duration, mark):
    started = time.monotonic()
    stop_time = None
    reason = None
    last_report = -10
    while tracer.poll() is None:
        now = time.monotonic()
        usage = trace_usage(run, temp_root, original_temp)
        if now - last_report >= 5:
            mark('trace_storage', **usage)
            last_report = now
        if (usage['free_bytes'] < HARD_FREE_BYTES or
                usage['trace_and_session_bytes'] > 2 * TRACE_BYTES_LIMIT):
            reason = 'Аварийная остановка трассировщика для сохранения места на диске.'
            tracer.kill()
            mark('trace_guard_kill', reason=reason, **usage)
            break
        if stop_time is None:
            reason = guard_reason(usage)
            if reason or now - started > duration + 10:
                reason = reason or 'Истёк срок записи; запрошено завершение трассировщика.'
                tracer.send_signal(signal.SIGINT)
                stop_time = now
                mark('trace_stop_requested', reason=reason, **usage)
                print(reason + ' Игра продолжает работать.', flush=True)
        elif now - stop_time > FINALIZATION_SECONDS:
            reason = 'Трассировщик не завершил сохранение за 60 секунд; остановлен.'
            tracer.kill()
            mark('trace_finalization_timeout', reason=reason, **usage)
            break
        time.sleep(0.5)
    return tracer.wait(), reason


def copy_log_tail(source, destination, limit=8 * 1024 ** 2):
    with source.open('rb') as stream:
        stream.seek(0, os.SEEK_END)
        size = stream.tell()
        stream.seek(max(0, size - limit))
        data = stream.read(limit)
    destination.write_bytes(data)
    return {'source': str(source), 'original_bytes': size,
            'copied_bytes': len(data), 'tail_only': size > limit}


def clean_env():
    # Instruments can store target environment variables in the trace. Do not
    # propagate shell tokens or unrelated development-tool credentials.
    names = ['HOME', 'USER', 'LOGNAME', 'PATH', 'TMPDIR', 'LANG', 'LC_ALL',
             'SHELL', '__CF_USER_TEXT_ENCODING']
    return {key: os.environ[key] for key in names if key in os.environ}


def command(args, timeout=30):
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout,
                          env=clean_env())


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n')


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False


def metadata(path):
    return dict(line.split('=', 1) for line in path.read_text().splitlines() if '=' in line)


def summarize_gpu_timing(run):
    directory = run / 'gpu-timing'
    if not directory.is_dir():
        return None
    result = {'note': 'Sampled GPU command-buffer intervals, not whole-frame latency. '
              'Precise timestamps can add overhead. detile_dispatch excludes the later image upload; '
              'tile_download_dispatch includes image download and tiling. Unattributed time includes '
              'rendering, other compute/copies and dependencies. Render passes are sparse rotated '
              'samples; mixed-shader passes cannot be attributed to one shader. Do not add overlapping scopes.',
              'files': [], 'submissions': [], 'work_groups': []}
    groups = {}
    for path in sorted(directory.glob('*.csv')):
        status = path.with_name(path.stem + '_status.txt')
        file_info = {'name': path.name, 'status': metadata(status) if status.exists() else {},
                     'invalid_rows': 0}
        result['files'].append(file_info)
        batches = {}
        with path.open() as stream:
            for row in csv.DictReader(stream):
                try:
                    if None in row or any(value is None for value in row.values()):
                        raise ValueError('partial row')
                    for key in ('gpu_ms', 'offset_ms'):
                        row[key] = float(row[key])
                        if not math.isfinite(row[key]) or row[key] < 0:
                            raise ValueError('invalid duration')
                    for key in ('tick', 'index', 'scope_count', 'scope_overflow', 'render_passes',
                                'address', 'bytes', 'width', 'height', 'depth', 'pitch', 'bits',
                                'tile_mode', 'mips', 'layers'):
                        row[key] = int(row[key])
                    if row['kind'] == 'render_pass':
                        for key in ('render_pass', 'draw_calls', 'indirect_calls', 'shader_changes',
                                    'vertices', 'first_vs', 'first_ps', 'last_vs', 'last_ps'):
                            row[key] = int(row[key])
                    batches.setdefault(row['tick'], []).append(row)
                except (TypeError, KeyError, ValueError):
                    file_info['invalid_rows'] += 1
        for tick, rows in batches.items():
            full = [r for r in rows if r['kind'] == 'submission']
            if len(full) != 1:
                file_info['invalid_rows'] += len(rows)
                continue
            full = full[0]
            scopes = sorted((r for r in rows if r['kind'] != 'submission'), key=lambda r: r['offset_ms'])
            disjoint = all(a['offset_ms'] + a['gpu_ms'] <= b['offset_ms'] + 0.000002
                           for a, b in zip(scopes, scopes[1:]))
            valid = (full['status'] == 'ok' and len(scopes) == full['scope_count'] and disjoint
                     and all(r['status'] == 'ok' and r['offset_ms'] + r['gpu_ms'] <= full['gpu_ms'] + 0.000002
                             for r in scopes))
            total = sum(r['gpu_ms'] for r in scopes if r['kind'] != 'render_pass')
            render_total = sum(r['gpu_ms'] for r in scopes if r['kind'] == 'render_pass')
            result['submissions'].append({
                'file': path.name, 'scheduler': full['scheduler'], 'tick': tick,
                'record_begin_steady_ns': full['record_begin_steady_ns'],
                'submit_steady_ns': full['submit_steady_ns'], 'valid_intervals': valid,
                'gpu_ms': full['gpu_ms'], 'render_passes': full['render_passes'],
                'scope_count': full['scope_count'], 'scope_overflow': full['scope_overflow'],
                'measured_conversion_ms': total if valid else None,
                'measured_render_pass_ms': render_total if valid else None,
                'unattributed_gpu_ms': max(0, full['gpu_ms'] - total - render_total) if valid else None})
            if not valid:
                continue
            for row in scopes:
                if row['kind'] == 'render_pass':
                    result.setdefault('render_pass_samples', []).append({
                        **{k: row[k] for k in ('scheduler', 'tick', 'gpu_ms', 'offset_ms',
                                                'width', 'height', 'layers')},
                        **{k: int(row[k]) for k in ('render_pass', 'draw_calls', 'indirect_calls',
                                                    'shader_changes', 'vertices', 'first_vs',
                                                    'first_ps', 'last_vs', 'last_ps')}})
                    continue
                key = tuple(row[k] for k in ('kind', 'address', 'bytes', 'width', 'height', 'depth',
                                              'pitch', 'bits', 'tile_mode', 'mips', 'layers'))
                groups.setdefault(key, []).append(row['gpu_ms'])
    names = ('kind', 'address', 'bytes', 'width', 'height', 'depth', 'pitch', 'bits', 'tile_mode', 'mips', 'layers')
    for key, durations in groups.items():
        result['work_groups'].append({**dict(zip(names, key)), 'samples': len(durations),
            'gpu_ms_mean': sum(durations) / len(durations), 'gpu_ms_max': max(durations),
            'sampled_total_gpu_ms': sum(durations)})
    result['work_groups'].sort(key=lambda row: row['sampled_total_gpu_ms'], reverse=True)
    return result


def preflight(app):
    if sys.platform != 'darwin' or app.name != 'shadPS4-lbp3.app':
        raise RuntimeError('Нужна macOS и bundle с точным именем shadPS4-lbp3.app.')
    bundled_game_paths(app)
    for name in ['shadps4', 'shadps4-core', 'libvulkan_kosmickrisp.dylib']:
        if not (app / 'Contents/MacOS' / name).is_file():
            raise RuntimeError(f'В приложении отсутствует {name}')
    info = json.loads((app / 'Contents/Resources/BuildInfo.json').read_text())
    if info.get('performance_capture_version') != 1:
        raise RuntimeError('Выбрана старая сборка без поддержки изолированной записи.')
    for kind, required in [('templates', ['Time Profiler']), ('instruments', INSTRUMENTS)]:
        result = command(['xcrun', 'xctrace', 'list', kind])
        if result.returncode or any(item not in result.stdout for item in required):
            raise RuntimeError(f'Не доступны компоненты Xcode: {required}\n{result.stderr}')
    processes = command(['ps', '-ww', '-axo', 'comm=']).stdout.splitlines()
    if any('/lbp3-runtime/' in p or p.endswith('/shadps4-core') or
           ('.app/Contents/MacOS/' in p and p.endswith('/shadps4')) for p in processes):
        raise RuntimeError('Сначала закрой уже запущенную игру и её лаунчер. Запись откроет обычный лаунчер.')


def trace_command(pid, output, seconds, kind='gpu'):
    # A blank document with GPU avoids the full Metal Application/driver tables
    # and their per-API stack capture. Some common Metal metadata still use stacks.
    if kind not in ('gpu', 'cpu'):
        raise ValueError(f'Unknown recording phase: {kind}')
    args = ['xcrun', 'xctrace', 'record']
    args += ['--instrument', 'GPU'] if kind == 'gpu' else ['--template', 'Time Profiler']
    return args + ['--attach', str(pid), '--time-limit', f'{seconds}s',
                   '--output', str(output), '--no-prompt']


def summarize_gpu_waits(run, marks):
    """Clip retained wait intervals to capture phases; do not assign a whole wait to a frame."""
    sessions = []
    windows = {
        'retained_session': (-math.inf, math.inf),
        'baseline': (marks.get('baseline_start', math.inf),
                     marks.get('baseline_end', marks.get('trace_requested', -math.inf))),
        'cpu_window_approx': (marks.get('cpu_requested', math.inf),
                              marks.get('cpu_finished', -math.inf)),
        'post_baseline': (marks.get('post_baseline_start', math.inf),
                          marks.get('post_baseline_end', -math.inf)),
    }
    buckets = {name: {} for name in windows}
    context_names = {1: 'texture_cache_lock', 2: 'image_refresh', 4: 'buffer_upload',
                     8: 'upload_tracker_locks', 16: 'image_download'}
    for meta in (run / 'telemetry').glob('*_meta.txt'):
        values = metadata(meta)
        if values.get('gpu_wait_attribution_version') != '1':
            continue
        if 'gpu_waits' not in values or 'start_unix_ns' not in values:
            continue
        path = run / 'telemetry' / Path(values['gpu_waits']).name
        paths = [path.with_name(path.stem + '.previous.csv'), path]
        status_path = path.with_name(path.stem + '_status.txt')
        session = {'metadata': meta.name, 'files': [], 'retained_events': 0,
                   'invalid_rows': 0, 'status': metadata(status_path) if status_path.exists() else {},
                   'log_open': values.get('gpu_wait_log_open')}
        origin = int(values['start_unix_ns'])
        # Subtract integer epoch anchors first to preserve submillisecond precision.
        relative = {name: ((begin - origin) / 1e6, (end - origin) / 1e6)
                    for name, (begin, end) in windows.items()}
        seen = set()
        for item in paths:
            if not item.exists():
                continue
            session['files'].append(item.name)
            with item.open() as stream:
                for row in csv.DictReader(stream):
                    try:
                        if None in row or any(value is None for value in row.values()):
                            raise ValueError('Incomplete row')
                        number = int(row['event'])
                        begin, end, duration = map(float, (row['start_ms'], row['end_ms'], row['wait_ms']))
                        context = int(row['context_bits'])
                        if not all(math.isfinite(x) for x in (begin, end, duration)) or \
                                begin < 0 or end < begin or abs(end - begin - duration) > 0.00001:
                            raise ValueError('Invalid interval')
                        key = (row['source'], row['resource'], context,
                               row['caller_function'], row['caller_file'], row['caller_line'])
                    except (KeyError, ValueError, TypeError):
                        session['invalid_rows'] += 1
                        continue
                    if number in seen:
                        continue  # A live rotation can expose a row in both observed files.
                    seen.add(number)
                    session['retained_events'] += 1
                    for name, (low, high) in relative.items():
                        overlap = min(end, high) - max(begin, low)
                        if overlap <= 0:
                            continue
                        bucket = buckets[name].setdefault(key, {'events': 0, 'overlap_ms': 0,
                                                               'largest_overlap_ms': 0})
                        bucket['events'] += 1
                        bucket['overlap_ms'] += overlap
                        bucket['largest_overlap_ms'] = max(bucket['largest_overlap_ms'], overlap)
        sessions.append(session)
    if not sessions:
        return None
    phases = {}
    for name, values in buckets.items():
        rows = []
        for (source, resource, context, function, file, line), totals in values.items():
            rows.append({'source': source, 'resource': resource, 'context_bits': context,
                         'contexts': [label for bit, label in context_names.items() if context & bit],
                         'caller': {'function': function, 'file': file, 'line': line}, **totals})
        if rows:
            phases[name] = sorted(rows, key=lambda row: row['overlap_ms'], reverse=True)
    return {'version': 1, 'sessions': sessions, 'phases': phases,
            'semantics': 'Retained blocking waits only; durations clipped to phase overlap. '
                         'Wall time summed across waiters, not GPU execution or independent frame cost. '
                         'Check rotation, dropped-event and I/O status for coverage.'}


def summarize(run):
    events = [json.loads(line) for line in (run / 'markers.jsonl').read_text().splitlines()]
    marks = {e['event']: e['unix_ns'] for e in events}
    groups = {'all_session': [], 'baseline': [], 'gpu_timing_window': [], 'trace_window_approx': [],
              'cpu_window_approx': [], 'post_baseline': []}
    for meta in (run / 'telemetry').glob('*_meta.txt'):
        values = metadata(meta)
        if 'start_unix_ns' not in values:
            continue
        start = int(values['start_unix_ns'])
        frames = Path(values['frames'])
        with frames.open() as stream:
            for row in csv.DictReader(stream):
                try:
                    ms = float(row['frame_ms'])
                    timestamp = start + float(row['elapsed_ms']) * 1e6
                except (TypeError, ValueError, KeyError):
                    continue  # A still-open recorder can have an incomplete final line.
                if not math.isfinite(ms) or ms <= 0:
                    continue
                groups['all_session'].append(row)
                if marks.get('baseline_start', math.inf) <= timestamp < marks.get('baseline_end', marks.get('trace_requested', -math.inf)):
                    groups['baseline'].append(row)
                if marks.get('trace_requested', math.inf) <= timestamp <= marks.get('trace_finished', -math.inf):
                    groups['trace_window_approx'].append(row)
                if marks.get('gpu_timing_start', math.inf) <= timestamp <= marks.get('gpu_timing_end', -math.inf):
                    groups['gpu_timing_window'].append(row)
                if marks.get('cpu_requested', math.inf) <= timestamp <= marks.get('cpu_finished', -math.inf):
                    groups['cpu_window_approx'].append(row)
                if marks.get('post_baseline_start', math.inf) <= timestamp <= marks.get('post_baseline_end', -math.inf):
                    groups['post_baseline'].append(row)
    result = {'note': 'Trace bounds here include xctrace startup/finalization. Use GPU/CPU trace TOCs for exact bounds. '
                      'gpu_wait_ms is CPU waiting time, not GPU execution time. Baseline already includes CSV sampling.',
              'phases': {}}
    timings = ['gpu_wait_ms', 'frame_pool_wait_ms', 'present_fence_wait_ms',
               'draw_cpu_ms', 'dispatch_cpu_ms', 'host_shader_compile_ms',
               'guest_shader_compile_ms', 'sampler_overhead_ms',
               'submit_mutex_wait_ms', 'queue_submit_cpu_ms', 'command_pool_wait_ms',
               'command_buffer_acquire_cpu_ms', 'submit_pending_ops_cpu_ms',
               'texture_fault_lock_wait_ms', 'buffer_fault_lock_wait_ms']
    for name, rows in groups.items():
        if not rows:
            continue
        samples = sorted(float(row['frame_ms']) for row in rows)
        result['phases'][name] = {
            'frames': len(samples), 'fps_from_total_frame_time': 1000 * len(samples) / sum(samples),
            'frame_ms': {f'p{p}': samples[max(0, math.ceil(len(samples) * p / 100) - 1)] for p in [50, 95, 99]},
            'frames_over_33_333ms': sum(x > 1000 / 30 for x in samples),
            'frames_over_40ms': sum(x > 40 for x in samples),
            'timing_totals_ms': {key: sum(float(row.get(key) or 0) for row in rows)
                                 for key in timings if any(key in row for row in rows)},
        }
    gpu_waits = summarize_gpu_waits(run, marks)
    if gpu_waits is not None:
        result['gpu_wait_attribution'] = gpu_waits
    gpu_timing = summarize_gpu_timing(run)
    if gpu_timing is not None:
        result['gpu_work_timing'] = gpu_timing
    write_json(run / 'summary.json', result)


def snapshot(run, app):
    write_json(run / 'build.json', {
        'bundle': str(app),
        'build_info': json.loads((app / 'Contents/Resources/BuildInfo.json').read_text()),
        'sha256': {name: hashlib.sha256((app / 'Contents/MacOS' / name).read_bytes()).hexdigest()
                   for name in ['shadps4', 'shadps4-core', 'libvulkan_kosmickrisp.dylib']}})
    # Record graphics settings only, rather than copying accounts/keys or network credentials.
    config = json.loads((SUPPORT / 'config.json').read_text())
    settings = {key: value for key, value in config.items()
                if key.lower() in ['gpu', 'vulkan', 'log', 'debug', 'video', 'display', 'input']}
    launcher = json.loads((SUPPORT / 'lbp3-launcher.json').read_text())
    settings['launcher'] = {key: value for key, value in launcher.items()
                           if key in ['resolution', 'fullscreen', 'show_fps', 'patch_prize_bubbles',
                                      'disable_sprite_lights', 'disable_tone_map', 'lbp3_online']}
    write_json(run / 'settings.json', settings)
    with (run / 'system.txt').open('w') as stream:
        for args in [['sw_vers'], ['sysctl', 'hw.model', 'hw.memsize', 'hw.ncpu', 'vm.swapusage'],
                     ['xcodebuild', '-version'], ['pmset', '-g', 'therm']]:
            result = command(args)
            stream.write(f'{args}\n{result.stdout}{result.stderr}\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--app', required=True, type=Path)
    parser.add_argument('--output-root', type=Path, default=SUPPORT / 'performance-captures')
    parser.add_argument('--duration', type=int, default=5, help='GPU recording seconds (5–10)')
    parser.add_argument('--cpu-duration', type=int, default=10, help='Separate CPU profile seconds (5–15)')
    parser.add_argument('--baseline-seconds', type=int, default=15,
                        help='Uninstrumented baseline seconds (15–120); use a stationary replay for longer runs')
    parser.add_argument('--mode', choices=['cpu', 'gpu-cpu'], default='cpu',
                        help='CPU only by default: GPU tracing substantially slows LBP3')
    parser.add_argument('--check', action='store_true', help='Validate installation without starting the game')
    parser.add_argument('--no-gpu-timing', action='store_true', help='Skip sampled GPU timestamps')
    parser.add_argument('--replay', type=Path, help='Automatically replay a completed capture and run the measurements')
    parser.add_argument('--replay-timeout', type=int, default=1800, help='Maximum seconds to reach the recorded benchmark frame')
    args = parser.parse_args()
    app = args.app.expanduser().resolve()
    if not 5 <= args.duration <= 10:
        parser.error('--duration must be between 5 and 10 seconds')
    if not 5 <= args.cpu_duration <= 15:
        parser.error('--cpu-duration must be between 5 and 15 seconds')
    if not 15 <= args.baseline_seconds <= 120:
        parser.error('--baseline-seconds must be between 15 and 120 seconds')
    preflight(app)
    eboot, addons = bundled_game_paths(app)
    output_root = require_runtime_path(args.output_root)
    require_runtime_path(home_directory())
    info = json.loads((app / 'Contents/Resources/BuildInfo.json').read_text())
    if info.get('controller_movie_version') != 1:
        raise RuntimeError('В сборке нет записи/воспроизведения контроллера.')
    source = args.replay.expanduser().resolve() if args.replay else None
    replay_plan = replay_settings = None
    if source:
        replay_plan, replay_settings = prepare_replay(source)
    if not 30 <= args.replay_timeout <= 7200:
        parser.error('--replay-timeout must be between 30 and 7200 seconds')
    if not args.no_gpu_timing:
        info = json.loads((app / 'Contents/Resources/BuildInfo.json').read_text())
        if info.get('gpu_work_timing_version') != 1 or info.get('precise_compute_timestamps_version') != 1:
            raise RuntimeError('В сборке нет выборочных GPU-замеров. Обнови сборку или укажи --no-gpu-timing.')
    temp_root = Path(tempfile.gettempdir())
    require_disk_space([output_root, temp_root])
    if args.check:
        print('OK: приложение, eboot, инструменты записи и минимум 12 ГиБ свободного места доступны.')
        if source:
            print(f'OK: контроллер и сохранения проверены; автоматический замер с кадра {replay_plan["benchmark_frame"]}.')
        return
    stamp = dt.datetime.now().strftime('%Y%m%d-%H%M%S')
    run = output_root / f'{stamp}-{uuid.uuid4().hex[:6]}'
    (run / 'telemetry').mkdir(parents=True)
    (run / 'inputs').mkdir()
    (run / 'screenshots').mkdir()
    tracer = None
    pid = None
    outcome = {'complete': False, 'trace_recorded': False}

    def mark(event, **extra):
        with (run / 'markers.jsonl').open('a') as stream:
            stream.write(json.dumps({'event': event, 'unix_ns': time.time_ns(),
                                     'monotonic_ns': time.monotonic_ns(), **extra}) + '\n')

    def delay(seconds):
        for _ in range(seconds):
            if not alive(pid):
                raise RuntimeError('Игра закрылась до завершения записи.')
            time.sleep(1)

    try:
        write_json(run / 'capture-plan.json', {
            'version': 5, 'mode': args.mode,
            'controller_movie_version': 1, 'controller_recording': source is None,
            'controller_file_limit': INPUT_FILE_LIMIT, 'replay_source': str(source) if source else None,
            'sampled_gpu_seconds': 0 if args.no_gpu_timing else 15,
            'sampled_gpu_period': 31, 'sampled_gpu_file_limit_per_scheduler': 2 * 1024 ** 2,
            'sampled_render_pass_stride': info.get('gpu_render_pass_sample_stride', 0),
            'gpu_instrument': 'GPU' if args.mode == 'gpu-cpu' else None,
            'cpu_template': 'Time Profiler',
            'return_to_game_seconds': 5 if source else 10, 'baseline_seconds': args.baseline_seconds,
            'gpu_seconds': args.duration if args.mode == 'gpu-cpu' else 0,
            'cooldown_seconds': 10 if args.mode == 'gpu-cpu' else 0,
            'cpu_seconds': args.cpu_duration, 'post_baseline_seconds': 10,
            'note': 'GPU-only still captures some common Metal callstacks; overhead is not zero.'})
        snapshot(run, app)
        write_json(run / 'game-paths.json', {'eboot': str(eboot), 'addons': str(addons),
                   'eboot_sha256': hashlib.sha256(eboot.read_bytes()).hexdigest(),
                   'data_root': str(run)})
        if source:
            replay_plan, replay_settings = prepare_replay(source, run)
            # These are the actual CLI overrides used by the automatic replay.
            actual = json.loads((run / 'settings.json').read_text())
            actual['launcher'] = replay_settings['launcher']
            write_json(run / 'settings.json', actual)
        else:
            snapshot_home(run)
            write_json(run / 'input-environment.json', {'user_layout_sha256': user_layout(),
                       'clock': 'guest_flip', 'game': 'CUSA00063'})
        env = clean_env()
        env['SHADPS4_PERF_OUTPUT'] = str(run / 'telemetry')
        env['SHADPS4_INPUT_SCREENSHOTS'] = str(run / 'screenshots')
        if source:
            env['SHADPS4_INPUT_REPLAY'] = str(run / 'replay-controller.bin')
            env['SHADPS4_INPUT_REPORT'] = str(run / 'inputs')
            env['SHADPS4_REPLAY_HOME'] = str(run / 'replay-home')
        else:
            env['SHADPS4_INPUT_RECORD'] = str(run / 'inputs')
        if not args.no_gpu_timing:
            (run / 'gpu-timing').mkdir()
            env['SHADPS4_GPU_TIMING_DIR'] = str(run / 'gpu-timing')
            env['MESA_KK_PRECISE_COMPUTE_TIMESTAMPS'] = '1'
        mark('launcher_start')
        arguments = (['--launcher-launchd'] + replay_arguments(replay_settings, app) + ['--perf-telemetry', '--perf-output', str(run / 'telemetry')]
                     if source else ['--launcher-ui'])
        write_json(run / 'launch-command.json', [str(app / 'Contents/MacOS/shadps4'), *arguments])
        with (run / 'launcher.stdout.log').open('w') as log:
            launcher = subprocess.Popen([str(app / 'Contents/MacOS/shadps4'), *arguments],
                                         env=env, stdout=log, stderr=subprocess.STDOUT)
        print(f'Результаты: {run}\n' + ('Автоповтор: игра запускается с записью контроллера.' if source
              else 'В открывшемся лаунчере запусти игру. Ввод записывается с начала игры, включая меню.'), flush=True)
        deadline = time.monotonic() + 3600
        while time.monotonic() < deadline:
            records = list((run / 'telemetry').glob('*_meta.txt'))
            if records:
                values = metadata(records[-1])
                if 'pid' in values:
                    pid = int(values['pid'])
                    break
            if launcher.poll() is not None:
                raise RuntimeError('Лаунчер завершился до начала телеметрии. Проверь его лог и повтори запускатель записи.')
            time.sleep(1)
        if pid is None or not alive(pid):
            raise RuntimeError('Не удалось дождаться процесса игры с включённой телеметрией.')
        mark('telemetry_ready', pid=pid)
        if source:
            deadline = time.monotonic() + args.replay_timeout
            while time.monotonic() < deadline:
                if not alive(pid):
                    raise RuntimeError('Игра закрылась во время автоповтора.')
                path = run / 'inputs/status.json'
                status = json.loads(path.read_text()) if path.exists() else {}
                if status.get('valid') is False or status.get('aborted'):
                    raise RuntimeError('Воспроизведение контроллера остановлено; см. inputs/status.json.')
                if status.get('frame', 0) >= replay_plan['benchmark_frame']:
                    break
                time.sleep(0.1)
            else:
                raise RuntimeError('Истёк срок ожидания места замера при автоповторе.')
            mark('replay_benchmark_reached', guest_frame=status['frame'])
            print('Достигнут записанный кадр замера. Сохраняю контрольный скриншот; затем замер.', flush=True)
        else:
            input('\nДойди до места с просадкой FPS. Затем вернись сюда и нажми Enter: ')
        require_disk_space([run, temp_root])
        # The user can change launcher settings before pressing Launch.
        if not source:
            snapshot(run, app)
        gpu_stage = f'{args.duration} с GPU, сохранение, 10 с паузы, ' if args.mode == 'gpu-cpu' else ''
        timing_stage = '' if args.no_gpu_timing else '15 с выборочных GPU-замеров и 2 с паузы,\n'
        print(('Автоповтор продолжает записанный ввод.\n' if source else
               'Вернись в игру за 10 секунд. Оставайся на том же месте с просадкой и не двигай камеру.\n') +
              f'Этапы: {args.baseline_seconds} с базового замера,\n'
              f'{timing_stage}{gpu_stage}{args.cpu_duration} с CPU,\n'
              'сохранение и ещё 10 с базового замера. Повторно нажимать Enter не нужно.', flush=True)
        if not source:
            delay(5)
        (run / 'inputs/screenshot').touch()
        mark('reference_screenshot_requested')
        delay(5)
        mark('baseline_start')
        delay(args.baseline_seconds)
        mark('baseline_end')
        if not args.no_gpu_timing:
            mark('gpu_timing_start')
            (run / 'gpu-timing/enable').touch()
            print('Выборочные GPU-замеры: 15 секунд, без Metal-трассы.' +
                  ('' if source else ' Оставайся на месте.'), flush=True)
            delay(15)
            (run / 'gpu-timing/enable').unlink(missing_ok=True)
            mark('gpu_timing_end')
            delay(2)
        identity = command(['ps', '-ww', '-p', str(pid), '-o', 'command=']).stdout
        if str(run / 'telemetry') not in identity:
            raise RuntimeError('Процесс игры изменился; запись не будет подключена к другому PID.')
        # Count temporary growth across both phases, including any raw files
        # retained after the first trace. Never overlap GPU and CPU collectors.
        original_temp = file_sizes(temp_root.glob('instruments*.ktrace'))
        phases = [('cpu', args.cpu_duration)]
        if args.mode == 'gpu-cpu':
            phases.insert(0, ('gpu', args.duration))
        for kind, seconds in phases:
            if kind == 'cpu' and args.mode == 'gpu-cpu':
                print('GPU-трасса сохранена. Пауза 10 секунд, затем CPU-профиль.', flush=True)
                mark('cooldown_start')
                delay(10)
            if not alive(pid):
                raise RuntimeError('Игра закрылась до завершения записи.')
            identity = command(['ps', '-ww', '-p', str(pid), '-o', 'command=']).stdout
            if str(run / 'telemetry') not in identity:
                raise RuntimeError('Процесс игры изменился; запись остановлена.')
            require_disk_space([run, temp_root])
            reason = guard_reason(trace_usage(run, temp_root, original_temp))
            if reason:
                raise RuntimeError(reason)
            trace_args = trace_command(pid, run / f'{kind}.trace', seconds, kind)
            write_json(run / f'{kind}-command.json', trace_args)
            prefix = 'trace' if kind == 'gpu' else 'cpu'
            mark(f'{prefix}_requested')
            with (run / f'{kind}-xctrace.log').open('w') as log:
                tracer = subprocess.Popen(trace_args, env=clean_env(), stdout=log, stderr=subprocess.STDOUT)
                print(f'{kind.upper()}: запись {seconds} с, затем сохранение.' +
                      ('' if source else ' Продолжай тот же участок.'), flush=True)
                code, reason = monitor_trace(tracer, run, temp_root, original_temp, seconds, mark)
            mark(f'{prefix}_finished', returncode=code)
            if reason:
                outcome[f'{kind}_stop_reason'] = reason
            if code:
                raise RuntimeError(f'{kind.upper()} не записан (xctrace: {code}). См. {kind}-xctrace.log. CSV сохранены.')
            exported = command(['xcrun', 'xctrace', 'export', '--input', str(run / f'{kind}.trace'),
                                '--toc', '--output', str(run / f'{kind}-toc.xml')], timeout=120)
            (run / f'{kind}-export.log').write_text(exported.stdout + exported.stderr)
            outcome[f'{kind}_recorded'] = exported.returncode == 0
            if exported.returncode:
                raise RuntimeError(f'Трасса {kind.upper()} не читается. См. {kind}-export.log.')
        outcome['trace_recorded'] = True
        mark('post_baseline_start')
        print('Трассы сохранены. Ещё 10 секунд базового замера.', flush=True)
        delay(10)
        mark('post_baseline_end')
        (run / 'inputs/screenshot').touch()
        mark('final_screenshot_requested')
        if source:
            delay(3)
            (run / 'inputs/stop').touch()
            mark('replay_close_requested')
            print('Автозамер завершён. Запрошено обычное закрытие игры.', flush=True)
        else:
            print('\aЗАПИСЬ ГОТОВА. Закрой игру обычным способом для завершения CSV и ввода.', flush=True)
        close_deadline = time.monotonic() + 30 if source else math.inf
        while alive(pid):
            if time.monotonic() >= close_deadline:
                raise RuntimeError('Игра не закрылась за 30 секунд после запроса автозамера.')
            time.sleep(1)
        mark('game_closed')
        outcome['complete'] = True
    except KeyboardInterrupt:
        outcome['error'] = 'Recording interrupted by user.'
        print('\nОстанавливаю запись. ' + ('Запрашиваю закрытие автоповтора.' if source else
              'Игра не закрывается автоматически.'), flush=True)
    except Exception as error:
        outcome['error'] = str(error)
        print(f'Ошибка: {error}', file=sys.stderr)
    finally:
        (run / 'gpu-timing/enable').unlink(missing_ok=True)
        if source and pid is not None and alive(pid):
            (run / 'inputs/stop').touch()
        if tracer is not None and tracer.poll() is None:
            tracer.send_signal(signal.SIGINT)
            try:
                tracer.wait(timeout=10)
            except subprocess.TimeoutExpired:
                tracer.kill()
                tracer.wait()
                outcome['trace_finalization_aborted'] = True
        mark('collector_finished')
        copied_logs = []
        for log_source in [SUPPORT / 'lbp3-launcher-core.log', SUPPORT / 'log/shad_log.txt']:
            if log_source.exists() and pid is not None:
                try:
                    copied_logs.append(copy_log_tail(log_source, run / log_source.name))
                except OSError as error:
                    outcome['log_copy_error'] = str(error)
        outcome['copied_logs'] = copied_logs
        outcome['screenshots'] = [{'name': p.name, 'bytes': p.stat().st_size,
                                   'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
                                  for p in sorted((run / 'screenshots').glob('*.png'))]
        if not source:
            try:
                plan = input_plan(run)
                write_json(run / 'inputs/replay-plan.json', plan)
                outcome['controller_recording'] = {'valid': True, 'states': plan['states'],
                    'bytes': plan['bytes'], 'benchmark_frame': plan['benchmark_frame']}
                print(f'Контроллер записан: {plan["states"]} состояний, '
                      f'{plan["bytes"] / 1024 ** 2:.1f} МиБ. Автозамер с кадра {plan["benchmark_frame"]}.', flush=True)
            except (RuntimeError, OSError, ValueError) as error:
                outcome['controller_recording'] = {'valid': False, 'error': str(error)}
                outcome['complete'] = False
                outcome.setdefault('error', f'Запись контроллера непригодна для повтора: {error}')
                print(f'Контроллер не удалось сохранить для повтора: {error}', file=sys.stderr)
        elif (run / 'inputs/status.json').exists():
            outcome['controller_replay'] = json.loads((run / 'inputs/status.json').read_text())
        try:
            summarize(run)
        except Exception as error:
            outcome['summary_error'] = str(error)
        write_json(run / 'status.json', outcome)
        print(f'Папка записи: {run}', flush=True)
    if not outcome['complete']:
        raise SystemExit(1)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError) as error:
        print(f'Ошибка: {error}', file=sys.stderr)
        raise SystemExit(1)
