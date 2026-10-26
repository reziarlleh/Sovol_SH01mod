[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidatePattern('^COM\d+$')]
    [string]$ComPort,

    [string]$HexFile = '',

    [ValidateSet(9600, 19200, 38400, 115200, 230400, 460800, 691200)]
    [int]$BaudRate = 115200,

    [switch]$ChipErase,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-HexDataChecksum {
    param([Parameter(Mandatory = $true)][string[]]$Records)

    # check_hex_storage.ps1 must first validate syntax, record checksums,
    # record lengths, EOF and the application/storage boundary.
    $addresses = New-Object 'System.Collections.Generic.HashSet[long]'
    [int64]$upperAddress = 0
    [int]$checksum = 0
    foreach ($rawRecord in $Records) {
        $record = $rawRecord.Trim()
        if ($record.Length -eq 0) { continue }
        $length = [Convert]::ToInt32($record.Substring(1, 2), 16)
        $address = [Convert]::ToInt32($record.Substring(3, 4), 16)
        $type = [Convert]::ToInt32($record.Substring(7, 2), 16)
        switch ($type) {
            0 {
                for ($offset = 0; $offset -lt $length; $offset++) {
                    $absoluteAddress = $upperAddress + $address + $offset
                    if (-not $addresses.Add($absoluteAddress)) {
                        throw ('Overlapping HEX data address 0x{0:X8}; checksum verification is ambiguous.' -f $absoluteAddress)
                    }
                    $value = [Convert]::ToInt32($record.Substring(9 + 2 * $offset, 2), 16)
                    $checksum = ($checksum + $value) -band 0xFFFF
                }
            }
            1 { }
            2 { $upperAddress = [int64][Convert]::ToInt32($record.Substring(9, 4), 16) * 0x10 }
            3 { }
            4 { $upperAddress = [int64][Convert]::ToInt32($record.Substring(9, 4), 16) * 0x10000 }
            5 { }
            default { throw "Unsupported HEX record type $type; checksum verification is unavailable." }
        }
    }
    if ($addresses.Count -eq 0) { throw 'HEX contains no programmed bytes.' }
    return [pscustomobject]@{ Checksum16 = $checksum; ByteCount = $addresses.Count }
}

function Assert-XhscProgramLog {
    param(
        [Parameter(Mandatory = $true)][string]$LogText,
        [Parameter(Mandatory = $true)][ValidateRange(0, 65535)][int]$ExpectedChecksum16
    )

    # ASCII-only regex source avoids Windows PowerShell source-encoding drift.
    # XHSC V2.23 reports Chinese Program success and Verify match messages.
    if ($LogText -match '\u4e0d\u5339\u914d|\u5931\u8d25|\u9519\u8bef') {
        throw 'XHSC log contains a mismatch/failure/error message.'
    }
    $programMatches = [regex]::Matches($LogText, '(?m)^<--[^\r\n]+-->\u7f16\u7a0b\u6210\u529f\uff01\r?$')
    $verifyMatches = [regex]::Matches($LogText, '(?m)^<--[^\r\n]+-->\u9a8c\u8bc1\u6210\u529f\uff0c\u9a8c\u8bc1\u7ed3\u679c\u662f\uff1a\u5339\u914d\(0x(?<Checksum>[0-9A-Fa-f]{1,4})\)\r?$')
    if ($programMatches.Count -ne 1 -or $verifyMatches.Count -ne 1) {
        throw 'XHSC log must contain exactly one complete Program success and Verify match result.'
    }
    if ($verifyMatches[0].Index -le $programMatches[0].Index) {
        throw 'XHSC Verify result precedes Program success.'
    }
    $actualChecksum = [Convert]::ToInt32($verifyMatches[0].Groups['Checksum'].Value, 16)
    if ($actualChecksum -ne $ExpectedChecksum16) {
        throw ('XHSC Verify checksum 0x{0:X4} differs from HEX checksum 0x{1:X4}.' -f $actualChecksum, $ExpectedChecksum16)
    }
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$configText = [IO.File]::ReadAllText((Join-Path $projectRoot 'src/config.h'))
$versionMatch = [regex]::Match($configText, '#define FW_VERSION "(\d+\.\d+\.\d+)"')
if (-not $versionMatch.Success) { throw 'Firmware version missing from src/config.h' }
$version = $versionMatch.Groups[1].Value
$defaultHex = Join-Path $projectRoot ('build\v' + $version + '\sovol_sh01mod_v' + $version.Replace('.', '_') + '.hex')
if ([string]::IsNullOrWhiteSpace($HexFile)) {
    $HexFile = $defaultHex
}
$hexPath = [IO.Path]::GetFullPath($HexFile)

$programmerDir = Join-Path $projectRoot 'downloads\XHSC_ISP_V2.23\XHSC ISP'
$programmer = Join-Path $programmerDir 'XHSC.exe'
$checker = Join-Path $projectRoot 'tools\check_hex_storage.ps1'

foreach ($required in @($hexPath, $programmer, $checker)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}

Write-Host "Checking HEX: $hexPath"
$powershell = Get-Command powershell.exe -ErrorAction SilentlyContinue
if ($null -eq $powershell) {
    throw 'Windows PowerShell (powershell.exe) is required for HEX validation.'
}
& $powershell.Source -NoProfile -ExecutionPolicy Bypass -File $checker -HexFile $hexPath
if ($LASTEXITCODE -ne 0) {
    throw "HEX validation failed with exit code $LASTEXITCODE"
}
$hexData = Get-HexDataChecksum -Records ([IO.File]::ReadAllLines($hexPath))
Write-Host ('HEX data: {0} unique bytes; expected vendor checksum16: 0x{1:X4}' -f $hexData.ByteCount, $hexData.Checksum16)

# These indices belong to this exact XHSC V2.23 binary. Its alphabetically
# sorted family list is different from Programmer Config Tool2 v1.309.
# The profile was checked against XHSC.exe's own managed code and family table;
# it identifies the software configuration, not the MCU connected to the port.
$profile = [pscustomobject]@{
    ProgrammerSha256 = 'c20ca3d4c321e78d2d8059281e2f133821cab0627aaa39ad2021389f3941ff30'
    Family = 'HC32L110x6xx/HC32F005x6xx'
    McuIndex = 33
    BaudRates = [int[]]@(691200, 460800, 230400, 115200, 38400, 19200, 9600)
    PageSize = 512
    FlashSize = 32768
    RamSize = 4096
}
$programmerHash = (Get-FileHash -LiteralPath $programmer -Algorithm SHA256).Hash.ToLowerInvariant()
if ($programmerHash -ne $profile.ProgrammerSha256) {
    throw "Unsupported XHSC.exe SHA-256: $programmerHash. The pinned HC32F005 profile must be revalidated before using another binary."
}
$targetIndex = $profile.McuIndex
$baudIndex = [Array]::IndexOf($profile.BaudRates, $BaudRate)
if ($baudIndex -lt 0) {
    throw "Baud rate $BaudRate is not supported for HC32F005C6."
}

$normalizedPort = $ComPort.ToUpperInvariant()
if (-not $DryRun) {
    $availablePorts = @([IO.Ports.SerialPort]::GetPortNames())
    if (-not ($availablePorts | Where-Object { $_.ToUpperInvariant() -eq $normalizedPort })) {
        $listed = if ($availablePorts.Count -eq 0) { '<none>' } else { $availablePorts -join ', ' }
        throw "Port $normalizedPort is not present. Available ports: $listed"
    }
}

$operations = if ($ChipErase) { @('CE', 'B', 'P') } else { @('PE', 'B', 'P') }
$operationText = $operations -join ' '
Write-Host "Pinned family: $($profile.Family), MCU index: $targetIndex, baud index: $baudIndex, baud: $BaudRate, port: $normalizedPort"
Write-Host "Profile: Flash $($profile.FlashSize) bytes, page $($profile.PageSize) bytes, RAM $($profile.RamSize) bytes"
Write-Warning 'This profile does not confirm the connected hardware. Hardware/power levels must match this board; the original factory backup must already be preserved.'
Write-Host "Operations: $operationText"
if ($ChipErase) {
    Write-Warning 'Chip erase removes the factory firmware and the settings journal. Factory backup must already exist.'
}

$sessionsRoot = Join-Path $projectRoot 'hardware-local\isp-sessions'
$sessionName = (Get-Date -Format 'yyyyMMdd-HHmmss') + '_' + [Guid]::NewGuid().ToString('N')
$sessionRoot = Join-Path $sessionsRoot $sessionName
$sessionRecord = [ordered]@{
    StartedUtc = [DateTime]::UtcNow.ToString('o')
    DryRun = [bool]$DryRun
    Profile = $profile
    ComPort = $normalizedPort
    BaudRate = $BaudRate
    BaudIndex = $baudIndex
    HexFile = $hexPath
    HexSha256 = (Get-FileHash -LiteralPath $hexPath -Algorithm SHA256).Hash.ToLowerInvariant()
    HexDataByteCount = $hexData.ByteCount
    ExpectedChecksum16 = ('0x{0:X4}' -f $hexData.Checksum16)
    Operations = $operations
    Status = 'Preparing'
    ExitCode = $null
    Error = $null
}
New-Item -ItemType Directory -Force -Path $sessionRoot | Out-Null
Write-Host "Session files and programmer logs are retained: $sessionRoot"
try {
    Copy-Item -LiteralPath $programmerDir -Destination $sessionRoot -Recurse
    $workDir = Join-Path $sessionRoot 'XHSC ISP'
    $tempProgrammer = Join-Path $workDir 'XHSC.exe'
    $tempConfigPath = Join-Path $workDir 'Config.xml'
    if ((Get-FileHash -LiteralPath $tempProgrammer -Algorithm SHA256).Hash.ToLowerInvariant() -ne $profile.ProgrammerSha256) {
        throw 'The session copy of XHSC.exe does not match the pinned binary.'
    }

    $configXml = [xml](Get-Content -LiteralPath $tempConfigPath -Raw)
    $configXml.SystemConfig.TargetMCU.Index = [string]$targetIndex
    $configXml.SystemConfig.CrystalIndex = [string]$baudIndex
    $configXml.SystemConfig.HexFile = $hexPath
    $configXml.SystemConfig.Com = $normalizedPort
    $configXml.SystemConfig.ChipErase = if ($ChipErase) { 'true' } else { 'false' }
    $configXml.SystemConfig.Erase = 'true'
    $configXml.SystemConfig.BlankCheck = 'true'
    $configXml.SystemConfig.Program = 'true'
    $configXml.SystemConfig.Verify = 'true'
    $configXml.SystemConfig.Encrypt = 'false'
    $configXml.SystemConfig.Encrypt2 = 'false'
    $configXml.Save($tempConfigPath)

    if ($DryRun) {
        $sessionRecord.Status = 'DryRunOnly'
        Write-Host 'Dry run: XHSC.exe was not started.'
        Write-Host "Command: `"$tempProgrammer`" $operationText"
        Write-Host "Prepared configuration is retained: $tempConfigPath"
        return
    }

    $logPath = Join-Path $workDir 'log.txt'
    if (Test-Path -LiteralPath $logPath) {
        Move-Item -LiteralPath $logPath -Destination (Join-Path $workDir 'preserved-source-log.txt')
    }
    # After moving any inherited log aside, only a newly written log can pass.
    $programStartedUtc = [DateTime]::UtcNow
    Write-Host 'Starting XHSC ISP. Keep the board powered at 3.3 V only and wait for completion.'
    $sessionRecord.Status = 'Running'
    $process = Start-Process -FilePath $tempProgrammer -WorkingDirectory $workDir -ArgumentList $operations -WindowStyle Hidden -Wait -PassThru
    $sessionRecord.ExitCode = $process.ExitCode
    if ($process.ExitCode -ne 0) {
        throw "XHSC ISP failed with exit code $($process.ExitCode). Session retained: $sessionRoot"
    }
    if (-not (Test-Path -LiteralPath $logPath -PathType Leaf)) {
        throw 'XHSC exited with code 0 but did not create a fresh programmer log.'
    }
    if ((Get-Item -LiteralPath $logPath).LastWriteTimeUtc -lt $programStartedUtc) {
        throw 'XHSC programmer log predates this invocation; verification is not accepted.'
    }
    $strictUtf8 = New-Object System.Text.UTF8Encoding($false, $true)
    $logText = [IO.File]::ReadAllText($logPath, $strictUtf8)
    Assert-XhscProgramLog -LogText $logText -ExpectedChecksum16 $hexData.Checksum16
    $sessionRecord.Status = 'VendorChecksumVerified'
    Write-Host ('XHSC Program and vendor checksum16 Verify succeeded: 0x{0:X4}.' -f $hexData.Checksum16)
    Write-Warning 'Vendor checksum16 verification is not a full byte-for-byte Flash readback. Keep the retained logs and perform an independent full readback comparison.'
}
catch {
    $sessionRecord.Status = 'Failed'
    $sessionRecord.Error = $_.Exception.Message
    throw
}
finally {
    $sessionRecord['FinishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $sessionRecord | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $sessionRoot 'session.json') -Encoding UTF8
    Write-Host "Session preserved: $sessionRoot"
}
