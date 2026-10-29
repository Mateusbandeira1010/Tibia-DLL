param(
    [Parameter(Mandatory = $true)]
    [string]$DllPath,
    [int]$Enabled = 1
)

$ErrorActionPreference = 'Stop'
if ($Enabled -eq 0) {
    exit 0
}

$resolvedPath = (Resolve-Path -LiteralPath $DllPath).Path
$magic = [byte[]](0xD4, 0x91, 0x7A, 0x2C, 0xE8, 0x35, 0x6B, 0xF0)
$magicHex = [BitConverter]::ToString($magic)
$trailerSize = 56
$bytes = [IO.File]::ReadAllBytes($resolvedPath)

if ($bytes.Length -ge $trailerSize -and
    [BitConverter]::ToString($bytes, $bytes.Length - $trailerSize, 8) -eq
        $magicHex) {
    $trimmed = New-Object byte[] ($bytes.Length - $trailerSize)
    [Array]::Copy($bytes, $trimmed, $trimmed.Length)
    $bytes = $trimmed
}

if ($bytes.Length -lt 512 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) {
    throw 'Invalid PE image.'
}
$peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
$optionalOffset = $peOffset + 24
if ($peOffset -lt 0 -or $optionalOffset + 136 -gt $bytes.Length -or
    [BitConverter]::ToUInt32($bytes, $peOffset) -ne 0x00004550 -or
    [BitConverter]::ToUInt16($bytes, $optionalOffset) -ne 0x010B) {
    throw 'Invalid Win32 PE image.'
}

$normalized = [byte[]]$bytes.Clone()
[Array]::Clear($normalized, $optionalOffset + 64, 4)
[Array]::Clear($normalized, $optionalOffset + 128, 8)
$sha256 = [Security.Cryptography.SHA256]::Create()
try {
    $digest = $sha256.ComputeHash($normalized)
}
finally {
    $sha256.Dispose()
    [Array]::Clear($normalized, 0, $normalized.Length)
}

$stream = [IO.File]::Open($resolvedPath, [IO.FileMode]::Create,
    [IO.FileAccess]::Write, [IO.FileShare]::Read)
try {
    $writer = New-Object IO.BinaryWriter($stream, [Text.Encoding]::UTF8, $true)
    try {
        $writer.Write($bytes)
        $writer.Write($magic)
        $writer.Write([uint32]1)
        $writer.Write([uint64]$bytes.Length)
        $writer.Write($digest)
        $writer.Write([uint32]0)
        $writer.Flush()
    }
    finally {
        $writer.Dispose()
    }
}
finally {
    $stream.Dispose()
}

Write-Host "Integrity stamp applied: $resolvedPath"
