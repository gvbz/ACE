#requires -Version 7.0
param([string]$Serial, [string]$AdbPath = (Join-Path $PSScriptRoot '../AndroidSDK/platform-tools/adb.exe'))
$ErrorActionPreference = 'Stop'
$device = if ($Serial) { @('-s', $Serial) } else { @() }
$package = 'com.acecommunity.questtest'
$fixture = 'files/ProfileMigrationTest-' + [Guid]::NewGuid().ToString('N')
$helper = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../Sharing/Quest-ProfileMigration.ps1') -Raw
$match = [regex]::Match($helper, '(?s)\$migration = @\x27\r?\n(.*?)\r?\n\x27@')
if (!$match.Success) { throw 'Could not extract the installer migration for the device fixture.' }
$migration = $match.Groups[1].Value.Replace('files/ACEViewer', "$fixture/old").Replace('files/ACUnreal', "$fixture/new").Replace("`r", '')
function Invoke-FixtureShell([string]$Script) {
    & $AdbPath @device shell run-as $package sh -c "'$($Script.Replace("`r", ''))'"
    if ($LASTEXITCODE -ne 0) { throw 'Android profile migration fixture failed.' }
}
try {
    Invoke-FixtureShell @"
set -eu
mkdir -p $fixture/old/Saved/Login $fixture/old/Saved/DAT
echo encrypted-fixture > $fixture/old/Saved/Login/LastLogin.dat
echo dat-fixture > $fixture/old/Saved/DAT/client_portal.dat
$migration
test ! -e $fixture/old
test -f $fixture/new/Saved/Login/LastLogin.dat
test -f $fixture/new/Saved/DAT/client_portal.dat
echo PASS atomic-profile-rename
atlas=Saved/ClientPlugins/Waypoint/locations.json
mkdir -p $fixture/old/Saved/ClientPlugins/Waypoint
echo old-atlas > $fixture/old/`$atlas
mkdir -p $fixture/old/Saved/Login $fixture/old/Saved/DAT $fixture/old/Saved/Config/Android
echo old-login > $fixture/old/Saved/Login/LastLogin.dat
echo old-dat > $fixture/old/Saved/DAT/client_portal.dat
echo cell-fixture > $fixture/old/Saved/DAT/client_cell_1.dat
echo settings-fixture > $fixture/old/Saved/Config/Android/ACEVR.ini
$migration
grep -q encrypted-fixture $fixture/new/Saved/Login/LastLogin.dat
grep -q dat-fixture $fixture/new/Saved/DAT/client_portal.dat
grep -q cell-fixture $fixture/new/Saved/DAT/client_cell_1.dat
grep -q settings-fixture $fixture/new/Saved/Config/Android/ACEVR.ini
grep -qx old-atlas $fixture/new/`$atlas
echo PASS merge-preserves-new-profile-and-fills-missing-files
echo current-atlas > $fixture/new/`$atlas
echo legacy-atlas > $fixture/old/`$atlas
$migration
grep -q encrypted-fixture $fixture/new/Saved/Login/LastLogin.dat
grep -qx current-atlas $fixture/new/`$atlas
echo PASS repeat-migration
"@
} finally {
    # Generated fixture only; never operate on the installed game's profile.
    if ($fixture -notmatch '^files/ProfileMigrationTest-[0-9a-f]{32}$') { throw 'Unexpected fixture path' }
    Invoke-FixtureShell "rm -r $fixture"
}
