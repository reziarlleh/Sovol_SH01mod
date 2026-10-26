[CmdletBinding()]
param([switch]$VerifyOnly)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'This pinned host compiler targets Windows x64.' }

$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$hostRoot = Join-Path $projectRoot '.tools\host'
$packageName = 'llvm-mingw-20261020-ucrt-x86_64'
$archiveName = $packageName + '.zip'
$archivePath = Join-Path $hostRoot $archiveName
$installRoot = Join-Path $hostRoot $packageName
$compiler = Join-Path $installRoot 'bin\clang.exe'
$expectedSize = 190612575L
$expectedSha256 = 'fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd'
$sourceUrl = 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261020/llvm-mingw-20261020-ucrt-x86_64.zip'

function Assert-Archive([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Host compiler archive missing: $Path" }
    if ((Get-Item -LiteralPath $Path).Length -ne $expectedSize) {
        throw "Host compiler archive size mismatch; existing file was not overwritten: $Path"
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $expectedSha256) {
        throw "Host compiler archive SHA256 mismatch; existing file was not overwritten: $Path"
    }
}

function Assert-Https([Uri]$Uri) {
    if (-not $Uri.IsAbsoluteUri -or $Uri.Scheme -ne 'https' -or
        -not [string]::IsNullOrEmpty($Uri.UserInfo)) {
        throw 'Host compiler downloads and redirects must use absolute HTTPS URLs without credentials.'
    }
}

function Receive-Archive {
    Add-Type -AssemblyName System.Net.Http
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    $handler = [Net.Http.HttpClientHandler]::new()
    $handler.AllowAutoRedirect = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(30)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd('Sovol_SH01mod-host-test-setup/1.0')
    $partial = $archivePath + '.partial.' + [Guid]::NewGuid().ToString('N')
    $uri = [Uri]$sourceUrl
    try {
        for ($redirect = 0; $redirect -le 10; ++$redirect) {
            Assert-Https $uri
            $response = $null
            try {
                $response = $client.GetAsync($uri, [Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
                if ([int]$response.StatusCode -in @(301, 302, 303, 307, 308)) {
                    if ($redirect -eq 10 -or $null -eq $response.Headers.Location) { throw 'Host compiler download redirect failed.' }
                    $uri = [Uri]::new($uri, $response.Headers.Location)
                    continue
                }
                $response.EnsureSuccessStatusCode() | Out-Null
                $stream = [IO.File]::Open($partial, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try { $response.Content.CopyToAsync($stream).GetAwaiter().GetResult() | Out-Null }
                finally { $stream.Dispose() }
                Assert-Archive $partial
                [IO.File]::Move($partial, $archivePath)
                return
            } finally {
                if ($null -ne $response) { $response.Dispose() }
            }
        }
    } finally {
        $client.Dispose()
        if (Test-Path -LiteralPath $partial -PathType Leaf) { Remove-Item -LiteralPath $partial -Force }
    }
}

function Get-StreamSha256($Stream) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($Stream)).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}

function Assert-ZipEntries($Zip) {
    $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $prefix = $packageName + '/'
    foreach ($entry in $Zip.Entries) {
        $name = $entry.FullName.Replace('\', '/')
        if (-not $name.StartsWith($prefix, [StringComparison]::Ordinal)) {
            throw "Unexpected compiler ZIP root: $($entry.FullName)"
        }
        foreach ($part in $name.TrimEnd('/').Split('/')) {
            if ([string]::IsNullOrEmpty($part) -or $part -in @('.', '..') -or
                $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 -or
                $part -match '[. ]$' -or $part -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
                throw "Unsafe compiler ZIP entry: $($entry.FullName)"
            }
        }
        if (-not $names.Add($name.TrimEnd('/'))) { throw "Duplicate compiler ZIP entry: $($entry.FullName)" }
        if ((($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000) {
            throw "Symbolic links are not accepted in the compiler ZIP: $($entry.FullName)"
        }
    }
    if (-not $names.Contains($prefix + 'bin/clang.exe')) { throw 'Pinned compiler ZIP has no bin/clang.exe.' }
}

function Assert-Installation($Zip, [string]$Root) {
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw "Prepared host compiler directory missing: $Root" }
    $allowedPrefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $count = 0
    foreach ($entry in $Zip.Entries) {
        $name = $entry.FullName.Replace('\', '/')
        if ($name.EndsWith('/')) { continue }
        $relative = $name.Substring($packageName.Length + 1)
        $path = [IO.Path]::GetFullPath((Join-Path $Root $relative))
        if (-not $path.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Compiler ZIP path escaped the installation directory.' }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-Item -LiteralPath $path).Length -ne $entry.Length) {
            throw "Incomplete or changed host compiler; no existing files were overwritten: $path"
        }
        $archiveStream = $entry.Open()
        try { $expected = Get-StreamSha256 $archiveStream }
        finally { $archiveStream.Dispose() }
        $fileStream = [IO.File]::OpenRead($path)
        try { $actual = Get-StreamSha256 $fileStream }
        finally { $fileStream.Dispose() }
        if ($actual -ne $expected) { throw "Host compiler file SHA256 mismatch; no existing files were overwritten: $path" }
        ++$count
    }
    Write-Host "Prepared compiler files checked against the pinned ZIP: $count"
}

if ($VerifyOnly) {
    Assert-Archive $archivePath
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "Host compiler is not prepared: $compiler" }
} else {
    New-Item -ItemType Directory -Path $hostRoot -Force | Out-Null
    if (Test-Path -LiteralPath $archivePath) { Assert-Archive $archivePath }
    else { Write-Host "Downloading pinned host compiler: $archiveName"; Receive-Archive }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    Assert-ZipEntries $zip
    if (Test-Path -LiteralPath $installRoot) {
        Assert-Installation $zip $installRoot
    } elseif ($VerifyOnly) {
        throw "Host compiler is not prepared: $installRoot"
    } else {
        $staging = Join-Path $hostRoot ('.extract-llvm-' + [Guid]::NewGuid().ToString('N'))
        Write-Host "Extracting pinned host compiler into staging: $staging"
        # An interrupted extraction remains available for inspection. A unique
        # staging tree and Directory.Move prevent overwriting an existing install.
        [IO.Compression.ZipFile]::ExtractToDirectory($archivePath, $staging)
        $stagedRoot = Join-Path $staging $packageName
        Assert-Installation $zip $stagedRoot
        [IO.Directory]::Move($stagedRoot, $installRoot)
        [IO.Directory]::Delete($staging, $false)
    }
} finally { $zip.Dispose() }

if (-not $VerifyOnly) {
    & $compiler --version
    if ($LASTEXITCODE -ne 0) { throw "Prepared Clang could not start: $LASTEXITCODE" }
}
Write-Host "Host compiler ready: $compiler"
Write-Host 'PATH and system settings were not changed. No hardware access was performed.'
