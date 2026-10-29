param(
    [switch]$SkipBuild,
    [ValidateSet('Release', 'ReleasePublic')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$solution = Join-Path $repository 'vs2022\TibiaExtended.sln'
$builtDll = Join-Path $repository "vs2022\$Configuration\ddraw.dll"
$verifyScript = Join-Path $PSScriptRoot 'verify_public_binary.ps1'
$publicDirectory = Join-Path $repository 'dist\public'

if (-not $publicDirectory.StartsWith($repository,
    [StringComparison]::OrdinalIgnoreCase) -or $publicDirectory -eq $repository) {
    throw 'Unsafe public output path.'
}

if (-not $SkipBuild) {
    $msbuild = $null
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vswhereArguments = @('-latest', '-prerelease', '-products', '*',
            '-requires', 'Microsoft.Component.MSBuild', '-find',
            'MSBuild\**\Bin\MSBuild.exe')
        $msbuild = & $vswhere @vswhereArguments | Select-Object -First 1
    }
    if (-not $msbuild) {
        $fallback = 'C:\Program Files\Microsoft Visual Studio\18\Insiders\MSBuild\Current\Bin\MSBuild.exe'
        if (Test-Path -LiteralPath $fallback) {
            $msbuild = $fallback
        }
    }
    if (-not $msbuild) {
        throw 'MSBuild was not found. Install the Visual Studio C++ workload.'
    }
    $buildArguments = @($solution, '/t:Rebuild', '/m',
        "/p:Configuration=$Configuration", '/p:Platform=x86', '/v:minimal')
    & $msbuild @buildArguments
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}

if (-not (Test-Path -LiteralPath $builtDll)) {
    throw 'Release ddraw.dll was not generated.'
}
& $verifyScript -DllPath $builtDll -SelfTest

if (Test-Path -LiteralPath $publicDirectory) {
    Remove-Item -LiteralPath $publicDirectory -Recurse -Force
}
New-Item -ItemType Directory -Path $publicDirectory | Out-Null
$publicDll = Join-Path $publicDirectory 'ddraw.dll'
Copy-Item -LiteralPath $builtDll -Destination $publicDll

$certificateThumbprint = $env:TIBIA_SIGN_CERT_SHA1
if ($certificateThumbprint) {
    $signTool = $env:TIBIA_SIGNTOOL_PATH
    if (-not $signTool -or -not (Test-Path -LiteralPath $signTool)) {
        throw 'Set TIBIA_SIGNTOOL_PATH to signtool.exe before signing.'
    }
    $timestampUrl = $env:TIBIA_SIGN_TIMESTAMP_URL
    if (-not $timestampUrl) {
        $timestampUrl = 'http://timestamp.digicert.com'
    }
    $signArguments = @('sign', '/sha1', $certificateThumbprint, '/fd',
        'SHA256', '/tr', $timestampUrl, '/td', 'SHA256', $publicDll)
    & $signTool @signArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode signing failed with exit code $LASTEXITCODE."
    }
    & $signTool verify /pa /v $publicDll
    if ($LASTEXITCODE -ne 0) {
        throw "Authenticode verification failed with exit code $LASTEXITCODE."
    }
}

& $verifyScript -DllPath $publicDll
$file = Get-Item -LiteralPath $publicDll
$hash = (Get-FileHash -LiteralPath $publicDll -Algorithm SHA256).Hash
$manifest = [ordered]@{
    file = $file.Name
    size = $file.Length
    sha256 = $hash
    signed = [bool]$certificateThumbprint
    generatedUtc = [DateTime]::UtcNow.ToString('o')
}
$manifestPath = Join-Path $publicDirectory 'build-manifest.json'
$manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host "Public DLL: $publicDll"
Write-Host "Size: $($file.Length) bytes"
Write-Host "SHA-256: $hash"
