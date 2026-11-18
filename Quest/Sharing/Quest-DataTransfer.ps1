#requires -Version 5.1
# Shared by the developer and friend installers. Invoke-Adb, Wait-QuestConnection,
# $AdbPath, $deviceArgs, $Serial, and $package are supplied by the installer.
function Send-QuestDataFile {
    param([string]$Source, [string]$Destination)
    # Use ADB's file-sync protocol, not Windows stdin. shell -T truncates at
    # Ctrl-Z and exec-in can terminate the ADB server after large transfers.
    # Stage outside shared storage (which Android may remove during app setup).
    $staging='/data/local/tmp/ace-dat-'+[guid]::NewGuid().ToString('N')+'.tmp'
    try {
        Invoke-Adb -Arguments ($deviceArgs + @('push', $Source, $staging)) | Out-Host
        # The outer shell opens the shell-owned staging file before run-as
        # changes identity. The destination is created with app ownership.
        $receive="run-as $package sh -c 'cat > $Destination' < $staging"
        Invoke-Adb -Arguments ($deviceArgs + @('shell', $receive)) | Out-Host
    } finally {
        try { Invoke-Adb -Arguments ($deviceArgs + @('shell','rm','-f',$staging)) | Out-Host }
        catch { Write-Warning "Could not remove temporary transfer file $staging. $($_.Exception.Message)" }
    }
}
function Install-QuestData {
    param([string]$DatDirectory, [string]$DiagnosticDirectory)
    $saved = 'files/ACUnreal/Saved/DAT'
    $required = @('client_portal.dat', 'client_cell_1.dat', 'client_local_English.dat')
    foreach ($name in $required) {
        $source = Join-Path $DatDirectory $name
        if (!(Test-Path -LiteralPath $source -PathType Leaf) -or (Get-Item -LiteralPath $source).Length -eq 0) {
            throw "Missing or empty required data file: $source"
        }
    }
    # Close the game before replacing databases it may have open. This preserves
    # settings and logins, and the installer only relaunches after verification.
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'am', 'force-stop', $package)) | Out-Host
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'mkdir', '-p', $saved)) | Out-Host
    foreach ($name in ($required + @('client_highres.dat'))) {
        $source = Join-Path $DatDirectory $name
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        Install-QuestFile $source $saved $name $DiagnosticDirectory 'Repair-Quest.cmd'
    }
}
function Resolve-QuestSkySource {
    param([string]$Path)
    $Path = $Path.Trim().Trim('"')
    if (Test-Path -LiteralPath $Path -PathType Container) { $Path = Join-Path $Path 'portal.dat' }
    $file = Get-Item -LiteralPath $Path -ErrorAction Stop
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($file.FullName))
    try {
        if ($file.Extension -ieq '.sky' -and $file.Length -ge 12 -and $file.Length -le (40MB + 2048)) {
            if ($reader.ReadUInt32() -in @(0x314b534c, 0x324b534c, 0x334b534c)) { return $file.FullName }
        } elseif ($file.Name -ieq 'portal.dat' -and $file.Length -ge 380) {
            $reader.BaseStream.Position = 300
            $magic = $reader.ReadUInt32()
            $sector = $reader.ReadUInt32()
            $length = $reader.ReadUInt32()
            $type = $reader.ReadUInt32()
            if ($magic -eq 0x5442 -and $length -eq $file.Length -and $type -eq 1 -and
                $sector -ge 256 -and $sector -le 65536 -and ($sector -band ($sector-1)) -eq 0) { return $file.FullName }
        }
        throw "Choose AC2 Legions portal.dat or an imported Legions.sky. Unrecognized file: $($file.Name)"
    } finally { $reader.Dispose() }
}

function Install-QuestSky {
    param([string]$Source, [string]$DiagnosticDirectory)
    $folder = 'files/ACUnreal/Saved/Skies/Import'
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'am', 'force-stop', $package)) | Out-Host
    Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'mkdir', '-p', $folder)) | Out-Host
    Install-QuestFile $Source $folder 'Legions.source' $DiagnosticDirectory 'Install-AC2-Sky.cmd'
    Write-Host 'Transfer verified. In the headset, open Options > VR settings > Graphics and choose Import and use AC2 sky.'
}

function Install-QuestFile {
    param([string]$Source, [string]$Saved, [string]$Name, [string]$DiagnosticDirectory, [string]$RecoveryCommand)
    $localSize = [string](Get-Item -LiteralPath $source).Length
    $localHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant()
    $partial = "$saved/$name.installing"
    $failures = @()
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            $probe = "run-as $package sh -c 'if [ -f $saved/$name ]; then sha256sum $saved/$name; fi'"
            $existing = (Invoke-Adb -Arguments ($deviceArgs + @('shell', $probe)) | Out-String).Trim()
            if ($existing -and ($existing -split '\s+')[0] -eq $localHash) {
                Write-Host "$name is already installed and verified."
                break
            }
            Write-Host "Transferring $name into app storage (attempt $attempt of 3)..."
            # Only constant paths and allowlisted basenames enter Android's
            # shell. Never truncate the installed DAT on a failed transfer.
            Send-QuestDataFile -Source $source -Destination $partial
            $size = (Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'stat', '-c', '%s', $partial)) | Out-String).Trim()
            $hash = ((Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'sha256sum', $partial)) | Out-String).Trim() -split '\s+')[0]
            if ($size -ne $localSize -or $hash -ne $localHash) {
                throw "Data verification failed: $name. Received $size of $localSize bytes; SHA256 did not match."
            }
            Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'mv', $partial, "$saved/$name")) | Out-Host
            Write-Host "Verified and installed $name."
            break
        } catch {
            $failure = $_.Exception.Message
            $failures += "File: $name; attempt ${attempt}: $failure"
            $diagnosticPath = Join-Path $DiagnosticDirectory 'Quest-Data.txt'
            try {
                @("AC:VR data repair; $([DateTime]::Now.ToString('o'))", "Device: $Serial") + $failures |
                    Set-Content -LiteralPath $diagnosticPath -Encoding UTF8
            } catch { Write-Host 'Could not save the data diagnostic file.' }
            if ($attempt -eq 3 -or $failure -match 'No space left|Permission denied|not debuggable') {
                try { Invoke-Adb -Arguments ($deviceArgs + @('shell', 'run-as', $package, 'rm', '-f', $partial)) | Out-Host }
                catch { Write-Warning "Reconnect the headset and retry to replace any partial transfer." }
                throw "Game data installation did not complete. $failure See Quest-Data.txt. Run $RecoveryCommand after correcting the problem."
            }
            Write-Host $failure
            Write-Host 'Checking the same Quest before retrying this file. Previously verified files are preserved.'
            $connection = Wait-QuestConnection -RequestedSerial $Serial
        }
    }
}
