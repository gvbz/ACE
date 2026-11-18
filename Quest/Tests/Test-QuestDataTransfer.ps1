#requires -Version 5.1
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. (Join-Path $root 'Sharing/Quest-DataTransfer.ps1')
$fixture = Join-Path $root ("Logs\DataTransferTests-" + (Get-Date -Format 'yyyyMMdd-HHmmss') + "\Friend's Quest")
$sourceDir = Join-Path $fixture 'DAT source'
$deviceDir = Join-Path $fixture 'device'
New-Item -ItemType Directory -Path $sourceDir,$deviceDir -Force | Out-Null
$oldPipeDir = $env:ACE_TEST_PIPE_DIR
$env:ACE_TEST_PIPE_DIR = $deviceDir
$AdbPath = Join-Path $fixture 'Fake Adb.exe'
# A real native subprocess exercises ADB push argument quoting and file bytes.
Add-Type -OutputAssembly $AdbPath -OutputType ConsoleApplication -TypeDefinition @'
using System;
using System.IO;
using System.Text.RegularExpressions;
public class FakeAdb {
 public static int Main(string[] args) {
  string root = Environment.GetEnvironmentVariable("ACE_TEST_PIPE_DIR");
  if (args.Length != 5 || args[0] != "-s" || args[1] != "QUEST_A" || args[2] != "push") return 8;
  if (!Regex.IsMatch(args[4], @"^/data/local/tmp/ace-dat-[0-9a-f]{32}\.tmp$")) return 9;
  string path = Path.Combine(root, Path.GetFileName(args[4]));
  string mode = File.Exists(Path.Combine(root,"mode")) ? File.ReadAllText(Path.Combine(root,"mode")).Trim() : "";
  if (mode == "early-exit") { Console.Error.WriteLine("Receiver closed before reading data"); return 0; }
  File.Copy(args[3], path, true);
  if (mode == "disconnect") { Console.Error.WriteLine("adb: error: device offline"); return 1; }
  if (mode == "corrupt") File.WriteAllText(path,"corrupted transfer");
  if (mode == "corrupt-once") { File.WriteAllText(path,"corrupted transfer"); File.WriteAllText(Path.Combine(root,"mode"),""); }
  if (mode == "storage") { Console.Error.WriteLine("No space left on device"); return 1; }
  return 0;
 }
}
'@
$package = 'com.acecommunity.questtest'
$Serial = 'QUEST_A'
$deviceArgs = @('-s',$Serial)
$script:commands = @()
$script:retries = 0
function Wait-QuestConnection { param([string]$RequestedSerial)
    if ($RequestedSerial -ne $Serial) { throw 'Retry changed headsets' }
    $script:retries++
    return [pscustomobject]@{Serial=$Serial;Model='Quest 3'}
}
function Invoke-Adb { param([string[]]$Arguments)
    $cmd = $Arguments -join ' '
    $script:commands += $cmd
    if ($Arguments.Count -eq 5 -and $Arguments[2] -eq 'push') {
        $output=@(& $AdbPath @Arguments 2>&1)
        if($LASTEXITCODE){throw "ADB push failed: $($output -join ' ')"}
        return
    }
    if ($cmd -match "shell run-as com.acecommunity.questtest sh -c 'cat > files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source)\.installing)' < /data/local/tmp/(ace-dat-[0-9a-f]{32}\.tmp)$") {
        [IO.File]::Copy((Join-Path $deviceDir $Matches[2]),(Join-Path $deviceDir $Matches[1]),$true)
        return
    }
    if ($cmd -match 'shell rm -f /data/local/tmp/(ace-dat-[0-9a-f]{32}\.tmp)$') {
        $tempPath=[IO.Path]::GetFullPath((Join-Path $deviceDir $Matches[1]))
        if(!$tempPath.StartsWith($deviceDir+'\')){throw 'Fixture path escaped'}
        if(Test-Path -LiteralPath $tempPath){Remove-Item -LiteralPath $tempPath -Force}
        return
    }
    if ($cmd -match 'shell run-as com.acecommunity.questtest rm -f files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source)\.installing)$') {
        [IO.File]::Delete((Join-Path $deviceDir $Matches[1]))
        return
    }
    if ($cmd -match 'shell am force-stop com.acecommunity.questtest$|shell run-as com.acecommunity.questtest mkdir -p files/ACUnreal/Saved/(?:DAT|Skies/Import)$') { return }
    if ($cmd -match 'if \[ -f files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source)) \]') {
        $path = Join-Path $deviceDir $Matches[1]
        if (Test-Path -LiteralPath $path) { return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $path }
        return
    }
    if ($cmd -match 'stat -c %s files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source)\.installing)$') {
        return [string](Get-Item -LiteralPath (Join-Path $deviceDir $Matches[1])).Length
    }
    if ($cmd -match 'sha256sum files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source)\.installing)$') {
        return (Get-FileHash -LiteralPath (Join-Path $deviceDir $Matches[1]) -Algorithm SHA256).Hash.ToLowerInvariant() + '  file'
    }
    if ($cmd -match 'mv files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source)\.installing) files/ACUnreal/Saved/(?:DAT|Skies/Import)/((?:client_\w+\.dat|Legions\.source))$') {
        # Test fixtures only; both resolved targets must remain in this device directory.
        $from = [IO.Path]::GetFullPath((Join-Path $deviceDir $Matches[1]))
        $to = [IO.Path]::GetFullPath((Join-Path $deviceDir $Matches[2]))
        if (!$from.StartsWith($deviceDir + '\') -or !$to.StartsWith($deviceDir + '\')) { throw 'Fixture path escaped' }
        [IO.File]::Copy($from,$to,$true)
        return
    }
    throw "Unexpected ADB command $cmd"
}
$bytes = New-Object byte[] (1024 * 1024 + 513)
$random = New-Object Random(27)
$random.NextBytes($bytes)
# Include all byte values, especially Ctrl-Z, CR/LF, NUL, and shell escape
# sequences. A binary .NET pipe alone is insufficient when ADB uses shell -T.
for ($i=0; $i -lt 256; $i++) { $bytes[$i]=[byte]$i }
foreach ($name in @('client_portal.dat','client_cell_1.dat','client_local_English.dat','client_highres.dat')) {
    [IO.File]::WriteAllBytes((Join-Path $sourceDir $name),$bytes)
}
try {
    Install-QuestData -DatDirectory $sourceDir -DiagnosticDirectory $fixture
    foreach ($name in @('client_portal.dat','client_cell_1.dat','client_local_English.dat','client_highres.dat')) {
        if ((Get-FileHash -LiteralPath (Join-Path $deviceDir $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $sourceDir $name)).Hash) { throw 'Binary stream changed bytes' }
    }
    Write-Output 'PASS binary file transfer through Windows PowerShell 5.1, including whitespace/apostrophe paths'
    $script:commands = @()
    Install-QuestData -DatDirectory $sourceDir -DiagnosticDirectory $fixture
    if (($script:commands -join '\n') -match '\bmv\b') { throw 'Replaced already verified data' }
    Write-Output 'PASS verified files are skipped'
    $target = Join-Path $deviceDir 'client_cell_1.dat'
    'old installed data' | Set-Content -LiteralPath $target
    'corrupt-once' | Set-Content -LiteralPath (Join-Path $deviceDir 'mode')
    Install-QuestData -DatDirectory $sourceDir -DiagnosticDirectory $fixture
    if ($script:retries -ne 1 -or (Get-FileHash -LiteralPath $target).Hash -ne (Get-FileHash -LiteralPath (Join-Path $sourceDir 'client_cell_1.dat')).Hash) { throw 'Retry did not repair data' }
    Write-Output 'PASS corrupt transfer retries on the same headset and repairs the failed cell DAT'
    foreach ($mode in @('corrupt','disconnect','storage','early-exit')) {
        'old installed data' | Set-Content -LiteralPath $target
        $before = (Get-FileHash -LiteralPath $target).Hash
        $mode | Set-Content -LiteralPath (Join-Path $deviceDir 'mode')
        $script:retries = 0
        $failed = $false
        try { Install-QuestData -DatDirectory $sourceDir -DiagnosticDirectory $fixture } catch { $failed = $_.Exception.Message -match 'Game data installation did not complete' }
        if (!$failed -or (Get-FileHash -LiteralPath $target).Hash -ne $before) { throw "Failure mode $mode replaced an unverified file or claimed success" }
        $expectedRetries = if ($mode -eq 'storage') { 0 } else { 2 }
        if ($script:retries -ne $expectedRetries) { throw "Unexpected retries for $mode" }
        if ($mode -eq 'early-exit') {
            $diagnostic=Get-Content -LiteralPath (Join-Path $fixture 'Quest-Data.txt') -Raw
            if ($diagnostic -match 'Exception calling "Wait"|One or more errors occurred') { throw 'Transfer diagnostic hid the underlying pipe error' }
        }
        Write-Output "PASS $mode preserves installed files and reports the failure"
    }
    if (@(Get-ChildItem -LiteralPath $deviceDir -Filter 'ace-dat-*.tmp').Count) { throw 'Staging file leaked' }
    '' | Set-Content -LiteralPath (Join-Path $deviceDir 'mode')
    $sky = Join-Path $sourceDir 'Legions.sky'
    foreach ($header in @('LSK1','LSK2','LSK3')) {
        [Text.Encoding]::ASCII.GetBytes($header).CopyTo($bytes,0)
        [IO.File]::WriteAllBytes($sky,$bytes)
        if ((Resolve-QuestSkySource $sky) -ne $sky) { throw "Rejected $header sky cache" }
    }
    $selected = Resolve-QuestSkySource $sky
    Install-QuestSky $selected $fixture
    $pending = Join-Path $deviceDir 'Legions.source'
    if ((Get-FileHash $sky).Hash -ne (Get-FileHash $pending).Hash) { throw 'Wrong sky staged' }
    $script:commands = @()
    Install-QuestSky $selected $fixture
    if (($script:commands -join ' ') -match '\bpush\b|\bmv\b') { throw 'Duplicate sky replaced' }
    $portal = Join-Path $sourceDir 'portal.dat'
    [BitConverter]::GetBytes([uint32]0x5442).CopyTo($bytes,300)
    [BitConverter]::GetBytes([uint32]1024).CopyTo($bytes,304)
    [BitConverter]::GetBytes([uint32]$bytes.Length).CopyTo($bytes,308)
    [BitConverter]::GetBytes([uint32]1).CopyTo($bytes,312)
    [IO.File]::WriteAllBytes($portal,$bytes)
    if ((Resolve-QuestSkySource $sourceDir) -ne $portal) { throw 'AC2 folder selection failed' }
    'storage' | Set-Content -LiteralPath (Join-Path $deviceDir 'mode')
    $failed = $false
    try { Install-QuestSky (Resolve-QuestSkySource $portal) $fixture } catch { $failed = $_.Exception.Message -match 'Install-AC2-Sky.cmd' }
    if (!$failed -or (Get-FileHash $sky).Hash -ne (Get-FileHash $pending).Hash) { throw 'Failed replacement lost pending sky' }
    $failed = $false
    try { Resolve-QuestSkySource (Join-Path $sourceDir 'client_portal.dat') } catch { $failed = $true }
    if (!$failed) { throw 'Sky picker accepted an AC1 database' }
    Write-Output "PASS sky staging, repeat transfer, failed replacement, and source validation. Evidence: $fixture"
} finally { $env:ACE_TEST_PIPE_DIR = $oldPipeDir }
