# 69. Shader prebuild at package open (patch 0068, SDK patch 32) - both games, and the Gears 1 / Judgment checklist

Shagg 2026-10-23: "if it's able to build behind the actual loading screen then we can completely scrap my previous
idea"; "I want ALL our fixes and patches to apply to both games"; Gears 1 and Judgment later.

## What it does
1. SDK patch 32 (patches/sdk-patch32-file-open-observer.diff): `rex::filesystem::SetFileOpenObserver` is called
   after every successful file open, with the guest path and the host path (host folder devices). The default is off.
2. Layer 2 (src/layer2_ue3/shader_prebuild.cpp, UE3, game-free): for a `.xxx` package (or a `*shadercache*.bin`)
   the game opens, one background thread reads the same file from disk once per session. It unpacks the compressed
   chunks with the SDK's own `lzx_decompress`, finds the shader containers (Gears 2: flat; Gears 3: inside inner
   XMemCompress blocks) and hands each new pixel shader to layer 0. It hands over two variants per record (the whole
   microcode region, and the code after the literal-constant block) until draws show which one the game uses, then
   only that one (under 5% of the hits = dropped, after 32 hits).
3. Layer 0 (shadow_renderer.cpp, d3d12_backend.cpp):
   - The recording thread drops shaders already known: drawn, from the start-up warm-up, or offered before.
   - One prebuild thread translates the rest exactly like a draw (`ShaderCache::TranslateDxbcOnly`).
   - The backend then builds a warm-only pipeline (`WarmPixelShader`): created, released, never cached or saved.
     The workers take these after live requests and before the start-up backlog. At most 64 are queued.
   - The template pipeline is the first DXBC triangle-list scene draw's vertex shader and state.
   - Job 362c [V]: the driver keeps each compiled stage, so the real pipeline later costs ~0.1 ms instead of 15-35 ms.
4. The 0063 / 0064 hold and the 0067 spinner stay as the safety net.

## Measured before the build [V, local test harness on the real packages, 2026-10-23 21:00-21:10]
| Package | Pixel shaders | Scan time | Notes |
|---|---|---|---|
| Gears 2 SP_Hospital_03 | 2127 | 0.2 s | |
| Gears 2 SP_Outpost_P | 422 | 0.1 s | |
| Gears 3 GearGame | 6848 | 0.6 s | 8975 with the nopad LZX fix |
| Gears 3 SP_Ravens2_05_S | 2109 | 0.4 s | |

- The frame-size check before decoding made the inner-block search 10-15x faster with identical results.
- Decoding each block on its own (a failed block no longer zeroes the rest of its chunk) raised Gears 3 GearGame
  from 2529 to 6848 pixel shaders.
- Gears 3 drawn records (359) against 4 local packages: 104 found. 121 would be found with the nopad fix.
  - Code-only variant: 120. Whole region: 5.
  - Gears 2 Hospital_03 against a ch10 record: whole region 187, code-only 16.
- Job 367 [V]: Gears 2 drawn matched by the whole region was 87% for PS and 81% for VS. The "0 of 218 VS" from the
  harness lists was wrong. Vertex shaders are not prebuilt yet: a VS needs a pixel-shader partner, and each new VS
  costs ~16 ms. That's the next step if the PS-only version passes.

## Open
- The nopad LZX fix in-process would mean patching the SDK's libmspack (LGPL, already linked by the SDK for XEX
  images). Gears 3 GearGame.xxx loses ~24% of its pixel shaders without it. Decide after the first measurements.
- Gears 3: needs SDK patch 32 in its SDK and the one call in its app (below).

## Checklist for a new UE3 game (Gears 1, Judgment)
1. SDK with patches 1-32 (the combined patches/rexglue-sdk.diff).
2. In the game's app, OnPostSetup: `XenosInstallRendererIfRequested(...)`, then `Ue3InstallShaderPrebuildIfRequested()`.
3. Game data on a host folder (game\extracted), not a disc image: the observer only gives host paths for host folders.
4. Run with --gow2_renderer_shader_backend=dxbc. Check the log for these lines:
   - "ue3_shader_prebuild: on";
   - per package, "N pixel / M vertex shader(s)";
   - the renderer's "shader prebuild (patch 0068)" totals, where "its shader prebuilt" should be most of
     "new draw pipelines with a pixel shader".
5. If a game packs its shader containers differently (no 0x102A110x magic, other inner framing), it shows up as
   0 pixel shaders per package. Then compare a drawn ucode record with the package bytes (tools: the local harness
   method in this note's measurements).
