#requires -Version 5.1
param([ValidateRange(1, 999)][int]$InstallerRevision = 11)
$ErrorActionPreference = 'Stop'
$apk = Join-Path $PSScriptRoot 'Packaged\Android_ASTC\ACUnreal-arm64.apk'
$aapt = Join-Path $PSScriptRoot 'AndroidSDK\build-tools\35.0.1\aapt.exe'
$metadata = & $aapt dump badging $apk | Out-String
if ($LASTEXITCODE -ne 0 -or $metadata -notmatch "package: name='com\.acecommunity\.questtest' versionCode='(\d+)' versionName='([^']+)'") {
    throw 'Cannot verify APK package and version. Build the Quest APK first.'
}
$storeVersion = [int]$Matches[1]
$version = $Matches[2]
if ($metadata -notmatch "(?m)^application-label:'AC:VR'\s*$") {
    throw 'Build the renamed AC:VR APK before exporting a new release.'
}
$root = Join-Path $PSScriptRoot 'Share'
$name = "AC-VR-Quest-v$storeVersion"
if ($InstallerRevision -gt 1) { $name += "-installer-r$InstallerRevision" }
$folder = Join-Path $root $name
$zip = Join-Path $root "$name.zip"
$patchZip = Join-Path $root "$name-update-only.zip"
if ((Test-Path -LiteralPath $folder) -or (Test-Path -LiteralPath $zip) -or (Test-Path -LiteralPath $patchZip)) {
    throw "Bundle already exists: $name. Preserve it; use a new build version or -InstallerRevision for a new export."
}
New-Item -ItemType Directory -Path $folder -Force | Out-Null
Copy-Item -LiteralPath $apk -Destination (Join-Path $folder 'AC-VR-arm64.apk')
$helpers = @('Install-Quest.ps1', 'Install-Quest.cmd', 'Update-Quest.cmd', 'Repair-Quest.cmd', 'Diagnose-Quest.cmd', 'Diagnose-Quest.ps1', 'Quest-DataTransfer.ps1', 'Quest-ProfileMigration.ps1', 'README.txt')
if ($InstallerRevision -ge 11) { $helpers += 'Install-AC2-Sky.cmd' }
foreach ($file in ($helpers + 'RELEASE-NOTES.md')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "Sharing\$file") -Destination (Join-Path $folder $file)
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\LICENSE') -Destination (Join-Path $folder 'LICENSE')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Sharing\START-HERE.html') -Destination $folder
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '..\Unreal\Build\Android\res\drawable\icon.png') -Destination (Join-Path $folder 'AC-Icon.png')
$copiedApk = Join-Path $folder 'AC-VR-arm64.apk'
[ordered]@{
    package = 'com.acecommunity.questtest'
    displayName = 'AC:VR'
    version = $version
    versionCode = $storeVersion
    installerRevision = $InstallerRevision
    apk = 'AC-VR-arm64.apk'
    apkBytes = (Get-Item -LiteralPath $copiedApk).Length
    apkSha256 = (Get-FileHash -LiteralPath $copiedApk -Algorithm SHA256).Hash
    createdUtc = [DateTime]::UtcNow.ToString('o')
    requiredData = @('client_portal.dat', 'client_cell_1.dat', 'client_local_English.dat')
    optionalData = @('client_highres.dat')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $folder 'manifest.json') -Encoding UTF8
# APK is already compressed. The explicit file allowlist keeps Saved/configs,
# credentials, DAT files, SDK tools, and future unrelated files out of the ZIP.
$files = $helpers + @('AC-VR-arm64.apk', 'RELEASE-NOTES.md', 'LICENSE', 'manifest.json', 'START-HERE.html', 'AC-Icon.png')
$paths = @($files | ForEach-Object { Join-Path $folder $_ })
Compress-Archive -LiteralPath $paths -DestinationPath $zip -CompressionLevel NoCompression
$patchPaths = @($helpers | ForEach-Object { Join-Path $folder $_ })
$patchPaths += Join-Path $PSScriptRoot 'Sharing\INSTALLER-UPDATE.txt'
Compress-Archive -LiteralPath $patchPaths -DestinationPath $patchZip -CompressionLevel Optimal
Get-Item -LiteralPath $zip, $patchZip | Select-Object FullName, Length | Format-Table -AutoSize
Get-FileHash -LiteralPath $zip, $patchZip -Algorithm SHA256 | Format-List Path, Hash
