#!/usr/bin/env python3
"""Install a verified candidate, validate real media, or restore the prior service.

Run explicitly as root during an authorized camera maintenance window. Only the
binary, this project's drop-in and its settings file are part of the transaction.
Existing Capture media is never removed. Old copies exist only until validation.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.request

SERVICE = 'tenco-oak-camera.service'
TARGET = Path('/home/nvidia/tenco-project/install/camera/oak_camera_service')
ROOT = Path('/home/nvidia/tenco-project/oak-camera-service')
DROP_IN = Path('/etc/systemd/system/tenco-oak-camera.service.d/20-native-capture.conf')
SETTINGS = ROOT / 'camera-settings.json'


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def request(path='/camera/status', method='GET'):
    req = urllib.request.Request('http://127.0.0.1:18080'+path, method=method,
                                 data=b'{}' if method == 'POST' else None)
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def ready(timeout=60):
    until = time.monotonic()+timeout
    previous, healthy = -1, 0
    while time.monotonic() < until:
        try:
            state = request()
            frame = state.get('frameId', 0)
            if state.get('cameraConnected') and state.get('success') and frame > previous:
                healthy += 1
                if healthy >= 3:
                    return state
            else:
                healthy = 0
            previous = frame
        except OSError:
            healthy = 0
        time.sleep(1)
    raise RuntimeError('camera did not produce continuous fresh frames')


def install(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    pending = target.with_name(target.name+'.pending-deploy')
    shutil.copy2(source, pending)
    os.replace(pending, target)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('expected_sha256')
    parser.add_argument('--drop-in', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise RuntimeError('root required')
    binary = args.binary.resolve(strict=True)
    if digest(binary) != args.expected_sha256 or len(args.expected_sha256) != 64:
        raise RuntimeError('candidate SHA256 mismatch')
    command = subprocess.check_output(['systemctl', 'show', SERVICE, '-p', 'ExecStart', '--value'], text=True)
    if 'path='+str(TARGET)+' ;' not in command:
        raise RuntimeError('unexpected installed service executable')
    if request().get('recording', True):
        raise RuntimeError('recording state is active/unknown; refusing replacement')
    temporary = Path(tempfile.mkdtemp(prefix='tenco-camera-deploy-'))
    protected = [TARGET, DROP_IN, SETTINGS]
    existed = {path: path.exists() for path in protected}
    for index, path in enumerate(protected):
        if existed[path]:
            shutil.copy2(path, temporary / str(index))
    evidence = {'candidateSha256': args.expected_sha256,
                'previousSha256': digest(TARGET), 'deployed': False}
    try:
        subprocess.run(['systemctl', 'stop', SERVICE], check=True)
        install(binary, TARGET)
        install(args.drop_in, DROP_IN)
        subprocess.run(['systemctl', 'daemon-reload'], check=True)
        subprocess.run(['systemctl', 'start', SERVICE], check=True)
        evidence['ready'] = ready()
        config = request('/camera/settings')
        if config.get('applyState') != 'applied' or not config.get('persisted'):
            raise RuntimeError('settings are not applied/persisted')
        recording = config['activeSettings']['recording']
        if (recording['width'], recording['height'], recording['fps']) != (1280, 720, 30):
            raise RuntimeError('unexpected initial recording defaults')
        evidence['configuration'] = config
        evidence['capabilities'] = request('/camera/capabilities')
        photo = request('/camera/photo', 'POST')
        if not photo.get('success'):
            raise RuntimeError('real photo failed: '+str(photo))
        evidence['photo'] = photo
        start = request('/camera/record/start', 'POST')
        if not start.get('success'):
            raise RuntimeError('recording start failed: '+str(start))
        time.sleep(20)
        stop = request('/camera/record/stop', 'POST')
        if not stop.get('success') or stop.get('lastRecordingError'):
            raise RuntimeError('recording failed: '+str(stop))
        video = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-count_frames',
            '-show_streams', '-show_format', '-of', 'json', stop['path']]))
        stream = video['streams'][0]
        fps = int(stream['nb_read_frames'])/float(stream['duration'])
        if (stream['width'],stream['height'],stream['codec_name']) != (1280,720,'h264') or fps < 15:
            raise RuntimeError('saved default media failed >=15 FPS/dimension/codec validation: '+str(video))
        subprocess.run(['ffmpeg','-v','error','-xerror','-i',stop['path'],'-f','null','-'],check=True)
        photo_info = json.loads(subprocess.check_output(['ffprobe','-v','error','-show_streams','-of','json',photo['path']]))['streams'][0]
        native = evidence['capabilities']['native']
        if (photo_info['width'],photo_info['height']) != (native['width'],native['height']):
            raise RuntimeError('default photo did not retain native resolution')
        evidence.update(recording=stop, ffprobe=video, photoDimensions=[photo_info['width'],photo_info['height']],
                        measuredFps=fps, finalStatus=ready())
        pid = subprocess.check_output(['systemctl','show',SERVICE,'-p','MainPID','--value'],text=True).strip()
        evidence['pid'] = int(pid)
        evidence['runningSha256'] = digest(Path('/proc')/pid/'exe')
        if digest(TARGET) != args.expected_sha256 or evidence['runningSha256'] != args.expected_sha256:
            raise RuntimeError('running/installed candidate hash mismatch')
        evidence['deployed'] = True
    except BaseException:
        try:
            subprocess.run(['systemctl', 'stop', SERVICE], check=True)
            for index, path in enumerate(protected):
                if existed[path]:
                    install(temporary / str(index), path)
                elif path.exists():
                    path.unlink()
            subprocess.run(['systemctl','daemon-reload'],check=True)
            subprocess.run(['systemctl','start',SERVICE],check=True)
            evidence['rollbackStatus'] = ready()
        except BaseException:
            print('ROLLBACK INCOMPLETE: temporary recovery files retained at '+str(temporary), flush=True)
            raise
        shutil.rmtree(temporary)
        raise
    else:
        shutil.rmtree(temporary)
        args.evidence.parent.mkdir(parents=True, exist_ok=True)
        args.evidence.write_text(json.dumps(evidence, ensure_ascii=False, indent=2)+'\n')
        print(json.dumps(evidence, ensure_ascii=False, indent=2),flush=True)
        print('Verified latest binary installed. Transaction backups removed.',flush=True)


if __name__ == '__main__':
    main()
