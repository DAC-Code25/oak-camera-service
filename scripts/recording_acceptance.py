#!/usr/bin/env python3
"""Record and validate the camera's current settings on the IPC.

Run only during an authorized recording test. This script neither changes camera
settings nor restarts services. It retains the recorded media and its JSON report.
720p/30 acceptance uses 29.7 real FPS (1% sensor-clock tolerance); larger outputs
require 25 FPS. Encoder labels and nominal container FPS cannot satisfy the check.
"""
import argparse
import datetime
import json
import math
from pathlib import Path
import subprocess
import time
import urllib.parse
import urllib.request


def number(value, label):
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(label + ' is not finite')
    return result


def display_geometry(stream):
    coded_width, coded_height = stream.get('width'), stream.get('height')
    rotation = 0
    matrices = [side for side in stream.get('side_data_list', [])
                if side.get('side_data_type') == 'Display Matrix']
    if len(matrices) > 1:
        raise ValueError('ambiguous recording display matrices')
    if matrices:
        if 'rotation' not in matrices[0]:
            raise ValueError('recording display matrix has no rotation')
        rotation = number(matrices[0]['rotation'], 'display matrix rotation')
        if rotation not in (-90, 0, 90):
            raise ValueError('unsupported recording display matrix rotation')
    # Only FFprobe's Display Matrix side data establishes a quarter turn. A
    # rotate tag or swapped dimensions alone cannot establish displayed size.
    displayed = (coded_height, coded_width) if rotation in (-90, 90) else (coded_width, coded_height)
    return coded_width, coded_height, int(rotation), displayed[0], displayed[1]


def validate_probe(probe, width, height, configured_fps, minimum_fps):
    """Validate decoded timestamps, independent of advertised r/avg_frame_rate."""
    streams = probe.get('streams', [])
    if len(streams) != 1:
        raise ValueError('expected exactly one selected video stream')
    stream = streams[0]
    coded_width, coded_height, rotation, display_width, display_height = display_geometry(stream)
    if (display_width, display_height) != (width, height):
        raise ValueError('saved resolution differs from active recording settings')
    frames = probe.get('frames', [])
    count = int(stream['nb_read_frames'])
    if count < 2 or len(frames) != count:
        raise ValueError('decoded frame count is missing or inconsistent')
    stamps = [number(frame['best_effort_timestamp_time'], 'frame timestamp') for frame in frames]
    gaps = [later-earlier for earlier, later in zip(stamps, stamps[1:])]
    if any(gap <= 0 for gap in gaps):
        raise ValueError('decoded timestamps are not strictly increasing')
    last_duration = number(frames[-1].get('pkt_duration_time', 1/configured_fps), 'last frame duration')
    if last_duration <= 0:
        last_duration = 1/configured_fps
    span = stamps[-1]-stamps[0]
    duration = span + last_duration
    # A longer declared duration also lowers the saved file's effective FPS.
    for value in (stream.get('duration'), probe.get('format', {}).get('duration')):
        if value not in (None, 'N/A'):
            duration = max(duration, number(value, 'media duration'))
    cadence_fps = (count-1)/span
    media_fps = count/duration
    result = {
        'width': width, 'height': height, 'codec': stream.get('codec_name'),
        'codedWidth': coded_width, 'codedHeight': coded_height,
        'displayRotation': rotation, 'displayWidth': display_width, 'displayHeight': display_height,
        'decodedFrames': count, 'durationSeconds': duration,
        'timestampFps': cadence_fps, 'savedFileFps': media_fps,
        'minimumFps': minimum_fps, 'maxFrameGapSeconds': max(gaps),
        'estimatedMissingFrames': sum(max(0, round(gap*configured_fps)-1) for gap in gaps),
        'passed': min(cadence_fps, media_fps) >= minimum_fps,
    }
    return result


def inspect_media(path, width, height, configured_fps, minimum_fps, timeout=180):
    path = Path(path)
    if not path.is_absolute() or not path.is_file():
        raise ValueError('returned recording must be an existing local absolute path; run this tool on the IPC')
    command = ['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
               '-show_frames', '-show_streams', '-show_format', '-show_entries',
               'frame=best_effort_timestamp_time,pkt_duration_time:stream=width,height,codec_name,nb_read_frames,duration:stream_side_data_list:format=duration',
               '-of', 'json', str(path)]
    completed = subprocess.run(command, check=True, capture_output=True, text=True, timeout=timeout)
    result = validate_probe(json.loads(completed.stdout), width, height, configured_fps, minimum_fps)
    # FFmpeg's default autorotation applies the same display matrix while
    # decoding. Do not use -noautorotate for the native portrait-coded stream.
    subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', str(path), '-map', '0:v:0',
                    '-f', 'null', '-'], check=True, capture_output=True, timeout=timeout)
    result.update(path=str(path), bytes=path.stat().st_size, fullDecodePassed=True)
    return result


class CameraClient:
    def __init__(self, base_url):
        parsed = urllib.parse.urlsplit(base_url)
        if parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', 'localhost', '::1'):
            raise ValueError('run on the IPC with a loopback HTTP camera URL')
        if parsed.username or parsed.password or parsed.query or parsed.fragment or parsed.path not in ('', '/'):
            raise ValueError('base URL must contain only loopback host and port')
        self.base_url = base_url.rstrip('/')
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

    def request(self, path='/camera/status', method='GET'):
        payload = b'{}' if method == 'POST' else None
        request = urllib.request.Request(self.base_url+path, data=payload, method=method,
                                         headers={'Content-Type': 'application/json'})
        # No write request is automatically retried after a timeout/lost response.
        with self.opener.open(request, timeout=10) as response:
            data = response.read(1024*1024+1)
        if len(data) > 1024*1024:
            raise RuntimeError('camera response exceeds limit')
        return json.loads(data)


def stop_owned(client, path):
    state = client.request()
    if state.get('lastRecordingPath') != path:
        raise RuntimeError('recording identity changed; refusing to stop another recording')
    return client.request('/camera/record/stop', 'POST')


def record_current(client, seconds, report, sample_interval=1):
    before = client.request()
    if before.get('recording') is not False or not before.get('cameraConnected'):
        raise RuntimeError('camera is unavailable or an existing recording is active/unknown')
    settings = client.request('/camera/settings')
    if settings.get('applyState') != 'applied' or not settings.get('activeSettings'):
        raise RuntimeError('camera settings are not confirmed applied')
    active = settings['activeSettings']
    recording = active['recording']
    capabilities = client.request('/camera/capabilities')
    width = recording['width'] or capabilities['native']['width']
    height = recording['height'] or capabilities['native']['height']
    fps = number(recording['fps'], 'configured FPS')
    minimum = 29.7 if width <= 1280 and height <= 720 else 25.0
    required = 30 if minimum == 29.7 else 25
    if fps < required:
        raise RuntimeError('current configured FPS is below the requested performance target; apply settings explicitly first')
    report.update(settings=settings, capabilities=capabilities, baseline=before,
                  requiredFps=minimum, samples=[], width=width, height=height)
    owned_path = None
    stopped = False
    try:
        try:
            started = client.request('/camera/record/start', 'POST')
        except Exception:
            report['startResultUnknown'] = True
            try:
                report['startReadback'] = client.request()
            except Exception as error:
                report['startReadbackError'] = str(error)
            raise
        report['start'] = started
        if not started.get('success') or not started.get('recording'):
            raise RuntimeError('recording start failed: '+str(started))
        path = started.get('path')
        if not path or path == before.get('lastRecordingPath') or 'already' in started.get('message', '').lower():
            raise RuntimeError('start did not identify a newly created recording; refusing to take ownership')
        owned_path = path
        started_at = time.monotonic()
        until = started_at + seconds
        while time.monotonic() < until:
            time.sleep(min(sample_interval, max(0, until-time.monotonic())))
            sample = client.request()
            report['samples'].append(sample)
            if sample.get('lastRecordingPath') != owned_path or not sample.get('recording'):
                raise RuntimeError('own recording stopped/changed during measurement')
        report['observationSeconds'] = time.monotonic()-started_at
        # The stop operation has been sent once. An uncertain response is reported;
        # cleanup must not send a second write request.
        stopped = True
        try:
            stop = stop_owned(client, owned_path)
        except Exception:
            report['stopResultUnknown'] = True
            try:
                report['stopReadback'] = client.request()
            except Exception as error:
                report['stopReadbackError'] = str(error)
            raise
        report['stop'] = stop
        if not stop.get('success') or stop.get('lastRecordingError'):
            raise RuntimeError('recording finalization failed: '+str(stop))
        media = inspect_media(owned_path, width, height, fps, minimum,
                              timeout=max(180, int(seconds*3)))
        if stop.get('recordingCodec') and media['codec'] != stop['recordingCodec']:
            raise RuntimeError('decoded media codec differs from camera recording status')
        return media
    finally:
        if owned_path and not stopped:
            # Query before cleanup. A start response lost before path ownership is
            # known is left untouched and reported as an unknown operation result.
            try:
                state = client.request()
                if state.get('lastRecordingPath') == owned_path:
                    report['cleanupStop'] = stop_owned(client, owned_path)
                else:
                    report['cleanupError'] = 'recording identity changed; no stop sent'
            except Exception as error:
                report['cleanupError'] = str(error)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base-url', default='http://127.0.0.1:18080')
    parser.add_argument('--seconds', type=float, default=30)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.seconds) or not 10 <= args.seconds <= 600:
        parser.error('--seconds must be between 10 and 600')
    if args.report.exists():
        parser.error('--report already exists; use a new report path')
    report = {'startedAtUtc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'settingsChanged': False, 'serviceRestarted': False, 'passed': False}
    try:
        report['media'] = record_current(CameraClient(args.base_url), args.seconds, report)
        report['passed'] = report['media']['passed']
    except Exception as error:
        report['error'] = str(error)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open('x', encoding='utf-8') as output:
        json.dump(report, output, ensure_ascii=False, indent=2)
        output.write('\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
