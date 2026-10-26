#!/usr/bin/env python3
"""Binary-format and sync lifecycle contracts; no guest execution or real app registration."""
import importlib.machinery
import importlib.util
from pathlib import Path
import plistlib
import struct
import tempfile
import unittest
from unittest.mock import patch
import uuid

loader = importlib.machinery.SourceFileLoader('raven_apps', str(Path(__file__).resolve().parents[1]/'raven-apps'))
spec = importlib.util.spec_from_loader(loader.name, loader)
apps = importlib.util.module_from_spec(spec)
loader.exec_module(apps)


def link(target='', relative='', arguments='', working='', icon='', idlist=b''):
    header = bytearray(76)
    struct.pack_into('<I', header, 0, 76)
    header[4:20] = uuid.UUID('00021401-0000-0000-c000-000000000046').bytes_le
    flags = 128
    body = b''
    if idlist:
        flags |= 1
        body += struct.pack('<H', len(idlist)) + idlist
    if target:
        flags |= 2
        path = target.encode('utf-16le')+b'\0\0'
        info = bytearray(36)
        struct.pack_into('<9I', info, 0, 36+len(path)+2, 36, 1, 0, 0, 0, 0, 36, 36+len(path))
        body += info + path + b'\0\0'
    for flag, value in [(8, relative), (16, working), (32, arguments), (64, icon)]:
        if value:
            flags |= flag
            data = value.encode('utf-16le')
            body += struct.pack('<H', len(data)//2) + data
    struct.pack_into('<I', header, 20, flags)
    return bytes(header)+body+b'\0\0\0\0'


class AppsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.prefix = self.root/'prefix'
        self.programs = self.prefix/'drive_c/ProgramData/Microsoft/Windows/Start Menu/Programs'
        self.programs.mkdir(parents=True)
        self.target = self.prefix/'drive_c/Program Files/Example/Example.exe'
        self.target.parent.mkdir(parents=True)
        self.target.write_bytes(b'test fixture')

    def test_unicode_and_raw_windows_arguments(self):
        path = self.programs/'Example.lnk'
        arguments = r'--profile "C:\Users\some one" --literal="a\\\"b"'
        path.write_bytes(link(target=r'C:\Program Files\Example\Example.exe', arguments=arguments,
                              working=r'C:\Program Files\Example', icon=r'C:\é.ico'))
        data = apps.parse_link(path)
        self.assertEqual(data['arguments'], arguments)
        self.assertEqual(data['icon'], 'C:\\é.ico')
        self.assertEqual(apps.host_path(self.prefix, data['target'].lower()), self.target.resolve())

    def test_relative_and_idlist(self):
        path = self.programs/'relative.lnk'
        path.write_bytes(link(relative=r'..\..\..\..\..\Program Files\Example\Example.exe'))
        self.assertTrue(apps.parse_link(path)['relative'].endswith('Example.exe'))
        drive = b'\x2fC:\\\0'
        file_item = bytearray(14)
        file_item[2] = 0x32
        file_item += b'EXAMPLE.EXE\0'
        struct.pack_into('<H', file_item, 0, len(file_item))
        items = struct.pack('<H', len(drive)+2)+drive+bytes(file_item)+b'\0\0'
        path.write_bytes(link(idlist=items))
        self.assertEqual(apps.parse_link(path)['target'], 'C:\\EXAMPLE.EXE')

    def test_truncated_data_rejected(self):
        path = self.programs/'broken.lnk'
        for data in (b'', link(target=r'C:\Example.exe')[:81], link(arguments='foo')[:-5]):
            path.write_bytes(data)
            with self.assertRaises((ValueError, struct.error)):
                apps.parse_link(path)

    def test_unknown_paths_rejected(self):
        for path in (r'\\server\share\program.exe', r'D:\program.exe', r'%UNKNOWN%\program.exe'):
            with self.assertRaises(ValueError):
                apps.host_path(self.prefix, path)

    def test_sync_idempotent_deduplicates_and_removes_only_owned(self):
        output = self.root/'Applications'
        output.mkdir()
        unrelated = output/'Unrelated.app'
        unrelated.mkdir()
        shortcut = self.programs/'Example.lnk'
        shortcut.write_bytes(link(target=r'C:\Program Files\Example\Example.exe'))
        duplicate = self.prefix/'drive_c/Users/Public/Desktop/Example.lnk'
        duplicate.parent.mkdir(parents=True)
        duplicate.write_bytes(shortcut.read_bytes())
        (self.programs/'Uninstall Example.lnk').write_bytes(shortcut.read_bytes())
        with patch.object(apps, 'APPS', output), patch.object(apps, 'raven_path', return_value=Path('/bin/true')), \
             patch.object(apps, 'make_icon', return_value=False), patch.object(apps.subprocess, 'run') as run:
            apps.sync(self.prefix)
            generated = output/'Example.app'
            self.assertTrue(generated.is_dir())
            info = plistlib.loads((generated/'Contents/Info.plist').read_bytes())
            self.assertEqual(info['CFBundleName'], 'Example')
            modified = (generated/'Contents/Info.plist').stat().st_mtime_ns
            apps.sync(self.prefix)
            self.assertEqual(modified, (generated/'Contents/Info.plist').stat().st_mtime_ns)
            self.assertEqual(len(list(output.glob('*.app'))), 2)
            self.assertEqual(run.call_count, 1)
            shortcut.unlink()
            duplicate.unlink()
            apps.sync(self.prefix)
            self.assertFalse(generated.exists())
            self.assertTrue(unrelated.exists())

    def test_launch_preserves_command_line_directory_and_saved_environment(self):
        output = self.root/'Applications'
        arguments = r'--profile "C:\a b" --literal=a\\b'
        with patch.object(apps, 'APPS', output), patch.object(apps, 'raven_path', return_value=Path('/bin/true')), \
             patch.object(apps, 'make_icon', return_value=False), patch.object(apps.subprocess, 'run'), \
             patch.dict(apps.os.environ, {'RAVEN_LOG_DIR': str(self.root/'logs'), 'RAVEN_GUEST_EXAMPLE': 'saved'}):
            app = apps.bundle(self.prefix, 'Example', self.target, arguments, str(self.target.parent))
        with patch.object(apps.subprocess, 'Popen') as supervisor, \
             patch.object(apps.os, 'open', return_value=42), patch.object(apps.os, 'dup2'), \
             patch.object(apps.os, 'close'), patch.object(apps.os, 'chdir'), \
             patch.object(apps.os, 'execve') as execute:
            apps.run(app)
        binary, command, environment = execute.call_args.args
        self.assertEqual(binary, '/bin/true')
        self.assertIn('--command-line="C:\\Program Files\\Example\\Example.exe" '+arguments, command)
        self.assertIn('--directory=C:\\Program Files\\Example', command)
        self.assertEqual(environment['RAVEN_GUEST_EXAMPLE'], 'saved')
        self.assertEqual(supervisor.call_args.kwargs['env']['RAVEN_APPS_DIR'], str(output))
        self.assertNotIn('RAVEN_APP_NAME', supervisor.call_args.kwargs['env'])

    def test_malformed_shortcut_preserves_existing_app(self):
        output = self.root/'Applications'
        shortcut = self.programs/'Example.lnk'
        shortcut.write_bytes(link(target=r'C:\Program Files\Example\Example.exe'))
        with patch.object(apps, 'APPS', output), patch.object(apps, 'raven_path', return_value=Path('/bin/true')), \
             patch.object(apps, 'make_icon', return_value=False), patch.object(apps.subprocess, 'run'):
            apps.sync(self.prefix)
            shortcut.write_bytes(b'partial installer write')
            apps.sync(self.prefix)
            self.assertTrue((output/'Example.app').is_dir())


if __name__ == '__main__':
    unittest.main()
