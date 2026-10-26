[CmdletBinding()]
param(
    [string]$ToolchainRoot = '',
    [string]$OutputDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$config = [IO.File]::ReadAllText((Join-Path $projectRoot 'src/config.h'))
$versionMatch = [regex]::Match($config, '#define FW_VERSION "(\d+\.\d+\.\d+)"')
if (-not $versionMatch.Success) { throw 'Firmware version missing from src/config.h' }
$version = $versionMatch.Groups[1].Value
if ([string]::IsNullOrWhiteSpace($ToolchainRoot)) {
    $ToolchainRoot = Join-Path $projectRoot 'toolchain'
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot ('build\v' + $version)
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$projectRoot = [IO.Path]::GetFullPath($projectRoot)
$artifactName = 'sovol_sh01mod_v' + $version.Replace('.', '_')

$gcc = Join-Path $ToolchainRoot 'bin\arm-none-eabi-gcc.exe'
$objcopy = Join-Path $ToolchainRoot 'bin\arm-none-eabi-objcopy.exe'
$size = Join-Path $ToolchainRoot 'bin\arm-none-eabi-size.exe'
$linkerScript = Join-Path $PSScriptRoot 'gnu_build\hc32f005c6pa.ld'
$startup = Join-Path $PSScriptRoot 'gnu_build\startup_hc32f005_gcc.c'
$runtime = Join-Path $PSScriptRoot 'gnu_build\runtime.c'

if (-not (Test-Path -LiteralPath $gcc)) {
    throw "ARM GCC not found: $gcc"
}
if (-not (Test-Path -LiteralPath $objcopy)) {
    throw "ARM objcopy not found: $objcopy"
}
if (-not (Test-Path -LiteralPath $linkerScript)) {
    throw "Linker script not found: $linkerScript"
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$includeDirectories = @(
    (Join-Path $projectRoot 'src'),
    (Join-Path $PSScriptRoot 'gnu_build'),
    (Join-Path $projectRoot 'vendor\HC32F005_DDL_Rev2.0.0\driver\inc'),
    (Join-Path $projectRoot 'vendor\HC32F005_DDL_Rev2.0.0\mcu\common'),
    (Join-Path $projectRoot 'vendor\CMSIS_5\CMSIS\Core\Include')
)
$includeArguments = @($includeDirectories | ForEach-Object { '-I' + $_ })

$commonFlags = @(
    '-std=c11',
    '-mcpu=cortex-m0plus',
    '-mthumb',
    '-mfloat-abi=soft',
    '-ffreestanding',
    '-fno-builtin',
    '-fno-common',
    '-ffunction-sections',
    '-fdata-sections',
    '-Os',
    '-g3',
    '-Wall',
    '-Wextra',
    '-Werror'
)
$applicationSources = @(
    'src\main.c',
    'src\hal.c',
    'src\bus.c',
    'src\sensor.c',
    'src\lcd.c',
    'src\controller.c',
    'src\storage.c',
    'src\flash_hal.c'
)
$vendorSources = @(
    'vendor\HC32F005_DDL_Rev2.0.0\driver\src\ddl.c',
    'vendor\HC32F005_DDL_Rev2.0.0\driver\src\sysctrl.c',
    'vendor\HC32F005_DDL_Rev2.0.0\driver\src\gpio.c',
    'vendor\HC32F005_DDL_Rev2.0.0\driver\src\adc.c',
    'vendor\HC32F005_DDL_Rev2.0.0\driver\src\wdt.c',
    'vendor\HC32F005_DDL_Rev2.0.0\driver\src\flash.c',
    'vendor\HC32F005_DDL_Rev2.0.0\mcu\common\system_hc32f005.c'
)
$supportSources = @($startup, $runtime)

function Invoke-Native([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Executable"
    }
}
$objects = New-Object 'System.Collections.Generic.List[string]'

foreach ($source in $applicationSources) {
    $sourcePath = Join-Path $projectRoot $source
    $objectPath = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source)) + '.o')
    Write-Host "CC  $source"
    Invoke-Native $gcc ($commonFlags + $includeArguments + @('-MMD', '-MP', '-c', $sourcePath, '-o', $objectPath))
    $objects.Add($objectPath)
}

foreach ($source in $vendorSources) {
    $sourcePath = Join-Path $projectRoot $source
    $objectPath = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source)) + '.o')
    Write-Host "CC  $source"
    $vendorFlags = $commonFlags + @('-Wno-unused-parameter') + $includeArguments + @('-MMD', '-MP', '-c', $sourcePath, '-o', $objectPath)
    Invoke-Native $gcc $vendorFlags
    $objects.Add($objectPath)
}

foreach ($source in $supportSources) {
    $objectPath = Join-Path $OutputDirectory (([IO.Path]::GetFileNameWithoutExtension($source)) + '.o')
    Write-Host "CC  $source"
    Invoke-Native $gcc ($commonFlags + @('-MMD', '-MP', '-c', $source, '-o', $objectPath))
    $objects.Add($objectPath)
}

$elf = Join-Path $OutputDirectory ($artifactName + '.elf')
$hex = Join-Path $OutputDirectory ($artifactName + '.hex')
$bin = Join-Path $OutputDirectory ($artifactName + '.bin')
$map = Join-Path $OutputDirectory ($artifactName + '.map')

Write-Host 'LD  firmware'
$linkFlags = @(
    '-mcpu=cortex-m0plus',
    '-mthumb',
    '-mfloat-abi=soft',
    '-nostdlib',
    '-Wl,--gc-sections',
    ('-Wl,-Map=' + $map),
    ('-T' + $linkerScript),
    '-o', $elf
)
Invoke-Native $gcc ($linkFlags + @($objects.ToArray()) + @('-lgcc'))

Write-Host 'OC  HEX/BIN'
Invoke-Native $objcopy @('-O', 'ihex', $elf, $hex)
Invoke-Native $objcopy @('-O', 'binary', $elf, $bin)

Write-Host 'SIZE'
Invoke-Native $size @($elf)

$checker = Join-Path $projectRoot 'tools\check_hex_storage.ps1'
$powershell = Get-Command powershell.exe -ErrorAction SilentlyContinue
if ($null -ne $powershell) {
    Write-Host 'CHK HEX storage area'
    Invoke-Native $powershell.Source @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $checker, '-HexFile', $hex)
}

Write-Host "Build complete: $hex"
