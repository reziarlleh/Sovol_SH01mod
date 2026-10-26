[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidatePattern('^COM\d+$')][string]$ComPort,
    [string]$HexFile = '',
    [switch]$Readback,
    [switch]$PrepareOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
# Reuse the pinned binary, MCU mapping, HEX validation and isolated configuration.
$output = @(& (Join-Path $PSScriptRoot 'flash.ps1') -ComPort $ComPort -HexFile $HexFile -DryRun)
$sessions = @($output | Where-Object { $null -ne $_.PSObject.Properties['WorkingDirectory'] })
if ($sessions.Count -ne 1) { throw 'Programmer preparation failed.' }
$prepared = $sessions[0]
$config = [xml][IO.File]::ReadAllText($prepared.ConfigPath)
$config.SystemConfig.EnglishLanguage = 'true'
if ($Readback) {
    $config.SystemConfig.HexFile = ''
    foreach ($name in @('Erase', 'BlankCheck', 'Program', 'Verify', 'Encrypt', 'Encrypt2')) {
        $config.SystemConfig.$name = 'false'
    }
    Write-Host 'Readback: empty HEX means full Flash. Use Upload, then save to a new local HEX file.'
}
$config.Save($prepared.ConfigPath)
if ($PrepareOnly) {
    Write-Host 'Prepared only: no GUI or serial connection was started.'
    return $prepared
}
# This explicit GUI command is intended to show the programmer to its operator.
# The working directory prevents Config.xml writes into a launcher/system folder.
Start-Process -FilePath $prepared.Programmer -WorkingDirectory $prepared.WorkingDirectory -WindowStyle Normal
Write-Host 'GUI opened. No erase/program/read button was pressed by this launcher.'
