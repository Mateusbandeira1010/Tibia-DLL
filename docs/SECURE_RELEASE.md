# Secure public release

The standard `Release|Win32` configuration is the public configuration. It
removes development logs and personal credits, enables string concealment and
the lightweight integrity check, disables PDB/MAP generation, and applies the
optimized linker settings. `Debug|Win32` remains suitable for development.

## Build and verify

Run from the repository root:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build_public_release.ps1
```

The script rebuilds the DLL, applies its integrity stamp, verifies that a
one-byte modification is detected, rejects forbidden ASCII/UTF-16 strings, and
copies only these public artifacts to `dist\public`:

- `ddraw.dll`
- `build-manifest.json`

The public directory never receives PDB, OBJ, ILK, MAP, source, or signing-key
files. The manifest records the final file size and SHA-256.

To validate an already built file:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\verify_public_binary.ps1 `
  -DllPath .\vs2022\Release\ddraw.dll -SelfTest
```

## Authenticode (optional)

Keep the certificate and private key outside the repository. Set only the
certificate thumbprint and tool path in the build environment:

```powershell
$env:TIBIA_SIGNTOOL_PATH = 'C:\Path\To\signtool.exe'
$env:TIBIA_SIGN_CERT_SHA1 = 'CERTIFICATE_THUMBPRINT'
$env:TIBIA_SIGN_TIMESTAMP_URL = 'http://timestamp.digicert.com'
powershell -NoProfile -ExecutionPolicy Bypass `
  -File .\scripts\build_public_release.ps1
```

The script signs the copied public DLL, verifies the Authenticode signature,
then verifies the internal normalized SHA-256 stamp again. It never accepts a
PFX path or password and never stores certificate material.

Manual verification:

```powershell
& $env:TIBIA_SIGNTOOL_PATH verify /pa /v .\dist\public\ddraw.dll
Get-FileHash .\dist\public\ddraw.dll -Algorithm SHA256
```

## Optional VMProtect markers

The project builds without a proprietary SDK. To enable markers locally,
define `TIBIA_ENABLE_PROTECT_MARKERS=1`, make `VMProtectSDK.h` available to the
compiler, and link the matching x86 SDK library. Only the small integrity
routines use generic marker names. Do not virtualize `DllMain`,
`DirectDrawCreate`, naked/ASM hooks, rendering, movement, or per-frame code.

## Security boundaries

The stamp detects ordinary file edits, including hex-editor changes. It is not
a substitute for Authenticode: a skilled analyst who can rewrite the verifier
can bypass a client-side check. Protect final public DLLs with a real
code-signing certificate when possible.
