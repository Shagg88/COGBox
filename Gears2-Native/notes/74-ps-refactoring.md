# 74. Pixel-shader refactoring switch (patch 0095) - both games

Shagg 2026-10-24: "Do whatever you can do while I'm asleep/away." Item from the Vulkan plan (notes/68): measure
whether our strict "precise everywhere" maths costs GPU time.

## What it does
- `--gow2_renderer_ps_refactoring` (default false = as before).
- true: a pixel shader that writes no depth, uses no kill and does no memory export declares the DXBC global flag
  "refactoring allowed". The D3D12 driver may then fuse multiply-adds and reorder maths in it.
- Vulkan: the same shaders get SPIR-V without NoContraction (dxbc_spirv reads the flag).
- Vertex shaders and depth-writing / killing pixel shaders stay precise, so positions, depth and alpha-test edges match
  between passes (notes/42).
- Colour results can differ in the last bit. Pipelines rebuild once (the bytecode changes).

## Checked before the job [V, local]
- Corpus (2475 Gears 2 ucode shaders): 1712 change, all as expected (vertex shaders unchanged).
- A 300-shader sample of the SPIR-V passes spirv-val.

## Job 400 (to measure)
- ch10 D3D12 defaults against job 397's gate (switch off: must be identical).
- ch10 and ch15 D3D12, ch15 Vulkan, switch off vs on: GPU by target and FLIP.
- Default changes only on a clear gain with a clean picture.
