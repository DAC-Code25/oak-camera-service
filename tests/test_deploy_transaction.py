"""Verify transaction rollback boundaries without systemd/USB/root access."""
import importlib.util
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch, Mock

spec = importlib.util.spec_from_file_location('deployment', Path(__file__).parents[1]/'scripts/deploy_media_settings.py')
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)


class DeployTransactionTest(unittest.TestCase):
    def test_ready_uses_real_health_frames_when_preview_is_idle(self):
        states = [dict(cameraConnected=True, success=True, frameId=1, lastFrameAgeMs=9000,
                       captureFrameId=value, lastCaptureFrameAgeMs=100) for value in (10, 10, 11, 12)]
        with patch.object(deployment, 'request', side_effect=states), patch.object(deployment.time, 'sleep'):
            self.assertEqual(deployment.ready()['captureFrameId'], 12)

    def test_ready_discards_stale_or_backward_health_progress(self):
        states = [dict(cameraConnected=True, success=True, captureFrameId=value, lastCaptureFrameAgeMs=age)
                  for value, age in ((10, 1), (11, 5000), (12, 1), (2, 1), (3, 1), (4, 1), (5, 1))]
        with patch.object(deployment, 'request', side_effect=states), patch.object(deployment.time, 'sleep'):
            self.assertEqual(deployment.ready()['captureFrameId'], 5)

    def test_install_readback_mismatch_preserves_previous_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root/'candidate', root/'installed'
            source.write_bytes(b'candidate changed after preflight')
            target.write_bytes(b'previous binary')
            with self.assertRaisesRegex(RuntimeError, 'SHA256'):
                deployment.install(source, target, '0'*64)
            self.assertEqual(target.read_bytes(), b'previous binary')
            self.assertFalse((root/'installed.pending-deploy').exists())

    def test_install_flush_failure_does_not_replace_previous_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root/'candidate', root/'installed'
            source.write_bytes(b'candidate'); target.write_bytes(b'previous binary')
            with patch.object(deployment.os, 'fsync', side_effect=OSError('disk failed')):
                with self.assertRaises(OSError):
                    deployment.install(source, target)
            self.assertEqual(target.read_bytes(), b'previous binary')
            self.assertFalse((root/'installed.pending-deploy').exists())

    def test_install_preserves_target_owner_before_flush_and_atomic_replace(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, target = root/'candidate', root/'settings'
            source.write_bytes(b'new settings'); target.write_bytes(b'old settings')
            events = []
            original_replace = deployment.os.replace
            def replace(pending, destination):
                events.append('replace'); original_replace(pending, destination)
            def ownership(pending, owner):
                self.assertEqual(owner, (1000, 1001))
                self.assertEqual(target.read_bytes(), b'old settings')
                self.assertTrue(pending.exists())
                events.append('owner')
            with patch.object(deployment, 'file_owner', return_value=(1000, 1001)), \
                 patch.object(deployment, 'apply_owner', side_effect=ownership), \
                 patch.object(deployment.os, 'fsync', side_effect=lambda _: events.append('fsync')), \
                 patch.object(deployment.os, 'replace', side_effect=replace):
                deployment.install(source, target)
            self.assertEqual(target.read_bytes(), b'new settings')
            self.assertLess(events.index('owner'), events.index('fsync'))
            self.assertLess(events.index('fsync'), events.index('replace'))

    def test_owner_failure_never_replaces_existing_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source, target = root/'backup', root/'settings'
            source.write_bytes(b'original settings'); target.write_bytes(b'candidate settings')
            with patch.object(deployment, 'apply_owner', side_effect=PermissionError('chown denied')):
                with self.assertRaises(PermissionError):
                    deployment.install(source, target, owner=(1000, 1001))
            self.assertEqual(target.read_bytes(), b'candidate settings')
            self.assertFalse((root/'settings.pending-deploy').exists())

    def test_posix_chown_and_first_install_use_explicit_service_owner(self):
        chown = Mock()
        with patch.object(deployment, 'os', SimpleNamespace(name='posix', chown=chown)), \
             patch.object(deployment, 'file_owner', return_value=(0, 0)):
            deployment.apply_owner(Path('pending'), (1000, 1001))
        chown.assert_called_once_with(Path('pending'), 1000, 1001)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source, target = root/'source', root/'new-settings'
            source.write_bytes(b'settings')
            with patch.object(deployment, 'apply_owner') as ownership:
                deployment.install(source, target, owner=(1000, 1001))
            ownership.assert_called_once_with(root/'new-settings.pending-deploy', (1000, 1001))

    @unittest.skipUnless(os.name == 'posix' and getattr(os, 'geteuid', lambda: -1)() == 0,
                         'real foreign uid/gid restoration requires a Linux root test')
    def test_native_root_owned_backup_restores_private_runtime_user_file(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); source, target, backup = root/'candidate', root/'settings', root/'backup'
            source.write_bytes(b'new settings'); source.chmod(0o600)
            target.write_bytes(b'old settings'); target.chmod(0o600)
            original_owner = (65534, 65534)
            os.chown(target, *original_owner)
            deployment.shutil.copy2(target, backup)
            self.assertEqual(deployment.file_owner(backup)[0], 0, 'test must reproduce root-owned copy2 backup')
            deployment.install(source, target)
            self.assertEqual(deployment.file_owner(target), original_owner)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            # Simulate a candidate that replaced ownership, then restore from
            # root's backup using the recorded original uid/gid.
            os.chown(target, 0, 0)
            deployment.install(backup, target, owner=original_owner)
            self.assertEqual(target.read_bytes(), b'old settings')
            self.assertEqual(deployment.file_owner(target), original_owner)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            deployment.install(source, root/'first-settings', owner=original_owner)
            self.assertEqual(deployment.file_owner(root/'first-settings'), original_owner)

    def test_concurrent_deployment_is_rejected_and_lock_is_released(self):
        with tempfile.TemporaryDirectory() as directory:
            with deployment.deployment_lock(Path(directory)):
                with self.assertRaisesRegex(RuntimeError, 'another camera deployment'):
                    with deployment.deployment_lock(Path(directory)):
                        self.fail('concurrent deployment acquired the lock')
            with deployment.deployment_lock(Path(directory)):
                pass

    def exercise(self, low_fps=False, active_recording=False, unknown_drop=False, readiness_only=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root/'.tenco-project.json').write_text('{}')
            candidate, target = root/'candidate', root/'installed'
            candidate.write_bytes(b'new tested binary'); target.write_bytes(b'old verified binary')
            candidate.chmod(0o755)
            project_drop, new_drop, settings = root/'systemd/camera.conf', root/'new-drop', root/'settings'
            project_drop.parent.mkdir(); project_drop.write_bytes(b'old drop')
            drop = root/'etc/systemd/camera.conf'; drop.parent.mkdir(parents=True)
            if unknown_drop:
                drop.write_bytes(b'unknown site override')
            else:
                drop.symlink_to(project_drop)
            new_drop.write_text('[Service]\nExecStart=\nExecStart=/bin/bash '+str(root/'src/deployment/project.sh')+' run camera isolated\n')
            settings.write_bytes(b'old settings')
            business = root/'Capture/business.mp4'; business.parent.mkdir(); business.write_bytes(b'preserved')
            evidence = root/'tmp/evidence.json'
            expected = deployment.digest(candidate)
            original_digest = deployment.digest
            commands = []
            requests = []

            def execute(command, **kwargs):
                commands.append(command)
                if command == ['systemctl','start',deployment.SERVICE] and target.read_bytes()==candidate.read_bytes():
                    settings.write_bytes(b'new settings')

            def output(command, **kwargs):
                if command[:2] == ['systemctl','show']:
                    if '--property=User,Group' in command:
                        return 'User='+str(os.getuid())+'\nGroup='+str(os.getgid())+'\n'
                    return 'path=/bin/bash ; argv[]=/bin/bash '+str(root/'src/deployment/project.sh')+' run camera isolated ;'
                if command[0] == 'ffprobe':
                    if 'test-photo.jpg' in command:
                        return json.dumps({'streams':[{'width':4056,'height':3040}]}).encode()
                    return json.dumps({'streams':[{'width':1280,'height':720,'codec_name':'h264',
                        'nb_read_frames':'200' if low_fps else '540','duration':'20'}]}).encode()
                raise AssertionError(command)

            def request(path='/camera/status', method='GET'):
                requests.append(path)
                if path == '/camera/settings':
                    return {'applyState':'applied','persisted':True,'activeSettings':{'recording':{'width':1280,'height':720,'fps':30}}}
                if path == '/camera/capabilities':
                    return {'native':{'width':4056,'height':3040}}
                if path == '/camera/photo': return {'success':True,'path':'test-photo.jpg'}
                if path.startswith('/camera/record/'): return {'success':True,'path':'test-video.mp4'}
                return {'recording':active_recording}

            with patch.multiple(deployment,PROJECT=root,TARGET=target,DROP_IN=drop,PROJECT_DROP_IN=project_drop,SETTINGS=settings,ROOT=root), \
                 patch.object(deployment.os,'geteuid',return_value=0,create=True), \
                 patch.object(deployment.subprocess,'run',side_effect=execute), \
                 patch.object(deployment.subprocess,'check_output',side_effect=output), \
                 patch.object(deployment,'request',side_effect=request), \
                 patch.object(deployment,'ready',return_value={'cameraConnected':True}), \
                 patch.object(deployment,'install',wraps=deployment.install) as installed, \
                 patch.object(deployment.time,'sleep'), \
                 patch.object(deployment,'running_binary',side_effect=lambda wanted:(42,original_digest(target)) if wanted==original_digest(target) else (_ for _ in ()).throw(RuntimeError('hash mismatch'))), \
                 patch.object(sys,'argv',['deploy',str(candidate),expected,'--drop-in',str(new_drop),'--evidence',str(evidence)]+(['--readiness-only'] if readiness_only else [])):
                if low_fps or active_recording or unknown_drop:
                    with self.assertRaises(RuntimeError): deployment.main()
                    self.assertEqual(target.read_bytes(),b'old verified binary')
                    self.assertEqual(drop.read_bytes(),b'unknown site override' if unknown_drop else b'old drop')
                    self.assertEqual(settings.read_bytes(),b'old settings')
                    if low_fps:
                        restored_settings = [call for call in installed.call_args_list if call.args[1] == settings]
                        self.assertEqual(len(restored_settings), 1)
                        self.assertEqual(restored_settings[0].kwargs['owner'], deployment.file_owner(settings))
                    self.assertFalse(evidence.exists())
                else:
                    deployment.main()
                    self.assertEqual(target.read_bytes(),candidate.read_bytes())
                    self.assertTrue(json.loads(evidence.read_text())['deployed'])
                    self.assertTrue(drop.is_symlink())
                    self.assertEqual(drop.resolve(),project_drop.resolve())
            self.assertEqual(business.read_bytes(),b'preserved')
            self.assertEqual(list((root/'tmp').glob('camera-deploy-*')),[])
            if active_recording or unknown_drop: self.assertFalse(commands)
            if readiness_only:
                self.assertFalse(any(path in ('/camera/photo','/camera/record/start','/camera/record/stop') for path in requests))
            if low_fps: self.assertEqual(commands.count(['systemctl','start',deployment.SERVICE]),2)

    def test_success_removes_transaction_backup(self): self.exercise()
    def test_fps_failure_restores_binary_dropin_and_settings(self): self.exercise(low_fps=True)
    def test_active_recording_prevents_mutation(self): self.exercise(active_recording=True)
    def test_unknown_dropin_is_preserved_before_mutation(self): self.exercise(unknown_drop=True)
    def test_readiness_only_uses_shared_transaction_without_media(self): self.exercise(readiness_only=True)

    def test_wrapper_and_owned_child_binary_are_checked(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); target=root/'camera'; target.write_bytes(b'verified')
            proc=root/'proc'; proc.mkdir()
            # MainPID is a Python logger; only one child in the owned cgroup is the camera.
            for pid,executable,group in [('40',root/'python','/system.slice/camera.service'),
                                         ('41',target,'/system.slice/unrelated.service'),
                                         ('42',target,'/system.slice/camera.service')]:
                process=proc/pid; process.mkdir()
                if not executable.exists(): executable.write_bytes(b'logger')
                (process/'exe').symlink_to(executable)
                (process/'cgroup').write_text('0::'+group+'\n')
            with patch.multiple(deployment,PROJECT=root,TARGET=target,PROC=proc), \
                 patch.object(deployment.subprocess,'check_output',return_value='/system.slice/camera.service\n'):
                wrapper='path=/bin/bash ; argv[]=/bin/bash '+str(root/'src/deployment/project.sh')+' run camera isolated ;'
                self.assertTrue(deployment.check_service_wrapper(wrapper))
                self.assertFalse(deployment.check_service_wrapper('path='+str(target)+' ;'))
                self.assertEqual(deployment.running_binary(deployment.digest(target)),(42,deployment.digest(target)))
                with self.assertRaises(RuntimeError): deployment.running_binary('0'*64)
                (proc/'42/cgroup').write_text('0::/system.slice/unrelated.service\n')
                with self.assertRaises(RuntimeError): deployment.running_binary(deployment.digest(target))


if __name__ == '__main__': unittest.main()
