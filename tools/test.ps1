[CmdletBinding()]
param([string]$Compiler = '')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Compiler) { $Compiler = Join-Path $root '.tools\host\llvm-mingw-20261020-ucrt-x86_64\bin\clang.exe' }
$output = Join-Path $root 'build\host-tests'
New-Item -ItemType Directory -Force -Path $output | Out-Null
foreach ($suite in @('controller', 'sensor', 'dual_sensor', 'sensor_timing', 'bus', 'lcd', 'storage', 'resume')) {
    $test = Join-Path $root "tests\test_$suite.c"
    $sources = @((Join-Path $root "src\$suite.c"))
    if ($suite -eq 'resume') { $sources = @((Join-Path $root 'src\controller.c'), (Join-Path $root 'src\storage.c')) }
    if ($suite -in @('dual_sensor', 'sensor_timing')) { $sources = @((Join-Path $root 'src\sensor.c')) }
    $exe = Join-Path $output "test_$suite.exe"
    & $Compiler -std=c11 -O1 -g -Wall -Wextra -Werror ('-I' + (Join-Path $root 'src')) $test @sources -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $suite" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $suite" }
}
Write-Host 'All eight SH01 C suites passed.'
