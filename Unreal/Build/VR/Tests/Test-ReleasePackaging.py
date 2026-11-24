"""Small isolated fixtures for release assembly/staging; no real builds required.

Run: python Unreal/Build/VR/Tests/Test-ReleasePackaging.py
"""
import argparse
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
import zipfile

sys.dont_write_bytecode = True


def load_script(name):
    path = Path(__file__).resolve().parents[1] / f'{name}.py'
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


build = load_script('Build-Release')
stage = load_script('Stage-WebsiteRelease')


def put(root, name, value=b'fixture'):
    path = root / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value.encode('utf-8') if isinstance(value, str) else value)
    return path


def elf(version, *, encoding='utf-16-le', machine=62):
    header = bytearray(64)
    header[:7] = b'\x7fELF\x02\x01\x01'
    header[16:18] = (3).to_bytes(2, 'little')
    header[18:20] = machine.to_bytes(2, 'little')
    return bytes(header) + (version + '\0').encode(encoding)


class ReleasePackagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ac-release-tests-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / 'repo'
        put(self.root, 'Unreal/Build/VR/Update-Client.py', '# Linux update helper fixture')
        self.site = Path(self.temp.name) / 'site'
        self.linux = self.root / 'Unreal/Saved/LinuxArchive/Linux'

    def fixture(self, version=88, with_linux=True, revision=6):
        display = f'2026.09.30.{version}'
        self.args = argparse.Namespace(quest_version=version, windows_version=display,
                                       linux_version=display if with_linux else None, installer_revision=revision)
        put(self.root, 'Unreal/Plugins/ACEClient/ClientMods/api.json',
            json.dumps({'lua_api': 2, 'supported_lua_apis': [1, 2]}))
        put(self.root, 'Unreal/Plugins/ACEClient/Source/ACEClient/Public/ACEClientBuild.h',
            f'inline constexpr int32 ReleaseNumber = {version};\nconst auto Version = "{display}";')
        apk = put(self.root, 'Quest/Packaged/Android_ASTC/ACUnreal-arm64.apk', b'fixture APK')
        manifest = {'versionCode': version, 'installerRevision': revision, 'version': display,
                    'displayName': 'AC:VR', 'apkSha256': build.sha(apk)}
        quest_zip = put(self.root, f'Quest/Share/AC-VR-Quest-v{version}-installer-r{revision}.zip')
        with zipfile.ZipFile(quest_zip, 'w') as archive:
            for name in ('Install-Quest.ps1', 'Install-Quest.cmd', 'Update-Quest.cmd', 'Repair-Quest.cmd',
                         'Quest-DataTransfer.ps1', 'Quest-ProfileMigration.ps1', 'README.txt',
                         'RELEASE-NOTES.md', 'LICENSE', 'START-HERE.html', 'AC-Icon.png'):
                archive.writestr(name, b'fixture')
            archive.writestr('manifest.json', json.dumps(manifest))
            archive.writestr('AC-VR-arm64.apk', apk.read_bytes())
            if revision >= 11:
                archive.writestr('Install-AC2-Sky.cmd', b'sky launcher')
            if version >= 154:
                archive.writestr('Diagnose-Quest.cmd', b'fixture diagnostic launcher')
                archive.writestr('Diagnose-Quest.ps1', b'fixture diagnostic collector')
        if version >= 154:
            put(self.root, 'Quest/Sharing/INSTALLER-UPDATE.txt', b'fixture installer update instructions')
            with zipfile.ZipFile(quest_zip) as archive, zipfile.ZipFile(
                    quest_zip.with_name(quest_zip.stem + '-update-only.zip'), 'w') as update:
                for name in ('Install-Quest.ps1', 'Install-Quest.cmd', 'Update-Quest.cmd', 'Repair-Quest.cmd',
                             'Diagnose-Quest.cmd', 'Diagnose-Quest.ps1', 'Quest-DataTransfer.ps1',
                             'Quest-ProfileMigration.ps1', 'README.txt'):
                    update.writestr(name, archive.read(name))
                if revision >= 11:
                    update.writestr('Install-AC2-Sky.cmd', archive.read('Install-AC2-Sky.cmd'))
                update.writestr('INSTALLER-UPDATE.txt', b'fixture installer update instructions')
        put(self.root, 'Unreal/Saved/VRWindowsArchive/Windows/ACUnreal/Binaries/Win64/ACUnreal.exe',
            b'MZ' + (display + '\0ACE.LocalCosmetics/1\0').encode('utf-16-le'))
        put(self.root, 'Unreal/Saved/VRWindowsArchive/Windows/Update-Client.ps1')
        put(self.root, 'Unreal/Saved/VRWindowsArchive/Windows/Update-Client.exe')
        put(self.root, 'Unreal/Saved/VRWindowsArchive/Windows/ACUnreal/Content/Paks/game.pak')
        put(self.root, 'Unreal/Build/VR/README-WINDOWS.txt')
        put(self.root, 'Unreal/Build/VR/README-LINUX.txt', 'Linux launch and manual update instructions')
        put(self.root, 'Quest/Sharing/RELEASE-NOTES.md', 'Fixture release notes')
        put(self.root, 'LICENSE', 'Fixture license')
        put(self.root, 'Unreal/Build/Branding/AC-Icon.png')
        put(self.root, 'Unreal/Build/Windows/Application.ico')
        put(self.site, '_tools/build-ac-pages.py', '# Test site generator\n')
        if with_linux:
            put(self.linux, build.LINUX_EXE, elf(display))
            put(self.linux, 'AC-Unreal.sh', '#!/bin/sh\nexec ./ACUnreal/Binaries/Linux/ACUnreal -nohmd "$@"\n')
            put(self.linux, 'ACUnreal.sh', '#!/bin/sh\n# UAT bootstrap\n')
            put(self.linux, 'Engine/Binaries/Linux/CrashReportClient', elf('helper'))
            put(self.linux, 'Engine/Binaries/ThirdParty/Linux/libfixture.so', elf('library'))
            put(self.linux, 'ACUnreal/Content/Paks/game.pak')
            put(self.linux, 'ACUnreal/Content/Paks/game.utoc')
            put(self.linux, 'ACUnreal/Content/Paks/game.ucas')
        return self.args

    def assemble(self):
        with contextlib.redirect_stdout(io.StringIO()):
            release = build.build_release(self.root, self.args)
        # Build-WindowsInstaller.ps1 normally appends this after release assembly.
        installer = put(release, f'AC-Unreal-Setup-v{self.args.quest_version}.exe', b'fixture installer')
        meta = json.loads((release / 'release-manifest.json').read_text(encoding='utf-8'))
        meta['windowsInstaller'] = build.artifact(installer)
        self.write_meta(release, meta)
        return release, meta

    def test_optional_transmog_is_not_part_of_client_release(self):
        self.fixture()
        windows = self.root / 'Unreal/Saved/VRWindowsArchive/Windows'
        put(windows, 'ACUnreal/Plugins/ACWoW/ACWoW.uplugin', '{}')
        with self.assertRaisesRegex(ValueError, 'must ship separately'):
            build.verify_client_plugin_boundary(windows)

    def phone_fixture(self):
        self.fixture(version=142)
        self.args.android_version = self.args.windows_version
        apk = put(self.root, 'Mobile/Packaged/Android_ASTC/ACUnreal-arm64.apk', b'phone APK fixture')
        manifest = {'package': 'com.acecommunity.acunreal.mobile', 'versionCode': 142,
                    'version': self.args.android_version, 'bytes': apk.stat().st_size,
                    'sha256': build.sha(apk)}
        put(self.root, 'Mobile/Packaged/android-manifest.json', json.dumps(manifest))
        return apk

    def test_android_preview_has_separate_artifact_and_update_channel(self):
        apk = self.phone_fixture()
        release, meta = self.assemble()
        public = self.publish_local(release)
        self.assertEqual(meta['android']['file'], 'AC-Unreal-Android-v142.apk')
        self.assertEqual((release / meta['android']['file']).read_bytes(), apk.read_bytes())
        self.assertIn(meta['android']['file'], (release / 'SHA256SUMS.txt').read_text())
        self.assertEqual(public['androidVersion'], self.args.android_version)
        self.assertEqual(public['date'], '2026-09-30', 'Release date follows the build version across UTC midnight')
        self.assertTrue(public['androidPreview'])
        self.assertEqual(public['updates']['android'], meta['android'])
        self.assertEqual(public['updates']['quest'], meta['questApk'])
        self.assertNotEqual(public['updates']['android'], public['updates']['quest'])

    def test_android_stale_build_rejected_before_release_is_created(self):
        apk = self.phone_fixture()
        apk.write_bytes(b'changed after verification')
        with self.assertRaisesRegex(ValueError, 'Android APK differs'):
            self.assemble()
        self.assertFalse((self.root / 'Releases').exists())

    def test_android_version_must_match_shared_release(self):
        self.phone_fixture()
        self.args.android_version = '2026.09.30.141'
        with self.assertRaisesRegex(ValueError, 'Android must display'):
            self.assemble()

    def test_release_reports_catalog_lua_api_versions(self):
        self.fixture()
        _, meta = self.assemble()
        self.assertEqual(meta['clientPluginApis']['lua'], 2)
        self.assertEqual(meta['clientPluginApis']['supportedLua'], [1, 2])

    def test_quest_diagnostics_ship_in_full_and_small_downloads(self):
        self.fixture(version=154, revision=11)
        release, meta = self.assemble()
        update = meta['questInstallerUpdate']
        with zipfile.ZipFile(release / update['file']) as archive:
            self.assertEqual(archive.read('Diagnose-Quest.ps1'), b'fixture diagnostic collector')
            self.assertEqual(archive.read('Install-AC2-Sky.cmd'), b'sky launcher')
            self.assertNotIn('AC-VR-arm64.apk', archive.namelist())
        self.assertIn(update['file'], (release / 'SHA256SUMS.txt').read_text())
        public = self.publish_local(release)
        self.assertEqual(public['questInstallerUpdate'], update)

    def test_quest_inconsistent_diagnostics_rejected_before_release_created(self):
        self.fixture(version=154)
        update = self.root / 'Quest/Share/AC-VR-Quest-v154-installer-r6-update-only.zip'
        with zipfile.ZipFile(update) as archive:
            files = {name: archive.read(name) for name in archive.namelist()}
        files['Diagnose-Quest.ps1'] = b'stale helper'
        with zipfile.ZipFile(update, 'w') as archive:
            for name, content in files.items():
                archive.writestr(name, content)
        with self.assertRaisesRegex(ValueError, 'Quest helper differs'):
            self.assemble()
        self.assertFalse((self.root / 'Releases').exists())

    def test_converted_cosmetic_assets_rejected_from_release(self):
        self.fixture()
        windows = self.root / 'Unreal/Saved/VRWindowsArchive/Windows'
        put(windows, 'ACUnreal/Content/mesh.acwm')
        with self.assertRaisesRegex(ValueError, 'cosmetic asset'):
            build.verify_client_plugin_boundary(windows)

    def test_release_requires_shared_cosmetic_api(self):
        self.fixture()
        windows = self.root / 'Unreal/Saved/VRWindowsArchive/Windows'
        put(windows, 'ACUnreal/Binaries/Win64/ACUnreal.exe', b'MZ old client')
        with self.assertRaisesRegex(ValueError, 'lacks the released local cosmetics API'):
            build.verify_client_plugin_boundary(windows)

    def write_meta(self, release, meta):
        (release / 'release-manifest.json').write_text(json.dumps(meta), encoding='utf-8')

    def publish_local(self, release, **kwargs):
        with contextlib.redirect_stdout(io.StringIO()):
            return stage.stage_release(self.root, release, self.site, **kwargs)

    def test_reinstall_preserves_imported_waypoint_atlas(self):
        self.fixture()
        windows = self.root / 'Unreal/Saved/VRWindowsArchive/Windows'
        saved = 'ACUnreal/Saved/ClientPlugins/Waypoint/locations.json'
        pak = 'ACUnreal/Content/Paks/game.pak'
        for staging in (windows, self.linux):
            put(staging, saved, b'build machine atlas')
            put(staging, pak, b'new packaged content')
        release, meta = self.assemble()

        zip_path = release / next(entry['name'] for entry in meta['archives'] if 'Windows' in entry['name'])
        with zipfile.ZipFile(zip_path) as archive:
            install = Path(self.temp.name) / 'install'
            personal = put(install / zip_path.stem, saved, b'player import')
            archive.extractall(install)
            self.assertEqual(personal.read_bytes(), b'player import')
            self.assertEqual((install / zip_path.stem / pak).read_bytes(), b'new packaged content')
        with tarfile.open(release / meta['linux']['file'], 'r:gz') as archive:
            paths = {entry.name.split('/', 1)[-1] for entry in archive if entry.isfile()}
            self.assertIn(pak, paths)
            self.assertNotIn(saved, paths)

    def test_linux_tar_preserves_launch_modes_and_excludes_private_build_files(self):
        self.fixture()
        excluded = (
            'ACUnreal/Saved/Config/Linux/Game.ini', 'ACUnreal/Source/private.cpp',
            'ACUnreal/Intermediate/object.o', 'ACUnreal/Logs/client.log',
            'ACUnreal/DATs/client_portal.dat', 'ACUnreal/Content/client_portal.DAT',
            'ACUnreal/Binaries/Linux/ACUnreal.debug', 'ACUnreal/Binaries/Linux/ACUnreal.sym',
            'ACUnreal/Binaries/Linux/ACUnreal.psym',
            'ACUnreal/Binaries/Linux/game.pdb', 'ACUnreal/Binaries/Win64/game.exe',
            'Engine/DerivedDataCache/cache', 'Engine/Source/private.h',
            'Engine/Binaries/Linux/Manifest_UFSFiles.txt', 'Engine/Binaries/Linux/helper.dll',
            'Manifest_NonUFSFiles.txt', 'Private.txt',
        )
        for name in excluded:
            put(self.linux, name)
        release, meta = self.assemble()
        archive_path = release / meta['linux']['file']
        self.assertEqual(meta['linuxVersion'], self.args.windows_version)
        self.assertEqual(meta['linux'], build.artifact(archive_path))
        self.assertEqual(meta['linuxExeSha256'], build.sha(self.linux / build.LINUX_EXE))
        self.assertIn(archive_path.name, (release / 'SHA256SUMS.txt').read_text())
        with tarfile.open(archive_path, 'r:gz') as archive:
            prefix = 'AC-Unreal-Linux-v88/'
            members = {member.name.removeprefix(prefix): member for member in archive.getmembers() if member.isfile()}
            for name in excluded:
                self.assertNotIn(name, members)
            for name in ('AC-Unreal.sh', 'ACUnreal.sh', build.LINUX_EXE.as_posix(),
                         'Engine/Binaries/Linux/CrashReportClient', 'Engine/Binaries/ThirdParty/Linux/libfixture.so'):
                self.assertEqual(members[name].mode, 0o755, name)
            for name in ('README-LINUX.txt', 'RELEASE-NOTES.md', 'LICENSE', 'ACUnreal/Content/Paks/game.pak'):
                self.assertEqual(members[name].mode, 0o644, name)
            self.assertEqual(archive.extractfile(prefix + 'LICENSE').read(), b'Fixture license')

    def test_v88_stages_linux_and_preserves_windows_quest_update_protocol(self):
        self.fixture()
        release, meta = self.assemble()
        public = self.publish_local(release)
        self.assertEqual(public['linux'], meta['linux'])
        self.assertEqual(public['linuxVersion'], '2026.09.30.88')
        self.assertEqual(public['questVersion'], '2026.09.30.quest.88')
        self.assertEqual(public['questInstalledVersion'], '2026.09.30.88')
        self.assertEqual(public['updates'], {'schema': 1, 'windows': meta['windowsInstaller'],
                                            'quest': meta['questApk'], 'linux': meta['linux']})
        target = self.site / 'downloads/ac/v88' / meta['linux']['file']
        self.assertEqual(build.sha(target), meta['linux']['sha256'])

    def test_v87_without_linux_still_stages(self):
        self.fixture(version=87, with_linux=False)
        release, _ = self.assemble()
        public = self.publish_local(release)
        self.assertNotIn('linux', public)
        self.assertNotIn('linuxVersion', public)
        self.assertEqual(set(public['updates']), {'schema', 'windows', 'quest'})

    def test_release_88_cannot_omit_linux(self):
        self.fixture(with_linux=False)
        with self.assertRaisesRegex(ValueError, 'require a native Linux build'):
            build.build_release(self.root, self.args)
        self.assertFalse((self.root / 'Releases').exists())

    def test_linux_missing_stale_wrong_architecture_or_unreal_string_encoding_rejected(self):
        self.fixture()
        exe = self.linux / build.LINUX_EXE
        for value in (elf('2026.09.30.87'), elf('2026.09.30.880'), elf(self.args.linux_version, machine=183),
                      elf(self.args.linux_version, encoding='utf-8'), elf(self.args.linux_version, encoding='utf-32-le'), b'MZ'):
            with self.subTest(binary=value):
                exe.write_bytes(value)
                with self.assertRaises(ValueError):
                    build.build_release(self.root, self.args)
                self.assertFalse((self.root / 'Releases').exists())
        exe.unlink()
        with self.assertRaisesRegex(ValueError, 'Package Linux first'):
            build.build_release(self.root, self.args)

    def test_linux_cooked_content_and_launcher_required(self):
        self.fixture()
        (self.linux / 'ACUnreal/Content/Paks/game.pak').unlink()
        with self.assertRaisesRegex(ValueError, 'cooked content is missing'):
            build.build_release(self.root, self.args)
        put(self.linux, 'ACUnreal/Content/Paks/game.pak')
        (self.linux / 'AC-Unreal.sh').unlink()
        with self.assertRaisesRegex(ValueError, 'launcher is missing'):
            build.build_release(self.root, self.args)
        self.assertFalse((self.root / 'Releases').exists())

    def test_mismatched_linux_version_rejected_by_build_and_stage(self):
        self.fixture()
        self.args.linux_version = '2026.09.30.87'
        with self.assertRaisesRegex(ValueError, 'must display the same version'):
            build.build_release(self.root, self.args)
        self.args.linux_version = self.args.windows_version
        release, meta = self.assemble()
        meta['linuxVersion'] = '2026.09.30.87'
        self.write_meta(release, meta)
        with self.assertRaisesRegex(ValueError, 'must display the same version'):
            self.publish_local(release)
        self.assertFalse((self.site / 'downloads').exists())

    def test_v88_cannot_stage_without_linux_metadata(self):
        self.fixture()
        release, meta = self.assemble()
        del meta['linux']
        del meta['linuxVersion']
        self.write_meta(release, meta)
        with self.assertRaisesRegex(ValueError, 'require the native Linux archive'):
            self.publish_local(release)
        self.assertFalse((self.site / 'downloads').exists())

    def test_corrupt_linux_archive_fails_before_staging(self):
        self.fixture()
        release, meta = self.assemble()
        (release / meta['linux']['file']).write_bytes(b'corrupted')
        with self.assertRaisesRegex(ValueError, 'Checksum mismatch'):
            self.publish_local(release)
        self.assertFalse((self.site / 'downloads').exists())

    def test_linux_cannot_use_windows_fallback(self):
        self.fixture()
        release, meta = self.assemble()
        meta['linux'] = meta['windowsInstaller']
        self.write_meta(release, meta)
        with self.assertRaisesRegex(ValueError, 'native versioned tar.gz archive'):
            self.publish_local(release)
        self.assertFalse((self.site / 'downloads').exists())

    def test_existing_linux_artifact_is_immutable_unless_explicitly_unpublished(self):
        self.fixture()
        release, meta = self.assemble()
        target = put(self.site, 'downloads/ac/v88/' + meta['linux']['file'], b'previously distributed')
        with self.assertRaisesRegex(ValueError, 'different published artifact'):
            self.publish_local(release)
        self.assertEqual(target.read_bytes(), b'previously distributed')
        self.assertFalse((target.parent / meta['windowsInstaller']['file']).exists())
        self.assertFalse((self.site / 'assets/ac/release.json').exists())
        self.publish_local(release, replace_unpublished=True)
        self.assertEqual(build.sha(target), meta['linux']['sha256'])
        # An identical local restage remains allowed without the override.
        self.publish_local(release)


if __name__ == '__main__':
    unittest.main(verbosity=2)
