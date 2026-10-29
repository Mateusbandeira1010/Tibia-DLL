param(
    [Parameter(Mandatory = $true)]
    [string]$DllPath,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$magicHex = 'D4-91-7A-2C-E8-35-6B-F0'
$trailerSize = 56
$forbidden = @(
    'Mateuzkl', 'DLL modified by', 'Mount (Ctrl+M)',
    'Dismount (Ctrl+M)', 'native mount panel',
    'invalid group count', 'mount_debug.log', 'framegroups_debug.log',
    '.pdb', 'C:\Users\Mateus'
)
$forbiddenByteSequences = [ordered]@{
    'Weather server marker' = 'D7-2B-91-4E-C3-68-AF-15'
    'Weather login marker' = '5A-57-44-4C-01-3C-A5-86'
}

function Test-ContainsByteSequence([byte[]]$Bytes, [byte[]]$Pattern) {
    if ($Pattern.Length -eq 0 -or $Pattern.Length -gt $Bytes.Length) {
        return $false
    }
    for ($offset = 0; $offset -le $Bytes.Length - $Pattern.Length;
        ++$offset) {
        $matches = $true
        for ($index = 0; $index -lt $Pattern.Length; ++$index) {
            if ($Bytes[$offset + $index] -ne $Pattern[$index]) {
                $matches = $false
                break
            }
        }
        if ($matches) {
            return $true
        }
    }
    return $false
}

function Get-PeLayout([byte[]]$Bytes) {
    if ($Bytes.Length -lt 512 -or
        [BitConverter]::ToUInt16($Bytes, 0) -ne 0x5A4D) {
        throw 'Invalid PE image.'
    }
    $peOffset = [BitConverter]::ToInt32($Bytes, 0x3C)
    $optionalOffset = $peOffset + 24
    if ($peOffset -lt 0 -or $optionalOffset + 136 -gt $Bytes.Length -or
        [BitConverter]::ToUInt32($Bytes, $peOffset) -ne 0x00004550 -or
        [BitConverter]::ToUInt16($Bytes, $optionalOffset) -ne 0x010B) {
        throw 'Invalid Win32 PE image.'
    }
    return @{
        Checksum = $optionalOffset + 64
        Security = $optionalOffset + 128
    }
}

function Test-IntegrityStamp([string]$Path) {
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($bytes.Length -le $trailerSize) {
        return $false
    }
    try {
        $layout = Get-PeLayout $bytes
    }
    catch {
        return $false
    }
    $certificateOffset = [BitConverter]::ToUInt32($bytes, $layout.Security)
    $certificateSize = [BitConverter]::ToUInt32($bytes,
        $layout.Security + 4)
    if ($certificateOffset -ne 0 -or $certificateSize -ne 0) {
        if ($certificateOffset -lt $trailerSize -or $certificateSize -eq 0 -or
            [uint64]$certificateOffset + $certificateSize -gt $bytes.Length) {
            return $false
        }
        $trailerOffset = $certificateOffset - $trailerSize
    }
    else {
        $trailerOffset = $bytes.Length - $trailerSize
    }
    if ([BitConverter]::ToString($bytes, $trailerOffset, 8) -ne $magicHex -or
        [BitConverter]::ToUInt32($bytes, $trailerOffset + 8) -ne 1 -or
        [BitConverter]::ToUInt64($bytes, $trailerOffset + 12) -ne
            [uint64]$trailerOffset) {
        return $false
    }

    $covered = New-Object byte[] $trailerOffset
    [Array]::Copy($bytes, $covered, $covered.Length)
    [Array]::Clear($covered, $layout.Checksum, 4)
    [Array]::Clear($covered, $layout.Security, 8)
    $sha256 = [Security.Cryptography.SHA256]::Create()
    try {
        $actual = [BitConverter]::ToString($sha256.ComputeHash($covered))
    }
    finally {
        $sha256.Dispose()
        [Array]::Clear($covered, 0, $covered.Length)
    }
    $expected = [BitConverter]::ToString($bytes, $trailerOffset + 20, 32)
    return $actual -eq $expected
}

$resolvedPath = (Resolve-Path -LiteralPath $DllPath).Path
if (-not (Test-IntegrityStamp $resolvedPath)) {
    throw 'Invalid or missing DLL integrity stamp.'
}

$binary = [IO.File]::ReadAllBytes($resolvedPath)
$ascii = [Text.Encoding]::ASCII.GetString($binary)
$unicode = [Text.Encoding]::Unicode.GetString($binary)
foreach ($term in $forbidden) {
    if ($ascii.Contains($term) -or $unicode.Contains($term)) {
        throw "Forbidden public string found: $term"
    }
}
foreach ($entry in $forbiddenByteSequences.GetEnumerator()) {
    [byte[]]$pattern = $entry.Value.Split('-') | ForEach-Object {
        [Convert]::ToByte($_, 16)
    }
    if (Test-ContainsByteSequence $binary $pattern) {
        throw "Forbidden public byte sequence found: $($entry.Key)"
    }
}

if ($SelfTest) {
    $temporaryName = "tibia-integrity-{0}.dll" -f
        [Guid]::NewGuid().ToString('N')
    $temporaryPath = Join-Path ([IO.Path]::GetTempPath()) $temporaryName
    try {
        [IO.File]::Copy($resolvedPath, $temporaryPath, $false)
        $testBytes = [IO.File]::ReadAllBytes($temporaryPath)
        $testOffset = [Math]::Min(4096, $testBytes.Length - $trailerSize - 1)
        $testBytes[$testOffset] = $testBytes[$testOffset] -bxor 0x01
        [IO.File]::WriteAllBytes($temporaryPath, $testBytes)
        if (Test-IntegrityStamp $temporaryPath) {
            throw 'Integrity self-test failed to detect a one-byte change.'
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporaryPath) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
    Write-Host 'One-byte modification test: detected'
}

Write-Host 'Public DLL verification: passed'
