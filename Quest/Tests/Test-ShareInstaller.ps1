#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$workspace = Join-Path $root ("Logs\ShareInstallerTests-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + "\Friend's Quest")
New-Item -ItemType Directory -Path $workspace -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'Sharing\Install-Quest.ps1') -Destination $workspace
Copy-Item -LiteralPath (Join-Path $root 'Sharing\Quest-DataTransfer.ps1') -Destination $workspace
Copy-Item -LiteralPath (Join-Path $root 'Sharing\Quest-ProfileMigration.ps1') -Destination $workspace
$fixtureDat = Join-Path $workspace 'DAT source'
New-Item -ItemType Directory -Path $fixtureDat -Force | Out-Null
foreach ($name in @('client_portal.dat','client_cell_1.dat','client_local_English.dat')) {
    'test binary data fixture' | Set-Content -LiteralPath (Join-Path $fixtureDat $name)
}
$fixtureApk = Join-Path $workspace 'AC-VR-arm64.apk'
'Test-only fixture, never installed on an Android device.' | Set-Content -LiteralPath $fixtureApk -Encoding ASCII
@{
    package='com.acecommunity.questtest'; version='test'; apk='AC-VR-arm64.apk'
    apkBytes=(Get-Item -LiteralPath $fixtureApk).Length
    apkSha256=(Get-FileHash -LiteralPath $fixtureApk -Algorithm SHA256).Hash
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $workspace 'manifest.json') -Encoding UTF8
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Fake-Adb.ps1" %*
exit /b %ERRORLEVEL%
'@ | Set-Content -LiteralPath (Join-Path $workspace 'Fake-Adb.cmd') -Encoding ASCII
@'
$scenario = (Get-Content -LiteralPath (Join-Path $PSScriptRoot 'scenario.txt') -Raw).Trim()
($args -join ' ') | Add-Content -LiteralPath (Join-Path $PSScriptRoot 'commands.txt')
if ($args[0] -eq 'start-server') {
    [Console]::Error.WriteLine('* daemon started successfully *')
    exit 0
}
if ($args[0] -eq 'devices') {
    $counterPath = Join-Path $PSScriptRoot 'enumerations.txt'
    $count = [int](Get-Content -LiteralPath $counterPath -Raw) + 1
    $count | Set-Content -LiteralPath $counterPath
    Write-Output 'List of devices attached'
    if ($scenario -eq 'none') { exit 0 }
    if ($scenario -eq 'offline' -or ($scenario -eq 'offline-ready' -and $count -eq 1)) {
        Write-Output "QUEST_A`toffline transport_id:1"; exit 0
    }
    if ($scenario -eq 'unauthorized' -or ($scenario -eq 'authorize-ready' -and $count -eq 1)) {
        Write-Output "QUEST_A`tunauthorized usb:1-2"; exit 0
    }
    Write-Output "QUEST_A`tdevice product:eureka model:Quest_3 device:eureka transport_id:1  "
    if ($scenario -eq 'phone') { Write-Output "PHONE`tdevice model:Pixel_9 transport_id:2" }
    if ($scenario -eq 'multiple') { Write-Output "QUEST_B`tdevice model:Quest_3 transport_id:3" }
    exit 0
}
if ($args[2] -eq 'install') {
    $counterPath = Join-Path $PSScriptRoot 'installs.txt'
    $count = [int](Get-Content -LiteralPath $counterPath -Raw) + 1
    $count | Set-Content -LiteralPath $counterPath
    if ($args -notcontains '--no-streaming') { [Console]::Error.WriteLine('Expected staged install'); exit 9 }
    if ($scenario -eq 'install-storage') {
        [Console]::Error.WriteLine('Failure [INSTALL_FAILED_INSUFFICIENT_STORAGE]'); exit 1
    }
    if ($scenario -eq 'install-disconnect' -and $count -eq 1) {
        [Console]::Error.WriteLine('adb: error: device offline'); exit 1
    }
    Write-Output 'Success'; exit 0
}
if ($args -contains 'getprop') {
    if ($scenario -eq 'model-race' -and [int](Get-Content -LiteralPath (Join-Path $PSScriptRoot 'enumerations.txt') -Raw) -eq 1) {
        [Console]::Error.WriteLine('adb: error: device offline'); exit 1
    }
    if ($args[1] -eq 'PHONE') { Write-Output 'Pixel 9' } else { Write-Output 'Quest 3' }
    exit 0
}
# Only the update preflight may ask about existing data in these fixtures.
if (($args -join ' ') -match 'shell run-as com.acecommunity.questtest test -s files/ACUnreal/Saved/DAT/') { exit 0 }
$cmd = $args -join ' '
if ($scenario -eq 'sky-only') {
    if ($cmd -match 'shell am force-stop com.acecommunity.questtest$|shell run-as com.acecommunity.questtest mkdir -p files/ACUnreal/Saved/Skies/Import$') { exit 0 }
    if ($cmd -match 'if \[ -f files/ACUnreal/Saved/Skies/Import/Legions.source \]') {
        Write-Output ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Legions.sky')).Hash.ToLowerInvariant() + '  file')
        exit 0
    }
}
if ($cmd -match 'if \[ -d files/ACEViewer \]; then echo legacy; fi') {
    if ($scenario -eq 'legacy-profile') { Write-Output 'legacy' }
    exit 0
}
if ($scenario -eq 'legacy-profile') {
    if ($cmd -match 'shell am force-stop com.acecommunity.questtest$') { exit 0 }
    if ($cmd -match 'old=files/ACEViewer' -and $cmd -match 'new=files/ACUnreal') { exit 0 }
}
if ($scenario -eq 'data-repair') {
    if ($cmd -match 'shell run-as com.acecommunity.questtest pwd$') { Write-Output '/data/user/0/com.acecommunity.questtest'; exit 0 }
    if ($cmd -match 'shell am force-stop com.acecommunity.questtest$|shell run-as com.acecommunity.questtest mkdir -p files/ACUnreal/Saved/DAT$') { exit 0 }
    if ($cmd -match 'if \[ -f files/ACUnreal/Saved/DAT/(client_\w+\.dat) \]') {
        Write-Output ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot ('DAT source/' + $Matches[1])) -Algorithm SHA256).Hash.ToLowerInvariant() + '  file')
        exit 0
    }
}
[Console]::Error.WriteLine('Unexpected fake ADB command: ' + ($args -join ' '))
exit 8
'@ | Set-Content -LiteralPath (Join-Path $workspace 'Fake-Adb.ps1') -Encoding ASCII
function Test-Case {
    param([string]$Scenario, [string]$Expected, [int]$ExitCode=0, [string[]]$Options=@(), [int]$ExpectedInstalls=0)
    $Scenario | Set-Content -LiteralPath (Join-Path $workspace 'scenario.txt')
    '0' | Set-Content -LiteralPath (Join-Path $workspace 'enumerations.txt')
    '0' | Set-Content -LiteralPath (Join-Path $workspace 'installs.txt')
    '' | Set-Content -LiteralPath (Join-Path $workspace 'commands.txt')
    $invokeArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $workspace 'Install-Quest.ps1'), '-AdbPath', (Join-Path $workspace 'Fake-Adb.cmd'), '-NoLaunch') + $Options
    $ErrorActionPreference = 'Continue'
    $output = & powershell.exe @invokeArgs 2>&1 | Out-String
    $code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    $logName = ($Scenario + '-' + ($Options -join '_') + '.log') -replace '[<>:"/\\|?*]', '_'
    $output | Set-Content -LiteralPath (Join-Path $workspace $logName)
    if ($code -ne $ExitCode -or $output -notmatch $Expected) { throw "Case $Scenario failed: exit=$code; expected=$Expected`n$output" }
    $installs = [int](Get-Content -LiteralPath (Join-Path $workspace 'installs.txt') -Raw)
    if ($installs -ne $ExpectedInstalls) { throw "Case $Scenario attempted $installs installs; expected $ExpectedInstalls" }
    $commands = Get-Content -LiteralPath (Join-Path $workspace 'commands.txt') -Raw
    if ($commands -match '\buninstall\b|\bkill-server\b|\bclear\b' -or ($Scenario -notin @('data-repair','legacy-profile','sky-only') -and $commands -match '\bforce-stop\b')) { throw "Case $Scenario used a destructive or unrelated command" }
    Write-Output "PASS $Scenario $($Options -join ' ')"
}
Test-Case 'none' 'cannot see the requested headset' 1 @('-CheckConnection','-ConnectionTimeoutSeconds','0')
Test-Case 'unauthorized' 'not yet authorized' 1 @('-CheckConnection','-ConnectionTimeoutSeconds','0')
Test-Case 'offline' 'connection is offline' 1 @('-CheckConnection','-ConnectionTimeoutSeconds','0')
Test-Case 'authorize-ready' 'Connection check passed' 0 @('-CheckConnection','-ConnectionTimeoutSeconds','10')
Test-Case 'offline-ready' 'Connection check passed' 0 @('-CheckConnection','-ConnectionTimeoutSeconds','10')
Test-Case 'phone' 'Authorized Quest 3 detected: QUEST_A' 0 @('-CheckConnection','-ConnectionTimeoutSeconds','0')
Test-Case 'multiple' 'Multiple authorized Quests found' 1 @('-CheckConnection','-ConnectionTimeoutSeconds','0')
Test-Case 'multiple' 'Authorized Quest 3 detected: QUEST_B' 0 @('-CheckConnection','-ConnectionTimeoutSeconds','0','-Serial','QUEST_B')
Test-Case 'phone' 'cannot see the requested headset' 1 @('-CheckConnection','-ConnectionTimeoutSeconds','0','-Serial','MISSING')
Test-Case 'model-race' 'Connection check passed' 0 @('-CheckConnection','-ConnectionTimeoutSeconds','10')
Test-Case 'install-disconnect' 'Installation complete' 0 @('-SkipData','-ConnectionTimeoutSeconds','10') 2
Test-Case 'install-storage' 'INSTALL_FAILED_INSUFFICIENT_STORAGE' 1 @('-SkipData','-ConnectionTimeoutSeconds','10') 1
Test-Case 'data-repair' 'Installation complete' 0 @('-RepairData','-DatDirectory',$fixtureDat,'-ConnectionTimeoutSeconds','0') 0
Test-Case 'invalid-options' 'cannot be combined' 1 @('-RepairData','-SkipData','-ConnectionTimeoutSeconds','0') 0
Test-Case 'legacy-profile' 'Existing game data, login and settings migrated' 0 @('-SkipData','-ConnectionTimeoutSeconds','0') 1
[IO.File]::WriteAllBytes((Join-Path $workspace 'Legions.sky'),[Text.Encoding]::ASCII.GetBytes('LSK2fixture!'))
'deliberately stale fixture checksum' | Set-Content -LiteralPath $fixtureApk
Test-Case 'sky-only' 'Transfer verified' 0 @('-SkyOnly','-AC2SkyPath',(Join-Path $workspace 'Legions.sky'),'-ConnectionTimeoutSeconds','0')
Write-Output "All 16 installer cases passed under Windows PowerShell. Evidence: $workspace"
