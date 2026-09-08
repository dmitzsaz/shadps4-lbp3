import importlib.util
import csv
import json
import os
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location(
    'recorder', Path(__file__).with_name('record-lbp3-performance.py'))
recorder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(recorder)


class RecorderTests(unittest.TestCase):
    def test_home_snapshot_and_replay_copy_are_independent(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            home = base / 'live-home'
            (home / '1000/savedata/CUSA00063').mkdir(parents=True)
            save = home / '1000/savedata/CUSA00063/save'
            save.write_bytes(b'initial progress')
            source = base / 'source'; source.mkdir()
            (source / 'inputs').mkdir()
            (source / 'inputs/controller.bin').write_bytes(b'recorded input')
            run = base / 'replay'; run.mkdir()
            support = base / 'support'; support.mkdir()
            (support / 'config.json').write_text('{}')
            with patch.object(recorder, 'home_directory', return_value=home), patch.object(recorder, 'SUPPORT', support):
                recorder.snapshot_home(source)
                recorder.write_json(source / 'input-environment.json', {'user_layout_sha256': None})
                recorder.write_json(source / 'settings.json', {'launcher': {}})
                with patch.object(recorder, 'input_plan', return_value={'benchmark_frame': 15,
                    'sha256': recorder.hashlib.sha256(b'recorded input').hexdigest()}):
                    recorder.prepare_replay(source, run)
                self.assertEqual((run / 'replay-controller.bin').read_bytes(), b'recorded input')
                (run / 'replay-controller.bin').write_bytes(b'changed working copy')
                self.assertEqual((source / 'inputs/controller.bin').read_bytes(), b'recorded input')
                (run / 'replay-home/1000/savedata/CUSA00063/save').write_bytes(b'test progress')
                self.assertEqual(save.read_bytes(), b'initial progress')
                self.assertEqual((source / 'initial-home/1000/savedata/CUSA00063/save').read_bytes(), b'initial progress')
                (source / 'initial-home/symlink').symlink_to(save)
                with patch.object(recorder, 'input_plan', return_value={}), self.assertRaisesRegex(RuntimeError, 'ссылку'):
                    recorder.prepare_replay(source)

    def test_automatic_replay_never_prompts_and_requests_normal_shutdown(self):
        self.exercise_capture(replay=True)

    def test_manual_capture_finalizes_controller_plan_after_normal_exit(self):
        self.exercise_capture(replay=False)

    def exercise_capture(self, replay):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            app = base / 'shadPS4-lbp3.app'
            (app / 'Contents/Resources').mkdir(parents=True)
            eboot = app / 'Contents/Resources/Game/CUSA00063/eboot.bin'
            eboot.parent.mkdir(parents=True)
            eboot.write_bytes(b'embedded game')
            (app / 'Contents/Resources/Addons/CUSA00063').mkdir(parents=True)
            recorder.write_json(app / 'Contents/Resources/BuildInfo.json', {
                'controller_movie_version': 1, 'gpu_work_timing_version': 1,
                'precise_compute_timestamps_version': 1})
            source = base / 'source'; source.mkdir()
            config = {'resolution': '1920x1080', 'fullscreen': True, 'show_fps': True,
                      'patch_prize_bubbles': False, 'disable_sprite_lights': False,
                      'disable_tone_map': False, 'lbp3_online': False}
            settings = {'launcher': config}
            launched = []
            active_run = None
            def snapshot(run, unused):
                recorder.write_json(run / 'settings.json', settings)
            def prepare(unused, run=None):
                if run:
                    (run / 'replay-home').mkdir()
                    (run / 'replay-controller.bin').write_bytes(b'working input')
                return {'benchmark_frame': 10}, settings
            def popen(args, **kwargs):
                nonlocal active_run
                launched.append((args, kwargs.get('env')))
                if len(launched) == 1:
                    env = kwargs['env']
                    active_run = Path(env.get('SHADPS4_INPUT_REPORT', env.get('SHADPS4_INPUT_RECORD'))).parent
                    (active_run / 'telemetry/test_meta.txt').write_text('pid=4242\n')
                    recorder.write_json(active_run / 'inputs/status.json', {'valid': True, 'frame': 10})
                return Mock(poll=Mock(return_value=None if len(launched) == 1 else 0))
            def alive(unused):
                if active_run is None:
                    return False
                if replay:
                    return not (active_run / 'inputs/stop').exists()
                return 'post_baseline_end' not in (active_run / 'markers.jsonl').read_text()
            def command(args, **kwargs):
                return SimpleNamespace(returncode=0, stdout=str(active_run / 'telemetry'), stderr='')
            arguments = ['recorder', '--app', str(app), '--output-root', str(base / 'runs')]
            if replay:
                arguments += ['--replay', str(source)]
            with patch.object(recorder.sys, 'argv', arguments), \
                 patch.object(recorder, 'preflight'), patch.object(recorder, 'require_disk_space'), \
                 patch.object(recorder, 'home_directory', return_value=base / 'live-home'), \
                 patch.object(recorder, 'prepare_replay', side_effect=prepare), \
                 patch.object(recorder, 'snapshot_home'), \
                 patch.object(recorder, 'input_plan', return_value={'benchmark_frame': 10, 'states': 20, 'bytes': 2416}), \
                 patch.object(recorder, 'snapshot', side_effect=snapshot), \
                 patch.object(recorder.subprocess, 'Popen', side_effect=popen), \
                 patch.object(recorder, 'alive', side_effect=alive), \
                 patch.object(recorder, 'command', side_effect=command), \
                 patch.object(recorder, 'trace_usage', return_value={'free_bytes': 20 * recorder.GIB, 'trace_and_session_bytes': 0}), \
                 patch.object(recorder, 'monitor_trace', return_value=(0, None)), \
                 patch.object(recorder, 'summarize'), patch.object(recorder.time, 'sleep'), \
                 patch.object(recorder, 'SUPPORT', base / 'no-live-logs'), \
                 patch('builtins.input', side_effect=AssertionError('automatic replay asked for input') if replay else None) as prompt:
                recorder.main()
            args, env = launched[0]
            if replay:
                self.assertEqual(args[args.index('-g') + 1], str(eboot.resolve()))
                self.assertEqual(args[args.index('--set-addon-folder') + 1],
                                 str((app / 'Contents/Resources/Addons').resolve()))
                self.assertNotIn('--launcher-ui', args)
                self.assertEqual(Path(env['SHADPS4_INPUT_REPLAY']).resolve(),
                                 (active_run / 'replay-controller.bin').resolve())
                self.assertEqual(env['SHADPS4_REPLAY_HOME'], str(active_run / 'replay-home'))
                self.assertTrue((active_run / 'inputs/stop').exists())
                prompt.assert_not_called()
            else:
                self.assertIn('--launcher-ui', args)
                self.assertNotIn('-g', args)  # Preserve the GUI's choice of the bundled game.
                self.assertEqual(env['SHADPS4_INPUT_RECORD'], str(active_run / 'inputs'))
                self.assertNotIn('SHADPS4_REPLAY_HOME', env)
                self.assertFalse((active_run / 'inputs/stop').exists())
                self.assertTrue((active_run / 'inputs/replay-plan.json').is_file())
                prompt.assert_called_once()
            result = json.loads((active_run / 'status.json').read_text())
            self.assertTrue(result['complete'])
            self.assertIn('controller_replay' if replay else 'controller_recording', result)
            events = [json.loads(line)['event'] for line in (active_run / 'markers.jsonl').read_text().splitlines()]
            self.assertIn('gpu_timing_start', events)
            self.assertEqual('replay_close_requested' in events, replay)
            self.assertIn('game_closed', events)

    def test_replay_uses_current_bundle_instead_of_old_external_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'shadPS4-lbp3.app'
            eboot = app / 'Contents/Resources/Game/CUSA00063/eboot.bin'
            eboot.parent.mkdir(parents=True)
            eboot.write_bytes(b'game')
            addons = app / 'Contents/Resources/Addons'
            (addons / 'CUSA00063').mkdir(parents=True)
            config = dict(resolution='1920x1080', fullscreen=True, show_fps=True,
                          patch_prize_bubbles=False, disable_sprite_lights=False,
                          disable_tone_map=False, lbp3_online=False,
                          external_eboot='/old/Desktop/game/eboot.bin', addon_root='/old/Desktop/Addons')
            args = recorder.replay_arguments({'launcher': config}, app)
            self.assertEqual(args[args.index('-g') + 1], str(eboot.resolve()))
            self.assertEqual(args[args.index('--set-addon-folder') + 1], str(addons.resolve()))
            self.assertFalse(any('/old/Desktop/' in arg for arg in args))

    def test_missing_or_externally_linked_bundled_game_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'shadPS4-lbp3.app'
            eboot = app / 'Contents/Resources/Game/CUSA00063/eboot.bin'
            eboot.parent.mkdir(parents=True)
            (app / 'Contents/Resources/Addons/CUSA00063').mkdir(parents=True)
            with self.assertRaisesRegex(RuntimeError, 'встроенный eboot'):
                recorder.bundled_game_paths(app)
            external = Path(directory) / 'external-game'
            external.write_bytes(b'external')
            eboot.symlink_to(external)
            with self.assertRaisesRegex(RuntimeError, 'встроенный eboot'):
                recorder.bundled_game_paths(app)

    def test_protected_paths_follow_symlinks_but_allow_desktop_app_alias(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory).resolve()
            home = base / 'home'
            desktop = home / 'Desktop'
            desktop.mkdir(parents=True)
            app = base / 'Applications/shadPS4-lbp3.app'
            app.mkdir(parents=True)
            alias = desktop / 'shadPS4-lbp3.app'
            alias.symlink_to(app)
            hidden = base / 'capture-alias'
            hidden.symlink_to(desktop)
            with patch.object(recorder.Path, 'home', return_value=home):
                self.assertEqual(recorder.require_runtime_path(alias), app)
                for path in [desktop / 'captures', hidden / 'captures', home / 'Documents/game']:
                    with self.assertRaisesRegex(RuntimeError, 'защищённой папке'):
                        recorder.require_runtime_path(path)

    def test_gpu_timing_rejects_overlapping_truncated_and_partial_scopes(self):
        header = ('scheduler,tick,record_begin_steady_ns,submit_steady_ns,kind,index,status,'
                  'gpu_begin_ticks,gpu_end_ticks,gpu_ms,offset_ms,render_passes,scope_count,'
                  'scope_overflow,address,bytes,width,height,depth,pitch,bits,tile_mode,mips,layers').split(',')
        def row(index, kind, duration, offset, status='ok'):
            result = {key: 0 for key in header}
            result.update(scheduler='draw', tick=1, kind=kind, index=index, status=status,
                          gpu_ms=duration, offset_ms=offset, scope_count=2)
            return result
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'gpu-timing').mkdir()
            path = run / 'gpu-timing/test.csv'
            for offset, status, valid in [(5, 'ok', True), (2, 'ok', False), (5, 'truncated', False)]:
                with path.open('w') as stream:
                    writer = csv.DictWriter(stream, fieldnames=header)
                    writer.writeheader()
                    writer.writerows([row(0, 'submission', 10, 0),
                                      row(1, 'detile_dispatch', 3, 1),
                                      row(2, 'tile_download_dispatch', 2, offset, status)])
                    stream.write('draw,2,')
                result = recorder.summarize_gpu_timing(run)
                self.assertEqual(result['files'][0]['invalid_rows'], 1)
                self.assertEqual(result['submissions'][0]['valid_intervals'], valid)
                self.assertEqual(result['submissions'][0]['unattributed_gpu_ms'], 5 if valid else None)
                self.assertEqual(len(result['work_groups']), 2 if valid else 0)

    def test_gpu_waits_clip_cross_phase_intervals_and_read_rotated_history(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            telemetry = run / 'telemetry'
            telemetry.mkdir()
            path = telemetry / 'data_gpu_waits.csv'
            (telemetry / 'data_meta.txt').write_text(
                'start_unix_ns=1000000000\ngpu_wait_attribution_version=1\ngpu_wait_log_open=1\n'
                f'gpu_waits=/old/location/{path.name}\n')
            header = ['event', 'start_ms', 'end_ms', 'wait_ms', 'context_bits', 'source',
                      'resource', 'caller_function', 'caller_file', 'caller_line']
            first = [1, 10, 30, 20, 3, 'stream_reuse', 'tile_scratch', 'Reserve<int, int>', 'buffer.cpp', 9]
            second = [2, 31, 35, 4, 8, 'stream_reuse', 'upload_staging', 'Wait', 'buffer.cpp', 9]
            for name, rows in [('data_gpu_waits.previous.csv', [first]), (path.name, [first, second])]:
                with (telemetry / name).open('w') as stream:
                    writer = csv.writer(stream)
                    writer.writerow(header)
                    writer.writerows(rows)
            with path.open('a') as stream:
                stream.write('3,36,')
            (telemetry / 'data_gpu_waits_status.txt').write_text('rotations=1\ndropped_busy=2\n')
            summary = recorder.summarize_gpu_waits(run, {
                'baseline_start': 1_020_000_000, 'baseline_end': 1_025_000_000,
                'cpu_requested': 1_025_000_000, 'cpu_finished': 1_033_000_000})
            phases = summary['phases']
            self.assertEqual(sum(row['events'] for row in phases['retained_session']), 2)
            self.assertEqual(sum(row['overlap_ms'] for row in phases['retained_session']), 24)
            self.assertEqual(phases['baseline'][0]['overlap_ms'], 5)
            self.assertEqual(sum(row['overlap_ms'] for row in phases['cpu_window_approx']), 7)
            self.assertEqual(phases['baseline'][0]['contexts'], ['texture_cache_lock', 'image_refresh'])
            self.assertEqual(summary['sessions'][0]['invalid_rows'], 1)
            self.assertEqual(summary['sessions'][0]['status']['dropped_busy'], '2')

    def test_low_space_is_rejected_before_recording(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(recorder.shutil, 'disk_usage', return_value=Mock(free=recorder.GIB)):
                with self.assertRaisesRegex(RuntimeError, 'Недостаточно места'):
                    recorder.require_disk_space([Path(directory) / 'not-created-yet'])

    def test_storage_counts_temporary_trace_growth_not_just_output(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            run = base / 'run'
            run.mkdir()
            (run / 'session.trace').write_bytes(b'a' * 50)
            old = base / 'instruments-old.ktrace'
            old.write_bytes(b'b' * 100)
            initial = recorder.file_sizes([old])
            old.write_bytes(b'b' * 110)
            (base / 'instruments-new.ktrace').write_bytes(b'c' * 200)
            self.assertEqual(recorder.trace_usage(run, base, initial)['trace_and_session_bytes'], 260)

    def test_guard_stops_tracer_for_size_or_space_and_preserves_game(self):
        for usage in [{'free_bytes': 20 * recorder.GIB,
                       'trace_and_session_bytes': 3 * recorder.GIB},
                      {'free_bytes': 5 * recorder.GIB,
                       'trace_and_session_bytes': 0}]:
            tracer = Mock()
            tracer.poll.side_effect = [None, 0]
            tracer.wait.return_value = 0
            with patch.object(recorder, 'trace_usage', return_value=usage), \
                    patch.object(recorder.time, 'sleep'), patch.object(recorder.os, 'kill') as game_kill:
                code, reason = recorder.monitor_trace(tracer, Path('/run'), Path('/tmp'), {}, 10, Mock())
            self.assertEqual(code, 0)
            self.assertTrue(reason)
            tracer.send_signal.assert_called_once_with(recorder.signal.SIGINT)
            tracer.kill.assert_not_called()
            game_kill.assert_not_called()

    def test_finalization_cannot_wait_forever(self):
        tracer = Mock()
        tracer.poll.return_value = None
        tracer.wait.return_value = -9
        usage = {'free_bytes': 20 * recorder.GIB, 'trace_and_session_bytes': 0}
        with patch.object(recorder, 'trace_usage', return_value=usage), \
                patch.object(recorder.time, 'monotonic', side_effect=[0, 21, 82]), \
                patch.object(recorder.time, 'sleep'):
            code, reason = recorder.monitor_trace(tracer, Path('/run'), Path('/tmp'), {}, 10, Mock())
        self.assertEqual(code, -9)
        self.assertIn('60 секунд', reason)
        tracer.kill.assert_called_once()

    def test_critical_space_kills_only_tracer_without_waiting_for_save(self):
        tracer = Mock()
        tracer.poll.return_value = None
        tracer.wait.return_value = -9
        usage = {'free_bytes': 3 * recorder.GIB, 'trace_and_session_bytes': 0}
        with patch.object(recorder, 'trace_usage', return_value=usage):
            code, reason = recorder.monitor_trace(tracer, Path('/run'), Path('/tmp'), {}, 10, Mock())
        self.assertEqual(code, -9)
        tracer.kill.assert_called_once()
        tracer.send_signal.assert_not_called()

    def test_large_log_is_copied_as_bounded_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            source, dest = Path(directory) / 'source', Path(directory) / 'dest'
            source.write_bytes(b'old-prefix' + b'new-tail')
            info = recorder.copy_log_tail(source, dest, limit=8)
            self.assertEqual(dest.read_bytes(), b'new-tail')
            self.assertTrue(info['tail_only'])

    def test_environment_does_not_propagate_credentials_or_debug_layers(self):
        with patch.dict(os.environ, {'HOME': '/test home', 'PATH': '/usr/bin',
                                     'API_TOKEN': 'not-a-real-token',
                                     'MTL_CAPTURE_ENABLED': '1'}, clear=True):
            self.assertEqual(recorder.clean_env(), {'HOME': '/test home', 'PATH': '/usr/bin'})

    def test_trace_targets_one_process_and_preserves_paths(self):
        args = recorder.trace_command(123, Path('/test path/session.trace'), 5)
        self.assertEqual(args[args.index('--attach') + 1], '123')
        self.assertEqual(args[args.index('--output') + 1], '/test path/session.trace')
        self.assertNotIn('--all-processes', args)
        self.assertNotIn('Time Profiler', args)
        self.assertNotIn('Metal System Trace', args)
        self.assertIn('GPU', args)

    def test_cpu_capture_is_separate_from_metal(self):
        args = recorder.trace_command(123, Path('/test path/cpu.trace'), 10, 'cpu')
        self.assertIn('Time Profiler', args)
        self.assertNotIn('GPU', args)
        self.assertNotIn('Metal System Trace', args)
        with self.assertRaises(ValueError):
            recorder.trace_command(123, Path('/test.trace'), 5, 'typo')

    def test_cpu_and_post_baseline_have_distinct_summary_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'telemetry').mkdir()
            frames = run / 'telemetry/frames.csv'
            frames.write_text('frame_ms,elapsed_ms\n33,100\n40,200\n50,300\n')
            (run / 'telemetry/data_meta.txt').write_text(f'start_unix_ns=0\nframes={frames}\n')
            marks = [('baseline_start', 50000000), ('baseline_end', 140000000),
                     ('cpu_requested', 150000000), ('cpu_finished', 250000000),
                     ('post_baseline_start', 260000000), ('post_baseline_end', 310000000)]
            (run / 'markers.jsonl').write_text(''.join(
                json.dumps({'event': k, 'unix_ns': v}) + '\n' for k, v in marks))
            recorder.summarize(run)
            phases = json.loads((run / 'summary.json').read_text())['phases']
            self.assertEqual(phases['cpu_window_approx']['frame_ms']['p50'], 40)
            self.assertEqual(phases['baseline']['frame_ms']['p50'], 33)
            self.assertEqual(phases['post_baseline']['frame_ms']['p50'], 50)
            self.assertNotIn('trace_window_approx', phases)

    def test_summary_aligns_relative_frames_to_wall_clock_and_ignores_partial_row(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'telemetry').mkdir()
            frames = run / 'telemetry/example_frames.csv'
            frames.write_text('frame_ms,elapsed_ms,gpu_wait_ms\n'
                              '30,100,1\n40,200,2\n50,300,3\n35,400,4\npartial,\n')
            (run / 'telemetry/example_meta.txt').write_text(
                f'start_unix_ns=1000000000\nframes={frames}\n')
            markers = [('baseline_start', 1_150_000_000), ('trace_requested', 1_250_000_000),
                       ('trace_finished', 1_350_000_000)]
            (run / 'markers.jsonl').write_text(''.join(
                json.dumps({'event': name, 'unix_ns': timestamp}) + '\n'
                for name, timestamp in markers))
            recorder.summarize(run)
            phases = json.loads((run / 'summary.json').read_text())['phases']
            self.assertEqual(phases['all_session']['frames'], 4)
            self.assertEqual(phases['all_session']['frames_over_40ms'], 1)
            self.assertEqual(phases['baseline']['timing_totals_ms']['gpu_wait_ms'], 2)
            self.assertEqual(phases['trace_window_approx']['frame_ms']['p95'], 50)
            self.assertAlmostEqual(phases['all_session']['fps_from_total_frame_time'], 4000 / 155)

    def test_no_frames_is_an_empty_summary_not_a_false_success(self):
        with tempfile.TemporaryDirectory() as directory:
            run = Path(directory)
            (run / 'telemetry').mkdir()
            (run / 'markers.jsonl').write_text('')
            recorder.summarize(run)
            self.assertEqual(json.loads((run / 'summary.json').read_text())['phases'], {})


if __name__ == '__main__':
    unittest.main()
