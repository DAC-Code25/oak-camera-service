#!/usr/bin/env python3
"""Install a verified candidate, validate real media, or restore the prior service.

Run explicitly as root during an authorized camera maintenance window. Only the
binary, this project's drop-in and its settings file are part of the transaction.
Existing Capture media is never removed. Old copies exist only until validation.
"""
import argparse
import contextlib
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
PROJECT = Path('/home/nvidia/tenco-project')
TARGET = PROJECT / 'install/camera/oak_camera_service'
ROOT = PROJECT / 'oak-camera-service'
DROP_IN = Path('/etc/systemd/system/tenco-oak-camera.service.d/20-native-capture.conf')
PROJECT_DROP_IN = PROJECT / 'systemd/tenco-oak-camera.service.d/20-native-capture.conf'
SETTINGS = ROOT / 'camera-settings.json'
PROC = Path('/proc')


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
            # New services expose a tiny real sensor stream even while RGB
            # preview/photo/recording transport is intentionally idle. Older
            # rollback binaries still use their continuously produced preview.
            health = 'captureFrameId' in state
            frame = state.get('captureFrameId' if health else 'frameId', 0)
            age = state.get('lastCaptureFrameAgeMs' if health else 'lastFrameAgeMs', -1)
            valid = state.get('cameraConnected') and state.get('success') and \
                isinstance(frame, int) and not isinstance(frame, bool) and frame > 0 and \
                isinstance(age, (int, float)) and not isinstance(age, bool) and 0 <= age <= 1000
            if valid and frame > previous:
                healthy += 1
                if healthy >= 3:
                    return state
            elif not valid or frame < previous:
                healthy = 0
            previous = frame
        except OSError:
            healthy = 0
        time.sleep(1)
    raise RuntimeError('camera did not produce continuous fresh frames')


def sync_directory(path):
    if os.name == 'posix':
        descriptor = os.open(path, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(descriptor)
        finally:
            os.close(descriptor)


@contextlib.contextmanager
def deployment_lock(directory):
    """Concurrent deployments must not mistake each other's candidate for a backup."""
    directory.mkdir(parents=True, exist_ok=True)
    descriptor = os.open(directory/'camera-deployment.lock',
                         os.O_CREAT | os.O_RDWR | getattr(os, 'O_NOFOLLOW', 0), 0o600)
    with os.fdopen(descriptor, 'r+b') as stream:
        if os.name == 'posix':
            import fcntl
            try:
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError:
                raise RuntimeError('another camera deployment is active; no service changed') from None
            try:
                yield
            finally:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        else:
            import msvcrt
            if os.fstat(stream.fileno()).st_size == 0:
                stream.write(b'0'); stream.flush()
            stream.seek(0)
            try:
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError:
                raise RuntimeError('another camera deployment is active; no service changed') from None
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)


def file_owner(path):
    state = path.stat()
    return state.st_uid, state.st_gid


def apply_owner(path, owner):
    if os.name == 'posix' and file_owner(path) != owner:
        os.chown(path, *owner)


def service_owner():
    """Resolve the actual runtime account, never inherit sudo/root ownership."""
    if os.name != 'posix':
        return file_owner(PROJECT)
    import pwd
    import grp
    output = subprocess.check_output(['systemctl', 'show', SERVICE, '--property=User,Group'], text=True)
    values = dict(line.split('=', 1) for line in output.splitlines() if '=' in line)
    user, group = values.get('User', ''), values.get('Group', '')
    if not user:
        raise RuntimeError('camera service runtime User must be explicit before installing new files')
    account = pwd.getpwuid(int(user)) if user.isdecimal() else pwd.getpwnam(user)
    group_id = (int(group) if group.isdecimal() else grp.getgrnam(group).gr_gid) if group else account.pw_gid
    return account.pw_uid, group_id


def install(source, target, expected_sha256=None, owner=None):
    # A backup made by root/copy2 does not retain its original uid/gid. Callers
    # restoring a transaction must pass the pre-switch owner explicitly.
    if owner is None:
        owner = file_owner(target if target.exists() else source)
    target.parent.mkdir(parents=True, exist_ok=True)
    pending = target.with_name(target.name+'.pending-deploy')
    if os.path.lexists(pending):
        raise RuntimeError('pending install file already exists; preserve and inspect: '+str(pending))
    created = False
    try:
        with pending.open('xb') as output:
            created = True
            with source.open('rb') as incoming:
                shutil.copyfileobj(incoming, output)
            apply_owner(pending, owner)
            shutil.copystat(source, pending)
            output.flush()
            os.fsync(output.fileno())
        # Validate the bytes actually installed, including a candidate changed
        # after the initial preflight hash. Never start unverified bytes.
        if digest(pending) != (expected_sha256 or digest(source)):
            raise RuntimeError('pending installed file failed SHA256 verification')
        os.replace(pending, target)
        sync_directory(target.parent)
    except BaseException:
        if created and pending.exists():
            pending.unlink()
        raise


def contained(path, parent):
    resolved = path.resolve()
    if resolved == parent.resolve() or parent.resolve() not in resolved.parents:
        raise RuntimeError('path must stay inside managed project: '+str(path))
    return resolved


def check_service_wrapper(command):
    # The logger is systemd's MainPID; the ELF is its owned child. Reject direct
    # binaries or another workspace rather than bypassing the runtime logger.
    wrapper = str(PROJECT / 'src/deployment/project.sh')
    return any('path='+shell+' ;' in command and
               'argv[]='+shell+' '+wrapper+' run camera isolated ;' in command
               for shell in ('/bin/bash', '/usr/bin/bash'))


def running_binary(expected):
    group = subprocess.check_output(['systemctl','show',SERVICE,'-p','ControlGroup','--value'],text=True).strip()
    if not group.startswith('/') or group == '/' or '..' in group.split('/'):
        raise RuntimeError('invalid camera service control group')
    owned = []
    for process in PROC.iterdir():
        if not process.name.isdigit():
            continue
        try:
            if (process/'exe').resolve(strict=True) != TARGET.resolve(strict=True):
                continue
            groups = [line.split(':',2)[2] for line in (process/'cgroup').read_text().splitlines()]
            if any(value == group or value.startswith(group+'/') for value in groups):
                owned.append(process)
        except (OSError, RuntimeError, IndexError):
            continue
    if len(owned) != 1:
        raise RuntimeError('expected one owned camera executable, found '+str(len(owned)))
    actual = digest(owned[0]/'exe')
    if digest(TARGET) != expected or actual != expected:
        raise RuntimeError('running/installed candidate hash mismatch')
    return int(owned[0].name), actual


def validate_media(evidence):
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('expected_sha256')
    parser.add_argument('--drop-in', type=Path)
    parser.add_argument('--readiness-only', action='store_true', help='verify fresh frames without creating media')
    parser.add_argument('--evidence', type=Path, required=True)
    args = parser.parse_args()
    if os.geteuid() != 0:
        raise RuntimeError('root required')
    if not (PROJECT/'.tenco-project.json').is_file():
        raise RuntimeError('managed project marker is missing')
    binary = contained(args.binary, PROJECT)
    transaction_root = contained(PROJECT/'tmp', PROJECT)
    evidence_path = contained(args.evidence, transaction_root)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError('candidate is missing/not executable')
    if digest(binary) != args.expected_sha256 or len(args.expected_sha256) != 64:
        raise RuntimeError('candidate SHA256 mismatch')
    contained(TARGET, PROJECT)
    contained(SETTINGS, PROJECT)
    with deployment_lock(transaction_root):
        deploy(args, binary, transaction_root, evidence_path)


def deploy(args, binary, transaction_root, evidence_path):
    drop_source = contained(args.drop_in, PROJECT) if args.drop_in else None
    if drop_source:
        contained(PROJECT_DROP_IN, PROJECT)
        # Never take ownership of an unknown site override, including a regular
        # /etc file. Only our known project symlinks are eligible for replacement.
        known = {PROJECT_DROP_IN.resolve(), (ROOT/'systemd/20-native-capture.conf').resolve()}
        if os.path.lexists(DROP_IN) and (not DROP_IN.is_symlink() or DROP_IN.resolve() not in known):
            raise RuntimeError('unknown camera drop-in; preserve and inspect before deployment')
        if not drop_source.is_file():
            raise RuntimeError('project drop-in source is missing')
        starts = [line.strip() for line in drop_source.read_text().splitlines() if line.strip().startswith('ExecStart=')]
        if starts != ['ExecStart=', 'ExecStart=/bin/bash '+str(PROJECT/'src/deployment/project.sh')+' run camera isolated']:
            raise RuntimeError('drop-in must preserve the project camera runtime wrapper')
    command = subprocess.check_output(['systemctl', 'show', SERVICE, '-p', 'ExecStart', '--value'], text=True)
    if not check_service_wrapper(command):
        raise RuntimeError('unexpected installed service wrapper')
    if request().get('recording', True):
        raise RuntimeError('recording state is active/unknown; refusing replacement')
    transaction_root.mkdir(parents=True, exist_ok=True)
    protected = [TARGET, SETTINGS] + ([PROJECT_DROP_IN] if drop_source else [])
    existed = {path: path.exists() for path in protected}
    owners = {path: file_owner(path) for path in protected if existed[path]}
    runtime_owner = service_owner()
    temporary = Path(tempfile.mkdtemp(prefix='camera-deploy-', dir=transaction_root))
    previous_link = os.readlink(DROP_IN) if drop_source and DROP_IN.is_symlink() else None
    for index, path in enumerate(protected):
        if existed[path]:
            shutil.copy2(path, temporary / str(index))
    # Retained recovery files include the ownership lost by shutil.copy2.
    (temporary/'original-metadata.json').write_text(json.dumps([
        {'path': str(path), 'existed': existed[path], 'owner': list(owners[path]) if existed[path] else None}
        for path in protected], indent=2)+'\n')
    evidence = {'candidateSha256': args.expected_sha256,
                'previousSha256': digest(TARGET), 'deployed': False}
    try:
        subprocess.run(['systemctl', 'stop', SERVICE], check=True)
        install(binary, TARGET, args.expected_sha256, owner=owners.get(TARGET, runtime_owner))
        if drop_source:
            install(drop_source, PROJECT_DROP_IN, owner=owners.get(PROJECT_DROP_IN, runtime_owner))
            DROP_IN.parent.mkdir(parents=True, exist_ok=True)
            pending_link = DROP_IN.with_name(DROP_IN.name+'.pending-deploy')
            if os.path.lexists(pending_link):
                raise RuntimeError('pending drop-in link already exists; preserve and inspect')
            pending_link.symlink_to(PROJECT_DROP_IN)
            os.replace(pending_link, DROP_IN)
        subprocess.run(['systemctl', 'daemon-reload'], check=True)
        subprocess.run(['systemctl', 'start', SERVICE], check=True)
        evidence['ready'] = ready()
        evidence['pid'], evidence['runningSha256'] = running_binary(args.expected_sha256)
        if not args.readiness_only:
            validate_media(evidence)
        evidence['pid'], evidence['runningSha256'] = running_binary(args.expected_sha256)
        evidence['deployed'] = True
    except BaseException:
        try:
            subprocess.run(['systemctl', 'stop', SERVICE], check=True)
            for index, path in enumerate(protected):
                if existed[path]:
                    install(temporary / str(index), path, owner=owners[path])
                elif path.exists():
                    path.unlink()
            if drop_source:
                if DROP_IN.is_symlink():
                    DROP_IN.unlink()
                if previous_link:
                    DROP_IN.symlink_to(previous_link)
            subprocess.run(['systemctl','daemon-reload'],check=True)
            subprocess.run(['systemctl','start',SERVICE],check=True)
            evidence['rollbackStatus'] = ready()
            running_binary(evidence['previousSha256'])
        except BaseException:
            print('ROLLBACK INCOMPLETE: temporary recovery files retained at '+str(temporary), flush=True)
            raise
        shutil.rmtree(temporary)
        raise
    else:
        shutil.rmtree(temporary)
        evidence_path.parent.mkdir(parents=True, exist_ok=True)
        evidence_path.write_text(json.dumps(evidence, ensure_ascii=False, indent=2)+'\n')
        print(json.dumps(evidence, ensure_ascii=False, indent=2),flush=True)
        print('Verified latest binary installed. Transaction backups removed.',flush=True)


if __name__ == '__main__':
    main()
