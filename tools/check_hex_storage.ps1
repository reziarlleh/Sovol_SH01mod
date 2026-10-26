param([Parameter(Mandatory=$true)][string]$HexFile)

Set-StrictMode -Version 2.0

function Fail([string]$Message) {
    Write-Error ("FAIL: {0}" -f $Message)
    exit 2
}

if (-not (Test-Path -LiteralPath $HexFile -PathType Leaf)) {
    Fail ("HEX file not found: {0}" -f $HexFile)
}

$limit = [int64]0x7C00
$upper = [int64]0
$maxAddr = [int64]-1
$dataRecordCount = 0
$eofSeen = $false
$lineNumber = 0

foreach ($rawLine in (Get-Content -LiteralPath $HexFile)) {
    ++$lineNumber
    $line = $rawLine.Trim()
    if ($line.Length -eq 0) { continue }
    if ($eofSeen) { Fail ("data after EOF at line {0}" -f $lineNumber) }
    if (-not $line.StartsWith(':')) { Fail ("line {0} does not start with ':'" -f $lineNumber) }

    $hex = $line.Substring(1)
    if (($hex.Length -lt 10) -or (($hex.Length % 2) -ne 0) -or ($hex -notmatch '^[0-9A-Fa-f]+$')) {
        Fail ("invalid record syntax at line {0}" -f $lineNumber)
    }

    $len = [Convert]::ToInt32($hex.Substring(0,2),16)
    $expectedLength = 10 + (2 * $len)
    if ($hex.Length -ne $expectedLength) {
        Fail ("record length mismatch at line {0}" -f $lineNumber)
    }

    [int]$sum = [Convert]::ToInt32($hex.Substring(0,2),16)
    for ($i = 1; $i -lt ($hex.Length / 2); ++$i) {
        $sum += [Convert]::ToInt32($hex.Substring($i * 2,2),16)
    }
    if (($sum % 256) -ne 0) { Fail ("checksum mismatch at line {0}" -f $lineNumber) }

    $addr = [Convert]::ToInt32($hex.Substring(2,4),16)
    $type = [Convert]::ToInt32($hex.Substring(6,2),16)
    switch ($type) {
        0 {
            if ($len -eq 0) { Fail ("empty data record at line {0}" -f $lineNumber) }
            $start = $upper + [int64]$addr
            $end = $start + [int64]$len - 1
            if ($start -lt 0 -or $end -lt $start) { Fail ("address overflow at line {0}" -f $lineNumber) }
            if ($end -gt $maxAddr) { $maxAddr = $end }
            ++$dataRecordCount
            if ($end -ge $limit) {
                Fail ("HEX reaches storage area 0x7C00..0x7FFF at line {0}" -f $lineNumber)
            }
        }
        1 {
            if ($len -ne 0 -or $addr -ne 0) { Fail ("invalid EOF record at line {0}" -f $lineNumber) }
            $eofSeen = $true
        }
        2 {
            if ($len -ne 2 -or $addr -ne 0) { Fail ("invalid extended-segment record at line {0}" -f $lineNumber) }
            $upper = [int64]([Convert]::ToInt32($hex.Substring(8,4),16)) * 0x10
        }
        3 {
            if ($len -ne 4 -or $addr -ne 0) { Fail ("invalid start-segment record at line {0}" -f $lineNumber) }
        }
        4 {
            if ($len -ne 2 -or $addr -ne 0) { Fail ("invalid extended-linear record at line {0}" -f $lineNumber) }
            $upper = [int64]([Convert]::ToInt32($hex.Substring(8,4),16)) * 0x10000
        }
        5 {
            if ($len -ne 4 -or $addr -ne 0) { Fail ("invalid start-linear record at line {0}" -f $lineNumber) }
        }
        default { Fail ("unsupported record type {0} at line {1}" -f $type,$lineNumber) }
    }
}

if (-not $eofSeen) { Fail "missing EOF record" }
if ($dataRecordCount -eq 0) { Fail "HEX contains no programmed data records" }

Write-Host ("Highest programmed address: 0x{0:X8}" -f [uint64]$maxAddr)
Write-Host "OK: HEX structure/checksums are valid and storage area 0x7C00..0x7FFF is not occupied by the application."
