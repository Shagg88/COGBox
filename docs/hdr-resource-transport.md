# Versioned HDR resource transport

The shared Xenos renderer keeps FP16 scene colour alongside the original guest
resources. Gears 2 and Gears 3 consume this renderer. The guest's formats,
memory bytes and normal rendering path remain authoritative for coverage,
alpha, state changes and unsupported resource interpretations.

## Resource ownership

Each render-target companion is keyed by the renderer's never-reused target ID
and exact `write_gen`. Every original draw, clear and alias transfer changes
that version. A companion can be adopted in any frame when its content version
matches; holding a pause/menu image does not expire HDR by frame age.

A colour resolve immediately publishes an immutable FP16 version keyed by guest
address range, content version, format and endian interpretation. A later draw
cannot turn an earlier resolve into a different scene. Overlapping GPU writes
invalidate all affected interpretations. CPU writes are detected by the guest
watch's content version. Partial writes retain outside pixels only if the
previous full resource version was still current.

Full compatible resolves pin the target allocation through shared ownership,
without a GPU copy. A later writer uses copy-on-write when old colour must be
preserved. Full scene replacement allocates/reuses another image; a full clear
detaches the old target version without modifying pinned resolves. Unreferenced
allocations are pooled and safely retired through the backend.

Partial snapshot alpha records HDR validity, separately from the game's alpha. Offset
and scaled copies update their actual destination rectangle. Pixels outside
known HDR coverage are reconstructed from the original bound SDR texture or
the presenter's original RGB10 reference. This avoids uninitialised padded
areas and stale HDR after allocation reuse. Compatible resolves preserve
exponent gain and red/blue channel swaps in linear colour.

Matching colour aliases use the renderer's EDRAM tile address mapping to move
HDR. Original source pixels supply areas outside the visible companion.
Clears and raw word/depth reinterpretations synchronise only the changed
rectangle from the original target, preserving HDR elsewhere. Growing a
companion seeds new areas and copies its valid old extent.

## Later materials

Resource lineage identifies scene inputs; materials do not need individual
shader hashes. Each HDR input receives a proxy in a distinct unused 2D binding.
The translator supports the material's sampling operation, including explicit
LOD, gradients and loads. The original bounded evaluation determines kills,
alpha tests and output alpha. A second evaluation transports extended colour.
Control-flow dependency analysis extends upper RGB instruction clamps only
where they depend on HDR samples; unrelated clamps and lower bounds remain.

Stored channel order, fetch swizzle and RGBA8 byte-endian permutations are
tracked separately. Presentation maps HDR through the same framebuffer fetch
interpretation as the original reference. The Gears 3 `A0A` BGR fetch cancels
its resolve's red/blue storage swap. Original SDR gaps are already interpreted
and are not swizzled again. Material proxies retain the original texture-view
component mapping.

Paired effects use their original blend state and a pre-draw background. Depth
and stencil tests replay before the original draw, with writes disabled.
The target companion itself matches the padded attachment allocation, with a
separate visible extent, so ordinary overlays render directly without copying
the full visible image in and out for every draw. Paired scratch outputs are
padded when required. Native SMAA scales colour UVs to the visible rectangle
and keeps its weight texture in the original coordinate system. The original draw
performs depth/stencil updates once. Independent bounded overlays retain their
linear-light HDR composition path.

Inactive companions are evicted by age when the cache exceeds
`--gow2_hdr_companion_mb` (default 512 MiB) or 64 cached resource records.
Current/recent images remain resident; transient active images can exceed this
soft cache budget. Backend texture destruction defers GPU resource retirement.
One recently released allocation per size is kept in a bounded warm reserve
for up to 120 frames, preventing repeated large allocations when the active
working set sits just above the soft budget.

Material input preparation combines snapshot interpretation, validity fallback
and extended-gamma encoding in one compute pass. The result is reused for
repeated draws sampling the same content version and original reference.
Scene-effect background preparation and composition dispatch only over the
draw's clipped scissor rectangle.

## Validation and limits

Game-free CPU tests cover exact content ownership, held frames, incompatible
interpretations, overlapping writes, CPU reuse, binding exhaustion and shader
dependency propagation through branches and predicated overwrites.

`gn_hdrtest <d3d12|vulkan> hdr_transport` executes the production transport
shaders: frozen offset/scaled snapshots, partial invalidation, original SDR
gaps and alpha, regional clears, gain/channel swaps, packed presentation
references, colour aliases, multiple HDR inputs, RGB clamp extension and
original kill decisions, non-opaque whole-image validity, shared allocation
ownership, copy-on-write isolation, BGR/byte-endian interpretation and padded
SMAA sampling with deliberately invalid padding. Existing overlay, padded stencil/MRT, paired scene,
Gears 2/3 scene and UI regressions also run on both APIs. Captured Gears 3 blur
materials are compiled with their matching vertex shader through the generic
transport path; game-derived shader data stays in ignored local work files.

This supplies shared transport, not universal native HDR for every game.
Producing HDR initially still requires a correct scene/tonemap integration.
The current companion draw path covers single-sample RGBA8 colour targets and
compatible RGBA8 resolves/views. Multisample HDR, mip chains, cube/volume
views, arbitrary format reinterpretations, shader exports, multiple original
colour outputs and unsupported blend operations still use the original
renderer or invalidate the affected companion. GPU regressions establish the
tested operations; gameplay and whole-game performance require live verification.

The initial eager-copy build was rolled back after live testing exposed swapped
red/blue presentation and substantially worse performance. The corrected build
adds the fetch mapping and shared/direct padded paths above. A synthetic 4K,
35-overlay benchmark reduced preservation copies from 73 to one per frame;
the final local medians were 13.415 to 2.517 ms on D3D12 and 11.898 to 2.720 ms
on Vulkan. These timings describe that synthetic GPU workload, not gameplay
frame rates or a guarantee of matching the previous build in every scene.

The corrected build's live Gears 3 session kept the startup reference count
unchanged through menus and gameplay, with no later material, generation,
depth/stencil, resource or shader fallback counts. The user reported performance
was much better, with some remaining menu lag, a small gameplay cost and more
stuttering. The subsequent installed follow-up adds the fused/cached input
preparation, cropped composition and warm allocation reserve above. Its CPU
and GPU regressions pass; live frame-pacing validation remains pending.
