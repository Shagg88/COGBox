# 26. Vulkan: darker picture and black blocks (Gears 3 only)

Layer 0 bug (Gears 2 chat owns the fix); we run the tests. Test exe build\RelWithDebInfo-vk (SDK snapshot
rexglue-install-llvm23-rwdi-20261024-1709, Vulkan ON). Chapter 1 start, frame 1500 compared.

## Symptom [V]
Vulkan frame is darker overall (mean RGB 44/40/25 vs D3D12 62/56/38) with black patches (bookshelf wall, floor).
Gears 2 and Judgment Vulkan pictures match their D3D12.

## Ruled out [V]
- Job 127: pass_barriers=full, transfer_bits=false, tex_post_constants=false - each within 0.2/255 of plain Vulkan.
- Job 128: shader_backend=dxbc (instead of hlsl), vulkan_clear_in_pass=false - same.
- The game's draw list for the frame is the same on both APIs (727 / 728 records, 13 small particle/order differences).
- The Vulkan pipeline cache does save (35-73 MB, updated during runs).
- --clock_fixed_step_fps is not in this exe; frame 1500 still lined up between runs.

## Job 129: target contents (Gears 2's patch 0121, c09e0f2) [V]
- Frame 1500, first target switch: after draws #4-#105 (opaque scene into HDR target id 5 at 960x480) Vulkan is already
  ~20% darker (c0-c2 means 0.0962/0.0700/0.0532 vs D3D12 0.1214/0.0846/0.0617). So the darkening starts in the opaque
  scene draws, before any post-processing; the 16_16 lead below is downstream, not the cause.
- id 7 (fmt 14, 640x4096) after draw 543: Vulkan has +inf values where D3D12 tops out at 6.55e+04 - possible source of
  the black blocks.
- Runs: D3D12 out\runs\20261024-190111, Vulkan out\runs\20261024-190317. Mailed to Gears 2 19:12.

## Job 130: per-draw stats (patch 0122, ae3c4ae) [V]
- --gow2_renderer_float24_remap=false changes nothing (picture and stats same as plain Vulkan).
- Draws 1-110 (opaque, id 5) add the same amount on both APIs; id 5 is already different at the first readout, so the
  difference there is carried in from before the frame's first colour draw [G cause].
- Draw 303 (vs B5A1208D037C2F4F ps FC367470036D7CFE, 1296 verts, into id 2 = 8888_GAMMA, red only, blend src*SRC_ALPHA,
  reads the depth copy R32F + BC1/BC3 textures): red mean 0.1322 on D3D12, 0.0640 on Vulkan; everything else in id 2
  equal before and after. Looks like UE3's light attenuation / shadow mask [G]: half of it would darken lit surfaces
  only, which is what Judgment sees too. The big extra darkening of id 5 comes after it (draws 355-511, the lighting).
- Runs: D3D12 out\runs\20261024-192420, Vulkan 20261024-192627, Vulkan remap off 20261024-192834. Mailed 19:40.

## Job 131: around draw 303 and through the lighting (patch 0123, 589bf20) [V]
- Draw 303 alone changes the attenuation channel: 0.1337 vs 0.0649 (Vulkan/D3D12 = 0.49).
- The scene target restarts empty at draw 362; the lights (draws ~395-442) then add exactly half as much on Vulkan
  (0.0978 vs 0.1961 after 442) over the same pixels - the darkening. Fixing draw 303 should fix it [G].
- Draws ~483-506 leave ~3.3% more pixels unwritten on Vulkan - coverage (depth/stencil/clip): likely the black blocks.
- Runs: A/B out\runs\20261024-194045 / -194252, C/D -194459 / -194705. Mailed 19:55; next: Gears 2's patch 0124 reads
  the depth copy and depth target.

## Job 133 (try 1 of Shagg's 3, patch 0125 14b4e08) [V]
- Draw 303's inputs are identical on both APIs: depth copy t0 (R32F 1280x720) mean 0.0310 [0.00854, 0.0796] on both;
  the target going in is the same. Its output is still half on Vulkan (0.0631 vs 0.1321).
- So the fault is inside draw 303 on Vulkan [G]: most likely the blend's SRC_ALPHA (the shader's alpha output), else
  the gamma target's blending or the BC textures / samplers. Suggested try 2: force src*ONE for that draw on Vulkan.
- Runs: D3D12 out\runs\20261024-201158, Vulkan 20261024-201405. Mailed 20:21.

## Job 134b (try 2, last run before Vulkan was parked - Shagg 20:34) [V]
- Vulkan with --gow2_renderer_gpu_block_textures=false --gow2_renderer_gpu_texture_load=false (textures loaded on
  the CPU, ~120 ms/frame so a 360 s run): draw 303's red = 0.1208, close to D3D12's 0.1321 (normal Vulkan 0.0631).
- Points at the Vulkan GPU texture loader (BC textures read with implicit LOD, Gears 2's lead) [G]. Caveat: in this
  run the depth copy t0 read all zero (timing changed), so the inputs were not identical - a strong hint, not proof.
- Run: out\runs\20261024-203548. Job 134 (120 s) never reached frame 1500.
- PARKED here. Next time: compare the BC textures' mip levels between the GPU and CPU loaders on Vulkan.

## Earlier lead (now downstream) [G]
k_16_16 render targets are created as VkFormat 92 (R16G16B16A16_SNORM) but the textures made from them are VkFormat 91
(UNORM); D3D12 uses DXGI 9 typeless for both, target flagged "bits". Draws #120-#130 (640x360, 322x182 k_16_16) feed
#144, the full-screen composite. Reading SNORM bits as UNORM gives about v/2 - a darker frame. Sent to Gears 2
(mails 18:30, 18:42); next test is their switch for this view format.
