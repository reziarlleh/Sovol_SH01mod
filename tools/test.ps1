[CmdletBinding()]
param([string]$Compiler = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) { $Compiler = Join-Path $root '.tools\host\llvm-mingw-20261020-ucrt-x86_64\bin\clang.exe' }
$output = Join-Path $root 'build\host-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($suite in @('controller', 'sensor', 'bus', 'lcd')) {
    $test = Join-Path $root "tests\test_$suite.c"
    $source = Join-Path $root "src\$suite.c"
    $exe = Join-Path $output "test_$suite.exe"
    & $Compiler -std=c11 -O1 -g -Wall -Wextra -Werror ('-I' + (Join-Path $root 'src')) $test $source -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $suite" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $suite" }
}
Write-Host 'All four SH01 C suites passed.'
