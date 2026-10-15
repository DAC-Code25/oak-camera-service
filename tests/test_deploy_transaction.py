"""Verify transaction rollback boundaries without systemd/USB/root access."""
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('deployment', Path(__file__).parents[1]/'scripts/deploy_media_settings.py')
deployment = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deployment)


class DeployTransactionTest(unittest.TestCase):
    def exercise(self, low_fps=False, active_recording=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            candidate, target = root/'candidate', root/'installed'
            candidate.write_bytes(b'new tested binary'); target.write_bytes(b'old verified binary')
            drop, new_drop, settings = root/'drop', root/'new-drop', root/'settings'
            drop.write_bytes(b'old drop'); new_drop.write_bytes(b'new drop'); settings.write_bytes(b'old settings')
            business = root/'Capture/business.mp4'; business.parent.mkdir(); business.write_bytes(b'preserved')
            evidence = root/'evidence.json'
            expected = deployment.digest(candidate)
            original_digest = deployment.digest
            commands = []

            def execute(command, **kwargs):
                commands.append(command)
                if command == ['systemctl','start',deployment.SERVICE] and target.read_bytes()==candidate.read_bytes():
                    settings.write_bytes(b'new settings')

            def output(command, **kwargs):
                if command[:2] == ['systemctl','show']:
                    return '42\n' if 'MainPID' in command else 'path='+str(target)+' ;'
                if command[0] == 'ffprobe':
                    if 'test-photo.jpg' in command:
                        return json.dumps({'streams':[{'width':4056,'height':3040}]}).encode()
                    return json.dumps({'streams':[{'width':1280,'height':720,'codec_name':'h264',
                        'nb_read_frames':'200' if low_fps else '540','duration':'20'}]}).encode()
                raise AssertionError(command)

            def request(path='/camera/status', method='GET'):
                if path == '/camera/settings':
                    return {'applyState':'applied','persisted':True,'activeSettings':{'recording':{'width':1280,'height':720,'fps':30}}}
                if path == '/camera/capabilities':
                    return {'native':{'width':4056,'height':3040}}
                if path == '/camera/photo': return {'success':True,'path':'test-photo.jpg'}
                if path.startswith('/camera/record/'): return {'success':True,'path':'test-video.mp4'}
                return {'recording':active_recording}

            create_temp = tempfile.mkdtemp
            with patch.multiple(deployment,TARGET=target,DROP_IN=drop,SETTINGS=settings,ROOT=root), \
                 patch.object(deployment.os,'geteuid',return_value=0,create=True), \
                 patch.object(deployment.subprocess,'run',side_effect=execute), \
                 patch.object(deployment.subprocess,'check_output',side_effect=output), \
                 patch.object(deployment,'request',side_effect=request), \
                 patch.object(deployment,'ready',return_value={'cameraConnected':True}), \
                 patch.object(deployment.time,'sleep'), \
                 patch.object(deployment,'digest',side_effect=lambda p: expected if str(p).replace('\\','/').startswith('/proc/') else original_digest(p)), \
                 patch.object(deployment.tempfile,'mkdtemp',side_effect=lambda **kw: create_temp(dir=root)), \
                 patch.object(sys,'argv',['deploy',str(candidate),expected,'--drop-in',str(new_drop),'--evidence',str(evidence)]):
                if low_fps or active_recording:
                    with self.assertRaises(RuntimeError): deployment.main()
                    self.assertEqual(target.read_bytes(),b'old verified binary')
                    self.assertEqual(drop.read_bytes(),b'old drop')
                    self.assertEqual(settings.read_bytes(),b'old settings')
                    self.assertFalse(evidence.exists())
                else:
                    deployment.main()
                    self.assertEqual(target.read_bytes(),candidate.read_bytes())
                    self.assertTrue(json.loads(evidence.read_text())['deployed'])
            self.assertEqual(business.read_bytes(),b'preserved')
            self.assertEqual(sorted(p.name for p in root.iterdir() if p.is_dir()),['Capture'])
            if active_recording: self.assertFalse(commands)
            if low_fps: self.assertEqual(commands.count(['systemctl','start',deployment.SERVICE]),2)

    def test_success_removes_transaction_backup(self): self.exercise()
    def test_fps_failure_restores_binary_dropin_and_settings(self): self.exercise(low_fps=True)
    def test_active_recording_prevents_mutation(self): self.exercise(active_recording=True)


if __name__ == '__main__': unittest.main()
