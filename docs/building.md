# Building COGBox and the ports

You need your **own copy of each game** (with its title update) to build a port: the build translates the game's own
code, and no game code or data is in this repository.

## What you need (Windows)

- **Visual Studio 2022 or later** with "Desktop development with C++" (the build uses its environment and Ninja).
- In each project's `third_party\` folder (not committed, see `.gitignore`): portable **LLVM 23.1.4**
  (`llvm-23.1.4\`), **CMake 4.4.4** (`cmake-4.4.4\`) and **sccache 0.18.0** (`sccache-v0.18.0\`).
- The **rexglue SDK**, built and installed into `Gears2-Native\third_party\rexglue-install-llvm23\` - see
  [Gears2-Native/patches/README.md](../Gears2-Native/patches/README.md): clone the SDK at the pinned tag, apply our
  patches in number order, move its third-party submodules to the tags in `sdk-pins.txt`, and configure it with the
  options in `sdk-cmake-options.txt`.
- Your game, extracted with its title update, in each project's `game\` folder (the `tools\unpack_*` scripts and the
  header of each `tools\build.ps1` say what goes where).

## Building a game

From the project folder, in PowerShell:

| Game | Command | Output |
|---|---|---|
| Gears of War 2 (Title Update 6) | `tools\build.ps1 -Config Release -Tu6` | `build\Release-tu6\gow2.exe` |
| Gears of War | `tools\build.ps1 -Config Release` (in `Gears1-Native`) | `build\Release\gow1.exe` |
| Gears of War 3 (Title Update 6) | `tools\build.ps1 -Config Release -Tu6` (in `Gears3-Native`) | `build\Release-tu6\gow3.exe` |
| Gears of War: Judgment | `tools\build.ps1 -Config Release` (in `GearsJ-Native`) | `build\Release\gowj.exe` |

Gears 1, 3 and Judgment build on Gears 2's shared code (`Gears2-Native\src\layer0_xenos` and its tools), so keep the
four project folders side by side as they are here.

The launcher falls back to a second, normal build of a game whose faster build crashes twice in a row:
`Gears2-Native\cogbox\tools\build_normal_code.ps1` builds all four into `build\<config>-normal\`.

## Building the launcher (COGBox)

The launcher is cross-compiled on Linux with MinGW-w64:

```
cd Gears2-Native/cogbox
SDL3_DIR=/path/to/SDL3-3.4.20/x86_64-w64-mingw32 FFMPEG_DIR=/path/to/ffmpeg-xma ./build.sh
```

`tools/build_ffmpeg_xma.sh` builds the small FFmpeg (XMA decoder only, LGPL) the library music uses; without it the
launcher builds and runs without music. The unit tests in `cogbox/test/` build with plain `g++` (see the first lines of
each test file).

## Packaging

`Gears2-Native\cogbox\tools\package_cogbox.ps1` assembles the release folder from the four builds, the launcher, the
runtime DLLs and the licence texts.
