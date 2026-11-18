#requires -Version 5.1
param(
    [string]$AdbPath,
    [string]$DatDirectory,
    [string]$AC2SkyPath,
    [switch]$SkyOnly,
    [string]$Serial,
    [switch]$SkipData,
    [switch]$NoLaunch,
    [switch]$ValidateOnly,
    [switch]$CheckConnection,
    [switch]$RepairData,
    [ValidateRange(0, 600)]
    [int]$ConnectionTimeoutSeconds = 120
)
$ErrorActionPreference = 'Stop'
if ($RepairData -and $SkipData) { throw '-RepairData and -SkipData cannot be combined.' }
. (Join-Path $PSScriptRoot 'Quest-DataTransfer.ps1')
. (Join-Path $PSScriptRoot 'Quest-ProfileMigration.ps1')
$package = 'com.acecommunity.questtest'
$apk = Join-Path $PSScriptRoot 'AC-VR-arm64.apk'
if (!$SkyOnly) {
    $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw | ConvertFrom-Json
    if ($manifest.package -ne $package -or $manifest.apk -ne 'AC-VR-arm64.apk') {
        throw 'Unexpected bundle manifest. Extract the complete original ZIP again.'
    }
    if (!(Test-Path -LiteralPath $apk -PathType Leaf)) { throw 'APK missing. Extract the entire ZIP first.' }
    Write-Host 'Verifying the APK checksum...'
    if ((Get-Item -LiteralPath $apk).Length -ne $manifest.apkBytes -or
        (Get-FileHash -LiteralPath $apk -Algorithm SHA256).Hash -ne $manifest.apkSha256) {
        throw 'APK checksum mismatch. Download and extract the ZIP again.'
    }
    $displayName = if ($manifest.displayName) { $manifest.displayName } else { 'AC:VR' }
    Write-Host "Verified $displayName $($manifest.version) (bundle revision $($manifest.installerRevision))"
    if ($ValidateOnly) { return }
}

if ($SkyOnly -and !$AC2SkyPath -and !$CheckConnection) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    try {
        $dialog.Title = 'Choose your AC2 Legions portal.dat or imported Legions.sky'
        $dialog.Filter = 'AC2 sky sources (portal.dat or *.sky)|portal.dat;*.sky'
        if ($dialog.ShowDialog() -ne 'OK') { Write-Host 'Sky transfer cancelled.'; return }
        $AC2SkyPath = $dialog.FileName
    } finally { $dialog.Dispose() }
}
if ($AC2SkyPath) { $AC2SkyPath = Resolve-QuestSkySource $AC2SkyPath }
if ($SkyOnly -and $ValidateOnly) { Write-Host "Validated sky transfer source: $AC2SkyPath"; return }

if (!$AdbPath) {
    $bundledAdb = Join-Path $PSScriptRoot 'platform-tools\adb.exe'
    $pathAdb = Get-Command adb.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (Test-Path -LiteralPath $bundledAdb -PathType Leaf) { $AdbPath = $bundledAdb }
    elseif ($pathAdb) { $AdbPath = $pathAdb.Source }
    else {
        Write-Host 'Download and extract Windows platform-tools: https://developer.android.com/tools/releases/platform-tools'
        $AdbPath = (Read-Host 'Full path to adb.exe').Trim().Trim('"')
    }
}
if (!(Test-Path -LiteralPath $AdbPath -PathType Leaf)) { throw 'adb.exe not found. See README.txt.' }
$AdbPath = (Resolve-Path -LiteralPath $AdbPath).Path
function Invoke-Adb {
    param([string[]]$Arguments)
    # ADB prints normal daemon startup messages to stderr. Windows PowerShell
    # 5.1 must not interpret that as failure when the process exits successfully.
    $ErrorActionPreference = 'Continue'
    $output = @(& $AdbPath @Arguments 2>&1)
    $code = $LASTEXITCODE
    if ($code -ne 0) { throw "ADB failed (exit $code): $($output -join [Environment]::NewLine)" }
    $output | ForEach-Object { [string]$_ }
}
Invoke-Adb -Arguments @('start-server') | Out-Host
Write-Host "Using ADB: $AdbPath"
function Wait-QuestConnection {
param([string]$RequestedSerial)
$Serial = $RequestedSerial
Write-Host 'Keep the headset awake and connected. Look for Allow USB debugging (with a computer fingerprint).'
Write-Host 'Choose Always allow from this computer, then Allow. Accepting Quest Link or file access alone is not USB debugging.'
$deadline = [DateTime]::UtcNow.AddSeconds($ConnectionTimeoutSeconds)
$previousStatus = ''
$model = $null
$selectedSerial = $null
$listing = @()
do {
    $status = ''
    $candidates = @()
    try {
        $listing = @(Invoke-Adb -Arguments @('devices', '-l'))
        $devices = @(
            foreach ($line in $listing) {
                if ($line -match '^\s*(\S+)\s+(device|unauthorized|offline|authorizing|connecting|recovery|sideload|bootloader|no permissions)(?:\s|$)') {
                    [pscustomobject]@{ Serial = $Matches[1]; State = $Matches[2] }
                }
            }
        )
        $eligible = @($devices | Where-Object { $_.State -eq 'device' -and (!$Serial -or $_.Serial -eq $Serial) })
        foreach ($device in $eligible) {
            # Identify actual headsets, so an unrelated authorized phone does
            # not prevent installation. Never choose among several Quests.
            $detectedModel = (Invoke-Adb -Arguments @('-s', $device.Serial, 'shell', 'getprop', 'ro.product.model') | Out-String).Trim()
            if ($detectedModel -match '^Quest[ _]3(?:S)?$') {
                $candidates += [pscustomobject]@{ Serial = $device.Serial; Model = $detectedModel }
            }
        }
        if ($candidates.Count -eq 1) {
            $selectedSerial = $candidates[0].Serial
            $model = $candidates[0].Model
            break
        }
        if ($candidates.Count -gt 1) {
            $status = 'Multiple authorized Quests found: ' + (($candidates | ForEach-Object Serial) -join ', ') + '. Disconnect the extra headset/ADB connection, or rerun with -Serial followed by the desired serial.'
        } else {
            $relevant = @($devices | Where-Object { !$Serial -or $_.Serial -eq $Serial })
            if ($relevant.Count -eq 0) {
                $status = 'ADB cannot see the requested headset. Check Developer Mode, USB data cable, and the Windows Oculus ADB driver. Quest Link/file access can work even when the ADB interface is missing.'
            } elseif ($relevant.State -contains 'unauthorized' -or $relevant.State -contains 'authorizing') {
                $status = 'USB device detected, but this computer is not yet authorized for debugging. Accept Allow USB debugging inside the headset. Waiting for authorization to finish...'
            } elseif ($relevant.State -contains 'offline' -or $relevant.State -contains 'connecting') {
                $status = 'ADB sees the headset, but its debugging connection is offline. Keep it awake; reconnect the USB cable if it stays offline.'
            } else {
                $status = 'ADB sees connected devices, but no ready Quest 3/3S. Check the device list below and the selected serial.'
            }
        }
    } catch {
        # Authorization/reconnection can invalidate a transport between listing
        # it and reading its model. Re-enumerate rather than failing that race.
        $status = 'ADB connection changed or query failed; retrying. ' + $_.Exception.Message
    }
    if ($status -ne $previousStatus) {
        Write-Host $status
        $listing | Out-Host
        $previousStatus = $status
    }
    if ([DateTime]::UtcNow -ge $deadline) { break }
    Start-Sleep -Milliseconds 1000
} while ($true)
if (!$selectedSerial) {
    $diagnostic = @(
        'AC:VR installer connection diagnostic',
        "Time: $([DateTime]::Now.ToString('o'))",
        "ADB: $AdbPath",
        "Requested serial: $Serial",
        "Status: $previousStatus",
        'adb devices -l:',
        ($listing -join [Environment]::NewLine)
    )
    $diagnosticPath = Join-Path $PSScriptRoot 'Quest-Connection.txt'
    try {
        $diagnostic | Set-Content -LiteralPath $diagnosticPath -Encoding UTF8
        Write-Host "Connection details saved to $diagnosticPath. Send this text if it still fails."
    } catch {
        Write-Host 'Could not save the diagnostic file. Copy the device list displayed above.'
    }
    throw "No ready Quest selected after $ConnectionTimeoutSeconds seconds. $previousStatus Installation cannot continue."
}
return [pscustomobject]@{ Serial = $selectedSerial; Model = $model }
}
$connection = Wait-QuestConnection -RequestedSerial $Serial
$Serial = $connection.Serial
$model = $connection.Model
$deviceArgs = @('-s', $Serial)
Write-Host "Authorized $model detected: $Serial"
if ($CheckConnection) {
    Write-Host 'Connection check passed. No game files were installed or launched.'
    return
}
if ($SkyOnly) {
    Install-QuestSky $AC2SkyPath $PSScriptRoot
    return
}
$saved = 'files/ACUnreal/Saved/DAT'
$required = @('client_portal.dat', 'client_cell_1.dat', 'client_local_English.dat')
if (!$SkipData) {
    if (!$DatDirectory) {
        $defaultDat = "C:\Turbine\Asheron's Call"
        if (Test-Path -LiteralPath (Join-Path $defaultDat 'client_portal.dat') -PathType Leaf) {
            $DatDirectory = $defaultDat
        } else {
            $DatDirectory = (Read-Host 'Folder containing your Asherons Call DAT files').Trim().Trim('"')
        }
    }
    foreach ($name in $required) {
        $source = Join-Path $DatDirectory $name
        if (!(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-Item -LiteralPath $source).Length -eq 0) {
            throw "Missing or empty required data file: $source"
        }
    }
}

if (!$RepairData) {
Write-Host "Installing on $model. Keep the USB cable connected."
Write-Host 'Copying the APK first, then installing it. This may take a few minutes.'
$installErrors = @()
for ($attempt = 1; $attempt -le 2; $attempt++) {
    try {
        # Avoid the streamed PackageInstaller session seen failing on the
        # friend's PC. ADB uploads the APK, then asks Android to install it.
        Invoke-Adb -Arguments ($deviceArgs + @('install', '--no-streaming', '-r', $apk)) | Out-Host
        break
    } catch {
        $failure = $_.Exception.Message
        $installErrors += "Attempt ${attempt}: $failure"
        $diagnosticPath = Join-Path $PSScriptRoot 'Quest-Install.txt'
        try {
            @("AC:VR installer; bundle revision $($manifest.installerRevision); $([DateTime]::Now.ToString('o'))", "ADB: $AdbPath", "Device: $Serial", "APK: $apk") + $installErrors |
                Set-Content -LiteralPath $diagnosticPath -Encoding UTF8
            Write-Host "APK failure details saved to $diagnosticPath."
        } catch { Write-Host 'Could not save the APK diagnostic file.' }
        # Storage, package compatibility/signature, and outdated ADB errors need
        # action, not repeated installs. Never uninstall or erase data to retry.
        if ($attempt -eq 2 -or $failure -match 'INSTALL_FAILED|INSTALL_PARSE_FAILED|unknown option|unrecognized option|No space left|Permission denied') {
            throw "APK installation failed. $failure"
        }
        Write-Host $failure
        Write-Host 'Checking the same Quest connection before one retry. If it is offline, reconnect USB and keep the headset awake.'
        $connection = Wait-QuestConnection -RequestedSerial $Serial
    }
}
} else {
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'pwd')) | Out-Host
    Write-Host 'Repairing game data for the installed app. The APK will not be reinstalled.'
}
Move-QuestLegacyProfile
if ($SkipData) {
    foreach ($name in $required) {
        Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'test', '-s', "$saved/$name")) | Out-Host
    }
} else {
    Install-QuestData -DatDirectory $DatDirectory -DiagnosticDirectory $PSScriptRoot
}
if ($AC2SkyPath) { Install-QuestSky $AC2SkyPath $PSScriptRoot }
Write-Host 'Installation complete. Updating this way preserves saved logins and settings.'
Write-Host "Open $displayName from the Unknown Sources section of your app library."
Write-Host 'Enter the server address and your own account details supplied by the server owner.'
if (!$NoLaunch) {
    $power = Invoke-Adb -Arguments ($deviceArgs + @('shell', 'dumpsys', 'power')) | Out-String
    if ($power -match 'mWakefulness=Awake') {
        Invoke-Adb -Arguments ($deviceArgs + @('shell', 'am', 'start', '-W', '-n', "$package/com.epicgames.unreal.GameActivity")) | Out-Host
    } else { Write-Host 'The headset is asleep. Put it on before opening the app.' }
}
