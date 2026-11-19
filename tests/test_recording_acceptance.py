"""Offline acceptance checks; no camera connection and no settings writes."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock


MODULE_PATH = Path(__file__).resolve().parents[1]/'scripts/recording_acceptance.py'
SPEC = importlib.util.spec_from_file_location('recording_acceptance', MODULE_PATH)
acceptance = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(acceptance)


def probe(fps, frames=60):
    return {
        'streams': [{'width': 1280, 'height': 720, 'codec_name': 'h264',
                     'nb_read_frames': str(frames), 'avg_frame_rate': '30/1', 'r_frame_rate': '30/1'}],
        'frames': [{'best_effort_timestamp_time': str(index/fps), 'pkt_duration_time': str(1/fps)}
                   for index in range(frames)],
    }


class MediaValidationTests(unittest.TestCase):
    def test_30fps_accepts_sensor_clock_tolerance(self):
        result = acceptance.validate_probe(probe(29.97), 1280, 720, 30, 29.7)
        self.assertTrue(result['passed'])
        self.assertAlmostEqual(result['savedFileFps'], 29.97)
        self.assertEqual((result['codedWidth'], result['codedHeight']), (1280, 720))
        self.assertEqual((result['displayRotation'], result['displayWidth'], result['displayHeight']), (0, 1280, 720))

    def test_quarter_turn_display_matrix_restores_requested_dimensions(self):
        for rotation in (-90, 90):
            with self.subTest(rotation=rotation):
                value = probe(29.97)
                value['streams'][0].update(width=3040, height=4056, codec_name='mjpeg',
                    side_data_list=[{'side_data_type': 'Display Matrix', 'rotation': rotation}])
                result = acceptance.validate_probe(value, 4056, 3040, 30, 25)
                self.assertTrue(result['passed'])
                self.assertEqual((result['codedWidth'], result['codedHeight']), (3040, 4056))
                self.assertEqual((result['displayRotation'], result['displayWidth'], result['displayHeight']),
                                 (rotation, 4056, 3040))

    def test_swapped_dimensions_without_display_matrix_are_rejected(self):
        for extra in ({}, {'tags': {'rotate': '90'}}, {'side_data_list': [{'rotation': 90}]}):
            with self.subTest(extra=extra):
                value = probe(30)
                value['streams'][0].update(width=3040, height=4056, **extra)
                with self.assertRaisesRegex(ValueError, 'resolution differs'):
                    acceptance.validate_probe(value, 4056, 3040, 30, 25)

    def test_display_matrix_must_be_an_exact_supported_rotation(self):
        for rotation in (45, 89.999, -89.99, 180, 270, float('nan')):
            with self.subTest(rotation=rotation):
                value = probe(30)
                value['streams'][0].update(width=3040, height=4056,
                    side_data_list=[{'side_data_type': 'Display Matrix', 'rotation': rotation}])
                with self.assertRaises(ValueError):
                    acceptance.validate_probe(value, 4056, 3040, 30, 25)
        value = probe(30)
        value['streams'][0]['side_data_list'] = [{'side_data_type': 'Display Matrix'}]
        with self.assertRaisesRegex(ValueError, 'no rotation'):
            acceptance.validate_probe(value, 1280, 720, 30, 29.7)
        value['streams'][0]['side_data_list'] = [{'side_data_type': 'Display Matrix', 'rotation': 90}]*2
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            acceptance.validate_probe(value, 1280, 720, 30, 29.7)

    def test_nominal_30fps_cannot_hide_dropped_frames(self):
        result = acceptance.validate_probe(probe(15), 1280, 720, 30, 29.7)
        self.assertFalse(result['passed'])
        self.assertAlmostEqual(result['timestampFps'], 15)
        self.assertEqual(result['estimatedMissingFrames'], 59)

    def test_trailing_frozen_timeline_is_not_ignored(self):
        value = probe(30)
        value['format'] = {'duration': '4'}
        result = acceptance.validate_probe(value, 1280, 720, 30, 29.7)
        self.assertFalse(result['passed'])
        self.assertEqual(result['savedFileFps'], 15)

    def test_non_increasing_or_missing_timestamps_are_rejected(self):
        value = probe(30)
        value['frames'][2]['best_effort_timestamp_time'] = '0'
        with self.assertRaisesRegex(ValueError, 'strictly increasing'):
            acceptance.validate_probe(value, 1280, 720, 30, 29.7)
        value = probe(30)
        value['frames'].pop()
        with self.assertRaisesRegex(ValueError, 'inconsistent'):
            acceptance.validate_probe(value, 1280, 720, 30, 29.7)

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'FFmpeg unavailable')
    def test_real_files_are_decoded_and_timestamp_gaps_fail(self):
        with tempfile.TemporaryDirectory(prefix='oak-acceptance-') as directory:
            for sparse in (False, True):
                path = Path(directory)/('sparse.mp4' if sparse else 'complete.mp4')
                command = ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i',
                           'testsrc2=size=96x64:rate=30:duration=2']
                if sparse:
                    command += ['-vf', r'select=not(mod(n\,2))', '-vsync', 'vfr', '-r', '30']
                command += ['-an', '-c:v', 'libx264', '-preset', 'ultrafast', '-bf', '0', str(path)]
                subprocess.run(command, check=True, capture_output=True, timeout=30)
                result = acceptance.inspect_media(path, 96, 64, 30, 29.7, timeout=30)
                self.assertEqual(result['passed'], not sparse)
                self.assertTrue(result['fullDecodePassed'])

    @unittest.skipUnless(shutil.which('ffmpeg') and shutil.which('ffprobe'), 'FFmpeg unavailable')
    def test_mjpeg_mov_display_matrix_and_default_autorotation(self):
        with tempfile.TemporaryDirectory(prefix='oak-acceptance-rotation-') as directory:
            root = Path(directory)
            coded, displayed, decoded = root/'coded.mov', root/'displayed.mov', root/'decoded.png'
            subprocess.run(
                ['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=64x96:rate=30:duration=2',
                 '-an', '-c:v', 'mjpeg', '-threads', '1', '-q:v', '3', '-pix_fmt', 'yuvj420p', str(coded)],
                check=True, capture_output=True, timeout=30)
            subprocess.run(
                ['ffmpeg', '-v', 'error', '-i', str(coded), '-c', 'copy', '-metadata:s:v:0', 'rotate=90', str(displayed)],
                check=True, capture_output=True, timeout=30)
            result = acceptance.inspect_media(displayed, 96, 64, 30, 29.7, timeout=30)
            self.assertTrue(result['passed'])
            self.assertTrue(result['fullDecodePassed'])
            self.assertEqual((result['codedWidth'], result['codedHeight']), (64, 96))
            self.assertIn(result['displayRotation'], (-90, 90))
            self.assertEqual((result['displayWidth'], result['displayHeight']), (96, 64))
            # Export with default FFmpeg autorotation, then inspect the actual
            # decoded output image's size. A tag-only report cannot pass this.
            subprocess.run(
                ['ffmpeg', '-v', 'error', '-i', str(displayed), '-frames:v', '1', '-update', '1', str(decoded)],
                check=True, capture_output=True, timeout=30)
            geometry = subprocess.run(
                ['ffprobe', '-v', 'error', '-show_entries', 'stream=width,height', '-of', 'json', str(decoded)],
                check=True, capture_output=True, text=True, timeout=10)
            import json
            stream = json.loads(geometry.stdout)['streams'][0]
            self.assertEqual((stream['width'], stream['height']), (96, 64))


class OwnershipTests(unittest.TestCase):
    def test_existing_recording_is_never_stopped(self):
        client = mock.Mock()
        client.request.return_value = {'cameraConnected': True, 'recording': True}
        with self.assertRaisesRegex(RuntimeError, 'existing recording'):
            acceptance.record_current(client, 30, {})
        client.request.assert_called_once_with()

    def test_changed_recording_is_never_stopped(self):
        client = mock.Mock()
        client.request.return_value = {'lastRecordingPath': '/new/other.mp4'}
        with self.assertRaisesRegex(RuntimeError, 'identity changed'):
            acceptance.stop_owned(client, '/own/video.mp4')
        client.request.assert_called_once_with()

    def test_loopback_only_client(self):
        with self.assertRaisesRegex(ValueError, 'loopback'):
            acceptance.CameraClient('http://192.168.1.2:18080')
        with self.assertRaisesRegex(ValueError, 'base URL'):
            acceptance.CameraClient('http://user:password@localhost:18080')

    def test_unknown_stop_is_not_retried(self):
        client = mock.Mock()
        baseline = {'cameraConnected': True, 'recording': False, 'lastRecordingPath': ''}
        settings = {'applyState': 'applied', 'activeSettings': {'recording': {'width': 1280, 'height': 720, 'fps': 30}}}
        started = {'success': True, 'recording': True, 'path': '/own/video.mp4', 'message': 'recording started'}
        owned = {'recording': True, 'lastRecordingPath': '/own/video.mp4'}
        client.request.side_effect = [baseline, settings, {'native': {}}, started, owned, TimeoutError('lost stop response')]
        with mock.patch.object(acceptance.time, 'monotonic', side_effect=[0, 2, 2]):
            with self.assertRaisesRegex(TimeoutError, 'lost stop'):
                acceptance.record_current(client, 1, {})
        writes = [call for call in client.request.call_args_list if len(call.args) > 1]
        self.assertEqual(writes, [mock.call('/camera/record/start', 'POST'), mock.call('/camera/record/stop', 'POST')])

    def test_unknown_start_only_queries_status(self):
        client = mock.Mock()
        baseline = {'cameraConnected': True, 'recording': False, 'lastRecordingPath': ''}
        settings = {'applyState': 'applied', 'activeSettings': {'recording': {'width': 1280, 'height': 720, 'fps': 30}}}
        ongoing = {'recording': True, 'lastRecordingPath': '/unknown/video.mp4'}
        client.request.side_effect = [baseline, settings, {'native': {}}, TimeoutError('lost start response'), ongoing]
        report = {}
        with self.assertRaisesRegex(TimeoutError, 'lost start'):
            acceptance.record_current(client, 1, report)
        self.assertTrue(report['startResultUnknown'])
        self.assertEqual(report['startReadback'], ongoing)
        writes = [call for call in client.request.call_args_list if len(call.args) > 1]
        self.assertEqual(writes, [mock.call('/camera/record/start', 'POST')])


if __name__ == '__main__':
    unittest.main()
