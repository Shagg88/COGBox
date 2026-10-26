# upl-n096a - S3.5 step 1: our renderer's backend skeleton + shader cache (build check first)

What it is: our own D3D12 device (layer 0), an API-independent command interface (`xenos/renderer/gpu.h`), the shader
cache (microcode -> our HLSL -> DXIL with DXC in-process, cached in out\shader-cache), and the shadow observer behind
`--gow2_renderer=shadow`. In this step the observer only counts draws/copies, translates + compiles every shader it
sees, and runs a one-time device self-test (compute dispatch + readback). Off by default; nothing changes for players.

Copy into the repo at the same paths, apply EDITS.md (dev_tools.h, gow2_app.h) and cmake-snippet.txt (CMakeLists.txt).
Also includes the reviewed renderer-core headers (upl-n092 + review fixes/b) and their test file.

Run: `--gow2_renderer=shadow` (optional `--gow2_renderer_log_every=300`, `--gow2_shader_cache_dir=...`).
Expected log: `gow2_renderer: shadow mode on: D3D12 <GPU>, Shader Model 6.6 | DXC 1.9 (...) in-process from ...`,
then `gow2_renderer: self-test ok (compute + readback)`, then every 300 frames a `frames A-B: draws ...` summary
(shaders translated / disk hits / compiled / failed). On the second run "compiled" should be ~0 (all disk hits).

Checked in the container: clang 18 -fsyntax-only, x86_64-w64-mingw32 target with -fms-extensions, against the SDK
headers (bd833a2a worktree) and DirectX-Headers (MSVC-style method signatures forced, as the Windows SDK uses with
clang targeting MSVC). Not built or run on Windows yet.

## Files (md5)
79ee9007a21db2979a2aa848bcff7dce  ./EDITS.md
b0432474342ea1518c353bc78cdc4c9a  ./cmake-snippet.txt
b7b03efde7eb52f0f0488351ebf82683  ./src/layer0_xenos/include/xenos/edram.h
11d03c717f7b339c3fcdf7670689c10b  ./src/layer0_xenos/include/xenos/gamma.h
f5f45c4dec46ab3234721c38ebb19c5c  ./src/layer0_xenos/include/xenos/primitives.h
9cb9b95c5a379513956f2c67df1d0226  ./src/layer0_xenos/include/xenos/renderer/gpu.h
b794aed206002e26f7945c45f8baa8a3  ./src/layer0_xenos/include/xenos/renderer/shader_cache.h
cae8f2d5c9dc76bb12a28795bddd074a  ./src/layer0_xenos/include/xenos/resolve.h
69731ea0e648dbd47e739a71f8e861e2  ./src/layer0_xenos/include/xenos/texture_layout.h
34e5797084efbd73e99c271de844c4ec  ./src/layer0_xenos/renderer/d3d12_backend.cpp
20e33af69dfe80a6b13a7df981ff5a4a  ./src/layer0_xenos/renderer/dxc_compiler.cpp
901357dc531a4d607081f21c349c3ead  ./src/layer0_xenos/renderer/shader_cache.cpp
b71c6dcde734fedf3321f72ac5513bc8  ./src/layer0_xenos/renderer/shadow_renderer.cpp
9b3288d131e90d54256079343510a385  ./tests/golden/layer0_renderer_core_golden.inc
d1dc020ca056f1d870fe965e47b3178f  ./tests/golden/layer0_renderer_core_sweeps.h
4aa7c91153b22a585b2f49b18c3cb217  ./tests/layer0_renderer_core_test.cpp
