[CmdletBinding()]
param([switch]$IncludeProgrammer, [switch]$ProgrammerOnly, [switch]$VerifyOnly)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$downloads = Join-Path $root 'downloads'
New-Item -ItemType Directory -Force -Path $downloads | Out-Null

function Get-Package([string]$Name, [long]$Size, [string]$Hash, [string]$Url) {
    $path = Join-Path $downloads $Name
    if (-not (Test-Path -LiteralPath $path)) {
        if ($VerifyOnly) { throw "Missing package: $path" }
        Write-Host "Downloading $Name"
        Add-Type -AssemblyName System.Net.Http
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        $client = New-Object Net.Http.HttpClient
        $client.Timeout = [TimeSpan]::FromMinutes(30)
        try {
            $response = $client.GetAsync($Url, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
            $response.EnsureSuccessStatusCode() | Out-Null
            $inputStream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
            $outputStream = [IO.File]::Create($path + '.partial')
            try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
            Move-Item -LiteralPath ($path + '.partial') -Destination $path
        } finally { $client.Dispose() }
    }
    if ((Get-Item -LiteralPath $path).Length -ne $Size -or
        (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Hash) {
        throw "Package size/SHA256 mismatch: $path"
    }
    Write-Host "Verified: $Name"
    return $path
}

if ($ProgrammerOnly) { $IncludeProgrammer = $true }
if (-not $ProgrammerOnly) {
    $armName = 'arm-gnu-toolchain-15.3.rel1-mingw-w64-i686-arm-none-eabi.zip'
    $armUrl = 'https://gitlab.arm.com/api/v4/projects/tooling%2Fgnu-toolchains-for-arm/packages/generic/gnu-toolchain/15.3.rel1/' + $armName
    $arm = Get-Package $armName 292045003 'f1941b026f7a5383cf69ad09640dd4ef3e1479d612fd77c2c24cd820cf5c279d' $armUrl
    $toolchain = Join-Path $root 'toolchain'
    if (-not $VerifyOnly -and -not (Test-Path -LiteralPath (Join-Path $toolchain 'bin/arm-none-eabi-gcc.exe'))) {
        if (Test-Path -LiteralPath $toolchain) { throw "Incomplete toolchain directory: $toolchain" }
        Expand-Archive -LiteralPath $arm -DestinationPath $toolchain
    }
}
if ($IncludeProgrammer) {
    $isp = Get-Package 'XHSC_ISP_V2.23.zip' 729856 '2f729500a72fce0e6eacf5be018c2489419bd2cc0623c02a196837bb441501ae' 'https://github.com/reziarlleh/Sovol_SH01mod/releases/download/v1.4.1/XHSC_ISP_V2.23.zip'
    $ispDir = Join-Path $downloads 'XHSC_ISP_V2.23'
    if (-not $VerifyOnly -and -not (Test-Path -LiteralPath (Join-Path $ispDir 'XHSC ISP/XHSC.exe'))) {
        if (Test-Path -LiteralPath $ispDir) { throw "Incomplete programmer directory: $ispDir" }
        Expand-Archive -LiteralPath $isp -DestinationPath $ispDir
    }
}
Write-Host 'Tools ready. No serial-port operation was performed.'
