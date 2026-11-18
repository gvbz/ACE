"""Assemble existing Quest, Windows, and native Linux builds into a local release.

Run the platform build scripts, tests, and Build-SharePackage.ps1 first. Release
88 and later require --linux-version, matching --windows-version. This packages
existing builds; it does not certify headset behavior or Linux runtime acceptance.
"""
from pathlib import Path
from datetime import date, datetime, timezone
import argparse
import gzip
import hashlib
import json
import mmap
import shutil
import tarfile
import zipfile


LINUX_FIRST_RELEASE = 88
LOCAL_COSMETICS_API = 'ACE.LocalCosmetics/1'
LINUX_EXE = Path('ACUnreal/Binaries/Linux/ACUnreal')
LINUX_LAUNCHER = Path('AC-Unreal.sh')
LINUX_ROOTS = {'ACUnreal', 'Engine', 'ACUnreal.sh', 'AC-Unreal.sh', 'NOTICES.txt'}
LINUX_EXCLUDED_DIRS = {
    'saved', 'intermediate', 'source', 'logs', 'dats', 'deriveddatacache',
    'win64', 'win32', 'windows', 'android', 'mac', 'linuxarm64',
}
LINUX_EXCLUDED_SUFFIXES = {
    '.pdb', '.log', '.dat', '.dmp', '.debug', '.sym', '.psym', '.symbols', '.map',
    '.o', '.obj', '.a', '.lib', '.exe', '.dll', '.bat', '.cmd', '.ps1', '.apk',
    '.target',
}


def require(condition, message):
    # Release validation must remain active under python -O.
    if not condition:
        raise ValueError(message)


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest().upper()


def artifact(path):
    return {'file': path.name, 'bytes': path.stat().st_size, 'sha256': sha(path)}


def verify_client_plugin_boundary(windows):
    """The client ships the reusable API; optional transmog packages ship separately."""
    exe = windows / 'ACUnreal/Binaries/Win64/ACUnreal.exe'
    require(exe.is_file(), f'Missing Windows client: {exe}')
    with exe.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as binary:
        require(binary.find(LOCAL_COSMETICS_API.encode('utf-16-le')) >= 0,
                'Windows client lacks the released local cosmetics API; rebuild ACEClient')
    for path in windows.rglob('*'):
        if not path.is_file():
            continue
        rel = path.relative_to(windows)
        parts = [part.lower() for part in rel.parts]
        if 'saved' in parts:
            continue  # User-installed packages/settings are already excluded from releases.
        require('acwow' not in parts and path.name.lower() not in {'acwow.local.exe', 'acwow.uplugin'},
                f'Optional AC-WoW Transmog files must ship separately: {rel}')
        require(path.suffix.lower() not in {'.m2', '.skin', '.blp', '.acwm'},
                f'Source or converted cosmetic asset in client release: {rel}')


def verify_binary_version(path, version, *, linux=False):
    require(path.is_file() and path.stat().st_size > 0, f'Package {"Linux" if linux else "Windows"} first: {path}')
    with path.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as binary:
        if linux:
            # The supported target is ELF64, little endian, EM_X86_64 (62).
            require(len(binary) >= 64 and binary[:6] == b'\x7fELF\x02\x01'
                    and binary[18:20] == b'\x3e\x00',
                    'Packaged Linux executable is not a native x86_64 ELF')
        # UE 5.8 UnixPlatform.h defines WIDECHAR/TCHAR as char16_t and
        # PLATFORM_TCHAR_IS_CHAR16=1, despite Linux wchar_t being 32 bits.
        # ACEClientBuild::Version is TEXT(...), so both platforms embed UTF-16LE.
        # Include the terminator to reject a version that merely prefixes another.
        require(binary.find((version + '\0').encode('utf-16-le')) >= 0,
                f'Packaged {"Linux" if linux else "Windows"} executable has a stale display version')


def is_linux_runtime_path(rel):
    return (rel.parts[0] in LINUX_ROOTS
            and not any(part.lower() in LINUX_EXCLUDED_DIRS for part in rel.parts)
            and rel.suffix.lower() not in LINUX_EXCLUDED_SUFFIXES
            and not rel.name.startswith('Manifest_'))


def linux_payload(linux):
    """Select runtime files only, refusing links that escape the build archive."""
    files = []
    for path in sorted(linux.rglob('*')):
        rel = path.relative_to(linux)
        if not is_linux_runtime_path(rel):
            continue
        if path.is_symlink():
            require(path.resolve().is_relative_to(linux.resolve()), f'Linux runtime link escapes archive: {rel}')
            require(path.is_file(), f'Unsupported Linux runtime directory or broken link: {rel}')
            target_rel = path.resolve().relative_to(linux.resolve())
            require(is_linux_runtime_path(target_rel), f'Linux runtime link points to excluded data: {rel}')
            require(target_rel.name.lower() not in {'config.js', 'log4net.config'}, 'Local server config in Linux runtime')
        if not path.is_file():
            continue
        require(path.name.lower() not in {'config.js', 'log4net.config'}, 'Local server config in Linux runtime')
        files.append((path, rel.as_posix()))
    require(LINUX_EXE.as_posix() in {rel for _, rel in files}, 'Linux runtime executable is missing')
    require(LINUX_LAUNCHER.as_posix() in {rel for _, rel in files}, 'Linux desktop launcher is missing')
    require(any(rel.startswith('ACUnreal/Content/Paks/') and path.suffix.lower() == '.pak'
                for path, rel in files), 'Linux cooked content is missing; run Build-Linux.ps1')
    return files


def linux_file_mode(path):
    with path.open('rb') as stream:
        magic = stream.read(4)
    # Windows cross-builds do not retain POSIX execute bits. Mark every shell
    # bootstrap and ELF (including CrashReportClient/libraries) as executable.
    return 0o755 if (path.suffix == '.sh' or magic.startswith(b'#!')
                     or magic == b'\x7fELF' or path.stat().st_mode & 0o111) else 0o644


def build_linux_archive(root, linux, release, version, display_version):
    verify_binary_version(linux / LINUX_EXE, display_version, linux=True)
    files = linux_payload(linux)
    files += [(root / source, name) for source, name in (
        ('Unreal/Build/VR/Update-Client.py', 'Update-Client.py'),
        ('Unreal/Build/VR/README-LINUX.txt', 'README-LINUX.txt'),
        ('Quest/Sharing/RELEASE-NOTES.md', 'RELEASE-NOTES.md'), ('LICENSE', 'LICENSE'))]
    for path, _ in files:
        require(path.is_file(), f'Missing Linux release input: {path}')
    archive_path = release / f'AC-Unreal-Linux-v{version}.tar.gz'
    prefix = f'AC-Unreal-Linux-v{version}'
    directories = set()
    with tarfile.open(archive_path, 'w:gz', compresslevel=1, dereference=True) as archive:
        for path, rel in files:
            member_name = f'{prefix}/{rel}'
            for directory in reversed(Path(member_name).parents):
                name = directory.as_posix()
                if name == '.' or name in directories:
                    continue
                info = tarfile.TarInfo(name)
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
                directories.add(name)
            info = archive.gettarinfo(str(path), arcname=member_name)
            info.mode = linux_file_mode(path)
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            with path.open('rb') as stream:
                archive.addfile(info, stream)
    # Consume the gzip stream completely to verify its CRC without loading the
    # multi-GB runtime into memory; tarfile alone can stop at the tar end marker.
    with gzip.open(archive_path, 'rb') as stream:
        while stream.read(1024 * 1024):
            pass
    with tarfile.open(archive_path, 'r:gz') as archive:
        for rel in (LINUX_EXE, LINUX_LAUNCHER):
            info = archive.getmember(f'{prefix}/{rel.as_posix()}')
            require(info.isfile() and info.mode == 0o755, f'Linux executable permissions missing: {rel}')
    return archive_path, [rel for _, rel in files]


def build_release(root, args):
    api = json.loads((root / 'Unreal/Plugins/ACEClient/ClientMods/api.json').read_text(encoding='utf-8-sig'))
    quest = root / 'Quest'
    windows = root / 'Unreal/Saved/VRWindowsArchive/Windows'
    linux = root / 'Unreal/Saved/LinuxArchive/Linux'
    version = args.quest_version
    android_meta = {}
    android_apk = root / 'Mobile/Packaged/Android_ASTC/ACUnreal-arm64.apk'
    if getattr(args, 'android_version', None):
        require(args.android_version == args.windows_version, 'Android must display the shared release version')
        android = json.loads((root / 'Mobile/Packaged/android-manifest.json').read_text(encoding='utf-8-sig'))
        require(android['package'] == 'com.acecommunity.acunreal.mobile'
                and android['versionCode'] == version and android['version'] == args.android_version,
                'Android package identity/version differs from release')
        require(android_apk.stat().st_size == android['bytes'] and sha(android_apk) == android['sha256'],
                'Android APK differs from verified build metadata')
    require(version < LINUX_FIRST_RELEASE or args.linux_version,
            'Release 88 and later require a native Linux build and --linux-version')
    require(not args.linux_version or args.linux_version == args.windows_version,
            'Linux and Windows must display the same version')
    quest_zip = quest / f'Share/AC-VR-Quest-v{version}-installer-r{args.installer_revision}.zip'
    quest_update_zip = quest_zip.with_name(quest_zip.stem + '-update-only.zip')
    quest_update_meta = {}
    apk = quest / 'Packaged/Android_ASTC/ACUnreal-arm64.apk'
    release = root / f'Releases/{date.today():%Y.%m.%d}-v{version}'
    require(not release.exists(), f'Release already exists; preserve it: {release}')
    with zipfile.ZipFile(quest_zip) as archive:
        expected = {'AC-VR-arm64.apk', 'Install-Quest.ps1', 'Install-Quest.cmd', 'Update-Quest.cmd',
                    'Repair-Quest.cmd', 'Quest-DataTransfer.ps1', 'Quest-ProfileMigration.ps1', 'README.txt', 'RELEASE-NOTES.md', 'LICENSE', 'manifest.json', 'START-HERE.html', 'AC-Icon.png'}
        if version >= 154:
            expected.update({'Diagnose-Quest.cmd', 'Diagnose-Quest.ps1'})
        if args.installer_revision >= 11:
            expected.add('Install-AC2-Sky.cmd')
        require(set(archive.namelist()) == expected, 'Unexpected Quest bundle contents')
        require(archive.testzip() is None, 'Quest ZIP checksum failure')
        manifest = json.loads(archive.read('manifest.json').decode('utf-8-sig'))
        require(manifest['versionCode'] == version and manifest['installerRevision'] == args.installer_revision,
                'Quest bundle version or installer revision differs')
        require(manifest['version'] == args.windows_version, 'Windows and Quest must display the same version')
        require(int(args.windows_version.rsplit('.', 1)[1]) == version, 'Display version must end in the release number')
        require(manifest['displayName'] == 'AC:VR', 'Rebuild the Quest bundle with current branding')
        require(manifest['apkSha256'] == sha(apk), 'Quest APK differs from the installer manifest')
        require(hashlib.sha256(archive.read('AC-VR-arm64.apk')).hexdigest().upper() == manifest['apkSha256'],
                'Quest installer APK checksum mismatch')
        if version >= 154:
            helpers = {'Install-Quest.ps1', 'Install-Quest.cmd', 'Update-Quest.cmd',
                       'Repair-Quest.cmd', 'Diagnose-Quest.cmd', 'Diagnose-Quest.ps1',
                       'Quest-DataTransfer.ps1', 'Quest-ProfileMigration.ps1', 'README.txt'}
            if args.installer_revision >= 11:
                helpers.add('Install-AC2-Sky.cmd')
            with zipfile.ZipFile(quest_update_zip) as update:
                require(set(update.namelist()) == helpers | {'INSTALLER-UPDATE.txt'}, 'Unexpected Quest installer-only contents')
                require(update.testzip() is None, 'Quest installer-only ZIP checksum failure')
                require(update.read('INSTALLER-UPDATE.txt') == (quest / 'Sharing/INSTALLER-UPDATE.txt').read_bytes(),
                        'Quest installer-only instructions differ from source')
                for name in helpers:
                    require(update.read(name) == archive.read(name), f'Quest helper differs between bundles: {name}')
    exe = windows / 'ACUnreal/Binaries/Win64/ACUnreal.exe'
    # The source stamp is a consistency check, not a substitute for build validation.
    stamp = (root / 'Unreal/Plugins/ACEClient/Source/ACEClient/Public/ACEClientBuild.h').read_text()
    require(f'"{args.windows_version}"' in stamp, 'Windows version differs from source stamp')
    require(f'ReleaseNumber = {version};' in stamp, 'Updater release number differs from package')
    verify_binary_version(exe, args.windows_version)
    verify_client_plugin_boundary(windows)
    require((windows / 'Update-Client.ps1').is_file(), 'Updater helper missing from Windows package')
    require((windows / 'Update-Client.exe').is_file(), 'Native updater helper missing from Windows package')
    if args.linux_version:
        # Preflight before creating an immutable release directory or copying GBs.
        verify_binary_version(linux / LINUX_EXE, args.linux_version, linux=True)
        linux_payload(linux)
        require((root / 'Unreal/Build/VR/README-LINUX.txt').is_file(), 'Linux README is missing')
    release.mkdir(parents=True)
    shutil.copy2(quest_zip, release / quest_zip.name)
    win_zip = release / f'AC-Unreal-and-AC-VR-Windows-v{version}.zip'
    prefix = f'AC-Unreal-and-AC-VR-Windows-v{version}/'
    members = []
    with zipfile.ZipFile(win_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=1, allowZip64=True) as archive:
        for path in sorted(windows.rglob('*')):
            if not path.is_file():
                continue
            rel = path.relative_to(windows)
            if any(part.lower() in {'saved', 'intermediate', 'source', 'logs', 'dats'} for part in rel.parts):
                continue
            if path.suffix.lower() in {'.pdb', '.log', '.dat', '.dmp'} or path.name.startswith('Manifest_'):
                continue
            if rel.parts[0] not in {'ACUnreal', 'Engine', 'ACUnreal.exe', 'Launch-VR.ps1', 'Launch-VR.bat', 'AC-Unreal.bat', 'AC-VR.bat', 'Update-Client.ps1', 'Update-Client.exe', 'NOTICES.txt'}:
                continue
            require(path.name.lower() not in {'config.js', 'log4net.config'}, 'Local server config in runtime')
            archive.write(path, prefix + rel.as_posix())
            members.append(rel.as_posix())
        for source, name in [('Unreal/Build/VR/README-WINDOWS.txt', 'README-WINDOWS.txt'),
                             ('Quest/Sharing/RELEASE-NOTES.md', 'RELEASE-NOTES.md'), ('LICENSE', 'LICENSE')]:
            archive.write(root / source, prefix + name)
    with zipfile.ZipFile(win_zip) as archive:
        require(archive.testzip() is None, 'Windows ZIP checksum failure')
    archives = [release / quest_zip.name, win_zip]
    linux_meta = {}
    if args.linux_version:
        linux_tar, linux_members = build_linux_archive(root, linux, release, version, args.linux_version)
        archives.append(linux_tar)
        linux_meta = {'linuxVersion': args.linux_version, 'linux': artifact(linux_tar),
                      'linuxExeSha256': sha(linux / LINUX_EXE), 'linuxFiles': linux_members}
    shutil.copy2(quest / 'Sharing/RELEASE-NOTES.md', release / 'RELEASE-NOTES.md')
    direct_apk = release / f'AC-VR-Quest-v{version}.apk'
    shutil.copy2(apk, direct_apk)
    downloads = archives + [direct_apk]
    if version >= 154:
        copied_update = release / quest_update_zip.name
        shutil.copy2(quest_update_zip, copied_update)
        downloads.append(copied_update)
        quest_update_meta = {'questInstallerUpdate': artifact(copied_update)}
    if getattr(args, 'android_version', None):
        phone_apk = release / f'AC-Unreal-Android-v{version}.apk'
        shutil.copy2(android_apk, phone_apk)
        downloads.append(phone_apk)
        android_meta = {'androidVersion': args.android_version, 'android': artifact(phone_apk), 'androidPreview': True}
    (release / 'SHA256SUMS.txt').write_text(''.join(f'{sha(p)}  {p.name}\n' for p in downloads), encoding='utf-8')
    (release / 'release-manifest.json').write_text(json.dumps({
        'desktopProduct': 'AC:Unreal', 'vrProduct': 'AC:VR',
        'createdUtc': datetime.now(timezone.utc).isoformat(), 'questVersion': manifest['version'],
        'releaseDate': date.fromisoformat(args.windows_version.rsplit('.', 1)[0].replace('.', '-')).isoformat(),
        'questVersionCode': version, 'installerRevision': args.installer_revision,
        'windowsVersion': args.windows_version, 'apkSha256': sha(apk), 'windowsExeSha256': sha(exe),
        'clientPluginApis': {'lua': api['lua_api'], 'supportedLua': api['supported_lua_apis'],
                             'localCosmetics': 1, 'localCosmeticsPlatforms': ['Win64']},
        'gameDataIncluded': False, 'serverIncluded': False, 'savedSettingsIncluded': False,
        'headsetAcceptance': 'See RELEASE-NOTES.md; packaging alone is not live acceptance.',
        'archives': [{'name': p.name, 'bytes': p.stat().st_size, 'sha256': sha(p)} for p in archives],
        'questApk': artifact(direct_apk), 'windowsFiles': members, **linux_meta, **android_meta, **quest_update_meta,
    }, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'release': str(release), 'archives': [{'name': p.name, 'bytes': p.stat().st_size} for p in archives]}, indent=2))
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quest-version', type=int, required=True)
    parser.add_argument('--windows-version', required=True)
    parser.add_argument('--linux-version', help='Required for release 88 onward; must match the Windows/Quest display version')
    parser.add_argument('--installer-revision', type=int, default=11)
    parser.add_argument('--android-version', help='Include the verified Android phone/tablet preview APK')
    args = parser.parse_args()
    try:
        build_release(Path(__file__).resolve().parents[3], args)
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from error


if __name__ == '__main__':
    main()
