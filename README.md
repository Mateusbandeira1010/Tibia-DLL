This code is distributed "as-is" without a license, for study purposes. Use it
at your own risk.

# Tibia 8.60 extended client DLL

`ddraw.dll` is a 32-bit proxy DLL for the CIP Tibia 8.60 client. The current
source supports:

- extended `.spr` files and validated sprite decoding;
- DAT frame groups and timed idle animations;
- alpha-channel sprite support for DirectX 9 and OpenGL (disabled in the
  production profile);
- the mount protocol, rendering, outfit-window selector and Ctrl+M toggle;
- native Chat/Smart Walking controls;
- an optional Store overlay, disabled by default (`should_use_store = false`);
- 16-bit effects/distance shoots and 32-bit health/mana fields;
- 16-bit outfit counts with an outfit limit of 255;
- health/mana bars, the FPS overlay and the high-resolution walk timer;
- multiclient startup patch for the matching CIP 8.60 executable;
- optional custom ping and weather, disabled for the supplied TFS 1.7 server.

# Compatibility

Copy only `ddraw.dll` next to `Tibia.exe`. The production profile is compiled
into the DLL and matches the extended TFS layout with a 16-bit outfit count.
No external configuration file is read or generated.

The compiled packet widths and custom opcodes must match the server. A mismatch
will desynchronize the packet stream and can crash the original client.

This package matches the supplied `src tfs 1.7/protocolgame.cpp`: health/mana
use 32 bits, effects/distance shoots use 16 bits, outfits include mounts, and
the outfit count uses 16 bits. The native dialog displays at most 255 outfits;
additional entries are consumed before reading the mount list.

`should_use_custom_ping = false` preserves the native empty `0x1E` ping.
The numbered custom ping protocol and its RTT overlay are not used by this
server. `should_use_custom_client_os = false` preserves the native Windows OS.
`should_use_multiclient = true` applies the verified 8.60 multiclient branch
patch in process memory during early startup. It does not modify `Tibia.exe` on
disk and stops with a clear error if the expected executable bytes do not match.
`should_use_weather = false` disables weather installation, the extra login
marker, packet interception, rendering and the weather timeout. Enable weather
only alongside its matching server protocol and custom client OS setting.

After successful initialization the DLL is pinned for the lifetime of the
client process. This is intentional: unloading a module while native Tibia
hooks can still execute would leave dangling jumps into released code.

# Building

Requirements: Visual Studio with the MSVC x86 toolchain and Windows SDK.
Use `Debug|Win32` for development and `Release|Win32` for distribution.

Build the supplied solution:

```powershell
msbuild .\vs2022\TibiaExtended.sln /m /p:Configuration=Release /p:Platform=x86
```

Or use the root CMake project from an x86 Visual Studio developer shell:

```powershell
cmake -S . -B out\build -A Win32
cmake --build out\build --config Release
ctest --test-dir out\build -C Release --output-on-failure
```

Both build paths compile as C++17 and link `bcrypt` and `opengl32`. The project intentionally
targets only the verified Tibia 8.60 memory layout.

The ready-to-copy binary is included at `dist/public/ddraw.dll`, with its
SHA-256 in `dist/public/build-manifest.json`.

For the verified public package, forbidden-string audit, integrity self-test,
optional Authenticode signing, and SHA-256 manifest, see
[Secure public release](docs/SECURE_RELEASE.md).
