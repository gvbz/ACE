"""Run on Linux/WSL: python3 Unreal/Build/VR/Tests/Test-LinuxUpdate.py"""
import argparse
import importlib.util
import io
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[1] / 'Update-Client.py'
spec = importlib.util.spec_from_file_location('linux_update', SCRIPT)
update = importlib.util.module_from_spec(spec)
spec.loader.exec_module(update)


def put(root, name, data=b'old'):
    path = root / name; path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class LinuxUpdateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ac-linux-update-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'Game folder with spaces'; self.root.mkdir()
        self.exe = put(self.root, update.EXE)
        put(self.root, 'AC-Unreal.sh', b'#!/bin/sh\nprintf old > restarted.txt\n')
        self.stage = self.base / 'stage'; self.stage.mkdir()
        self.backup = self.base / 'backup'; self.backup.mkdir()
        self.archive = self.base / 'update.tar.gz'
        self.elf = bytearray(64); self.elf[:6] = b'\x7fELF\x02\x01'; self.elf[18:20] = b'\x3e\x00'
        self.defaults = [(str(update.EXE), bytes(self.elf)), ('AC-Unreal.sh', b'#!/bin/sh\nprintf new > restarted.txt\n')]

    def package(self, extra=(), defaults=True):
        with tarfile.open(self.archive, 'w:gz') as tar:
            for name, data in (self.defaults if defaults else []) + list(extra):
                if isinstance(name, tarfile.TarInfo):
                    tar.addfile(name)
                else:
                    info = tarfile.TarInfo('AC-Unreal-Linux-v124/' + name)
                    info.size = len(data); info.mode = 0o644
                    tar.addfile(info, io.BytesIO(data))
        return update.digest(self.archive)

    def stage_package(self, extra=(), defaults=True):
        sha = self.package(extra, defaults)
        return update.stage_archive(self.archive, sha, 124, self.root, self.stage)

    def run_update(self, args):
        popen = subprocess.Popen
        def track_child(*positional, **keyword):
            child = popen(*positional, **keyword)
            self.addCleanup(child.wait, timeout=3)
            return child
        with patch.object(update.subprocess, 'Popen', side_effect=track_child):
            return update.run(args)

    def test_install_preserves_saved_dats_and_plugins(self):
        user_files = ('ACUnreal/Saved/Config/settings.ini', 'ACUnreal/Saved/ClientPlugins/Waypoint/locations.json',
                      'DATs/client_portal.dat', 'ACUnreal/Plugins/User/main.lua')
        for name in user_files:
            put(self.root, name, b'user data')
        put(self.root, 'ACUnreal/Content/Paks/old.pak')
        names = self.stage_package([('ACUnreal/Content/Paks/new.pak', b'new pak')])
        update.install_files(self.root, self.stage, names, self.backup)
        self.assertEqual(self.exe.read_bytes(), bytes(self.elf))
        self.assertTrue(self.exe.stat().st_mode & 0o111)
        self.assertFalse((self.root / 'ACUnreal/Content/Paks/old.pak').exists())
        for name in user_files:
            self.assertEqual((self.root / name).read_bytes(), b'user data')

    def test_rollback_on_partial_install_failure(self):
        names = self.stage_package()
        replace = os.replace
        def fail_once(source, target):
            if Path(source) == self.stage / update.EXE:
                raise OSError('simulated install failure')
            return replace(source, target)
        with patch.object(update.os, 'replace', side_effect=fail_once):
            with self.assertRaises(OSError):
                update.install_files(self.root, self.stage, names, self.backup)
        self.assertEqual(self.exe.read_bytes(), b'old')
        self.assertIn(b'printf old', (self.root / 'AC-Unreal.sh').read_bytes())

    def test_rollback_failure_preserves_recovery_copy(self):
        names = self.stage_package()
        replace = os.replace
        def fail(source, target):
            if Path(source) in {self.stage / update.EXE, self.backup / update.EXE}:
                raise OSError('simulated filesystem failure')
            return replace(source, target)
        with patch.object(update.os, 'replace', side_effect=fail):
            with self.assertRaises(update.RollbackError):
                update.install_files(self.root, self.stage, names, self.backup)
        self.assertEqual((self.backup / update.EXE).read_bytes(), b'old')

    def test_bad_checksum_does_not_write_install(self):
        self.package()
        with self.assertRaisesRegex(ValueError, 'checksum'):
            update.stage_archive(self.archive, '0'*64, 124, self.root, self.stage)
        self.assertEqual(self.exe.read_bytes(), b'old')

    def test_unsafe_paths_and_user_data_rejected(self):
        for name in ['../escape', '/absolute', 'ACUnreal/../../escape', 'ACUnreal/Saved/account.json', 'DATs/a.dat',
                     'Engine/client_portal.dat', 'ACUnreal\\evil']:
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    self.stage_package([(name, b'bad')])
        self.assertEqual(self.exe.read_bytes(), b'old')

    def test_archive_links_and_devices_rejected(self):
        for kind in [tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.CHRTYPE]:
            info = tarfile.TarInfo('AC-Unreal-Linux-v124/Engine/escape')
            info.type = kind; info.linkname = '/tmp'
            with self.assertRaises(ValueError):
                self.stage_package([(info, b'')])

    def test_symlink_install_destination_rejected(self):
        (self.root / 'Engine').symlink_to(self.base / 'outside', target_is_directory=True)
        with self.assertRaisesRegex(ValueError, 'symbolic link'):
            self.stage_package([('Engine/anything', b'bad')])

    def test_duplicate_member_rejected(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            self.stage_package([('AC-Unreal.sh', b'duplicate')])

    def test_wrong_release_rejected(self):
        sha = self.package()
        with self.assertRaisesRegex(ValueError, 'release archive root'):
            update.stage_archive(self.archive, sha, 125, self.root, self.stage)

    def test_non_linux_executable_rejected(self):
        self.defaults[0] = (str(update.EXE), b'MZ'+b'bad'*64)
        with self.assertRaisesRegex(ValueError, 'Linux x86_64'):
            self.stage_package()

    def test_recycled_pid_is_never_signalled(self):
        with patch.object(update, 'process_identity', return_value=('different', '2')), patch.object(update.os, 'kill') as kill:
            update.wait_for_client(12345, ('expected', '1'), timeout=0)
            kill.assert_not_called()

    def test_stuck_requesting_process_stops_without_killing_another(self):
        with subprocess.Popen(['/bin/sleep', '60']) as requesting, subprocess.Popen(['/bin/sleep', '60']) as other:
            try:
                identity = update.process_identity(requesting.pid)
                update.wait_for_client(requesting.pid, identity, timeout=0)
                requesting.wait(timeout=2)
                self.assertIsNone(other.poll())
            finally:
                requesting.terminate() if requesting.poll() is None else None
                other.terminate(); other.wait(timeout=2)

    def test_end_to_end_restarts_and_removes_installed_payload(self):
        args = argparse.Namespace(root=self.root, archive=self.archive, sha256=self.package(), release=124,
                                  pid=2147483647, result=self.base / 'result.txt')
        self.run_update(args)
        deadline = time.monotonic()+3
        while not (self.root/'restarted.txt').exists() and time.monotonic()<deadline: time.sleep(.01)
        self.assertEqual((self.root/'restarted.txt').read_text(), 'new')
        self.assertFalse(self.archive.exists())
        self.assertEqual(args.result.read_text(), 'Updated successfully.\n')
        self.assertFalse(list(self.root.glob('.ac-update-*')))

    def test_invalid_update_reopens_previous_client(self):
        args = argparse.Namespace(root=self.root, archive=self.archive, sha256='0'*64, release=124,
                                  pid=2147483647, result=self.base / 'result.txt')
        self.package()
        with self.assertRaises(ValueError): self.run_update(args)
        deadline = time.monotonic()+3
        while not (self.root/'restarted.txt').exists() and time.monotonic()<deadline: time.sleep(.01)
        self.assertEqual((self.root/'restarted.txt').read_text(), 'old')
        self.assertIn('checksum', args.result.read_text())

    def test_payload_cleanup_failure_still_restarts_updated_client(self):
        args = argparse.Namespace(root=self.root, archive=self.archive, sha256=self.package(), release=124,
                                  pid=2147483647, result=self.base / 'result.txt')
        unlink = Path.unlink
        def fail_payload(path, *positional, **keyword):
            if path == self.archive: raise PermissionError('simulated cleanup error')
            return unlink(path, *positional, **keyword)
        with patch.object(Path, 'unlink', fail_payload): self.run_update(args)
        deadline = time.monotonic()+3
        while not (self.root/'restarted.txt').exists() and time.monotonic()<deadline: time.sleep(.01)
        self.assertEqual((self.root/'restarted.txt').read_text(), 'new')
        self.assertEqual(args.result.read_text(), 'Updated successfully.\n')


class LinuxExitGuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='ac-exit-guard-test-')
        cls.base = Path(cls.temp.name)
        source = cls.base/'guard.cpp'
        source.write_text('''#include "ACELinuxExitGuard.h"
#include <atomic>
#include <cstring>
std::atomic<bool> Stopping{false};
bool ShouldExit(){return Stopping.load();}
int main(int argc,char** argv){
 if(!ACELinuxExitGuard::Start(ShouldExit,.25))return 2;
 if(!strcmp(argv[1],"normal")){Stopping=true;usleep(50000);return 0;}
 if(!strcmp(argv[1],"stuck"))Stopping=true;
 usleep(1000000);return 0;
}
''')
        include = SCRIPT.parents[2] / 'Source/ACUnreal'
        cls.exe = cls.base/'guard'
        subprocess.run(['g++','-std=c++17','-pthread','-I'+str(include),str(source),'-o',str(cls.exe)],check=True)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def test_active_or_minimized_client_not_terminated(self):
        self.assertEqual(subprocess.run([str(self.exe),'active'],timeout=3).returncode,0)

    def test_normal_exit_finishes_within_grace(self):
        self.assertEqual(subprocess.run([str(self.exe),'normal'],timeout=3).returncode,0)

    def test_shutdown_hang_terminates_process(self):
        result=subprocess.run([str(self.exe),'stuck'],capture_output=True,timeout=3)
        self.assertEqual(result.returncode,124)
        self.assertIn(b'shutdown exceeded',result.stderr)

    def test_full_stderr_pipe_cannot_block_shutdown(self):
        read_fd, write_fd = os.pipe()
        try:
            os.set_blocking(write_fd, False)
            try:
                while True: os.write(write_fd, b'x'*4096)
            except BlockingIOError: pass
            os.set_blocking(write_fd, True)
            result=subprocess.run([str(self.exe),'stuck'],stderr=write_fd,timeout=3)
            self.assertEqual(result.returncode,124)
        finally:
            os.close(read_fd); os.close(write_fd)


if __name__ == '__main__': unittest.main(verbosity=2)
