#requires -Version 7.0
param(
    [string]$Serial,
    [string]$DatDirectory = "C:\Turbine\Asheron's Call",
    [string]$AC2SkyPath,
    [switch]$SkipData,
    [switch]$NoLaunch
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Sharing/Quest-DataTransfer.ps1')
if ($AC2SkyPath) { $AC2SkyPath = Resolve-QuestSkySource $AC2SkyPath }
$adb = Join-Path $PSScriptRoot 'AndroidSDK\platform-tools\adb.exe'
$package = 'com.acecommunity.questtest'
$devices = @(& $adb devices | Select-String '^([^\s]+)\s+device$' | ForEach-Object { $_.Matches[0].Groups[1].Value })
if (!$Serial) {
    if ($devices.Count -ne 1) {
        & $adb devices -l
        throw 'Connect one Quest and accept Allow USB debugging, or specify -Serial.'
    }
    $Serial = $devices[0]
}
if ($Serial -notin $devices) { throw 'The specified device is not authorized for USB debugging.' }
$deviceArgs = @('-s', $Serial)
$model = (& $adb @deviceArgs shell getprop ro.product.model | Out-String).Trim()
if ($model -notmatch '^Quest 3(S)?$') { throw "Expected Quest 3, found '$model'." }
$apks = @(Get-ChildItem (Join-Path $PSScriptRoot 'Packaged') -Recurse -Filter '*.apk' | Where-Object Name -NotLike 'AFS*')
if ($apks.Count -ne 1) { throw 'Expected one packaged AC:VR APK. Run Build-Quest.ps1 first.' }
$apk = $apks[0].FullName
$aapt = Join-Path $PSScriptRoot 'AndroidSDK\build-tools\35.0.1\aapt.exe'
$metadata = & $aapt dump badging $apk
if ($LASTEXITCODE -ne 0 -or !($metadata | Select-String "^package: name='$package'")) {
    throw 'APK does not identify itself as the isolated AC:VR.'
}
if (!$SkipData) {
    foreach ($name in @('client_portal.dat', 'client_cell_1.dat', 'client_local_English.dat')) {
        if (!(Test-Path -LiteralPath (Join-Path $DatDirectory $name))) { throw "Missing DAT file: $name" }
    }
}
Write-Host "Installing $apk on $model"
& $adb @deviceArgs install -r $apk
if ($LASTEXITCODE -ne 0) { throw 'APK installation failed.' }
# Use the same atomic, verified transfer as the friend installer.
. (Join-Path $PSScriptRoot 'Sharing/Quest-ProfileMigration.ps1')
$AdbPath = $adb
function Invoke-Adb {
    param([string[]]$Arguments)
    $ErrorActionPreference = 'Continue'
    $output = @(& $AdbPath @Arguments 2>&1)
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "ADB failed (exit $code): $($output -join [Environment]::NewLine)" }
    $output | ForEach-Object { [string]$_ }
}
function Wait-QuestConnection {
    param([string]$RequestedSerial)
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    do {
        try {
            $state = (Invoke-Adb -Arguments @('-s',$RequestedSerial,'get-state') | Out-String).Trim()
            if ($state -eq 'device') { return [pscustomobject]@{Serial=$RequestedSerial} }
        } catch { }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Quest did not reconnect within two minutes. Check the cable and USB debugging prompt.' }
        Start-Sleep -Seconds 1
    } while ($true)
}
$saved = 'files/ACUnreal/Saved'
Move-QuestLegacyProfile
if (!$SkipData) {
    Install-QuestData -DatDirectory $DatDirectory -DiagnosticDirectory (Join-Path $PSScriptRoot 'Logs')
}
if ($AC2SkyPath) { Install-QuestSky $AC2SkyPath (Join-Path $PSScriptRoot 'Logs') }
if (!$NoLaunch) {
    $powerState = (& $adb @deviceArgs shell dumpsys power | Out-String)
    if ($powerState -match 'mWakefulness=Awake') {
        & $adb @deviceArgs shell am start -W -n "$package/com.epicgames.unreal.GameActivity"
        if ($LASTEXITCODE -ne 0) { throw 'Android could not launch the test activity.' }
    } else {
        Write-Host 'Headset is asleep. Put it on and open AC:VR from Apps > Unknown Sources.'
    }
}
Write-Host "Installed AC:VR. Default server: 10.0.0.26:9000. DAT path: $saved/DAT"
