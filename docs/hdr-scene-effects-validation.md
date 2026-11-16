# Selective HDR blur candidate — 15 November 2026

Implemented on `hdr-gears2-gears3`, on top of the working automatic UI changes.
This is a deliberately narrow first slice of stage two, not universal HDR
transport. Equal gameplay performance has **not** been established.

## Supported slice

Two captured Gears 3 blur pixel shaders (`8356D7C3D65FE328` and
`212F89E82422D29D`) can consume a proven resolved HDR scene and draw back into
the currently owned HDR scene. Their original geometry (including the built-in
rectangle-list geometry shader), scissor and alpha remain;
only the verified RGB upper clamp is extended. The existing scene mapper is not
applied again. Shader fingerprints and word counts must match the captured profiles.

The resolve registry stores a same-frame lease: target identity/write generation,
guest-memory range/version, dimensions, endian and R/B-copy mapping. Resolves do
not copy HDR pixels. On the first supported consumer, one compute dispatch makes
an encoded FP16 sampling snapshot from the visible HDR scene. Later draws reading
that exact version reuse it, even if the source render target has subsequently
changed. Other stale leases fail closed. CPU writes, overlapping guest resolves,
unknown target writes, clears, alias transfers and frame changes cannot establish
a new valid HDR version by themselves.

The original SDR draw remains intact for guest compatibility. The HDR replay writes
directly to the persistent HDR target, preserving pixels outside the original
geometry/scissor. There is no destination-preservation copy. Original and HDR
texture bindings/pipelines are restored, including invalidating the binding memo
when needed. RGB copy swaps are composed with raw fetch selectors; alpha is retained.

Eligibility currently requires opaque, straight-alpha or premultiplied single-target RGBA8 output, no depth/stencil
tests, active occlusion queries or side effects, canonical unsigned tiled 2D one-level scene input with matching pitch and
copy/fetch endian, and a complete untransformed source resolve. Alpha-as-RGB
selection, exponent-adjusted resolves, partial resolves, multisampling and other
formats stay on the existing fallback. Additional UI behavior is unchanged.

This does **not** yet cover intermediate/downsampled HDR effect chains, distortion,
arbitrary scene-reading shaders, copies or aliases carrying HDR forward. Supporting
those requires validating their material/resource flow; they are not silently
accepted by this candidate. Gears 2 shares the infrastructure but has no newly
verified blur profile in this slice.

## Performance constraints

`--gow2_hdr_scene_effects=false` is the default. With it off, no effect resources
are allocated, prepared or dispatched. With it on but no supported consumer,
the registry records metadata only and does no HDR conversion or effect replay.
There is one reusable visible-sized FP16 sampling image, about 63.3 MiB at 4K.
Its steady allocation is capped at 128 MiB. Resizing can temporarily retain the
old resource until its GPU fence completes. No unrelated render targets are upgraded.

HDR material variants use the existing asynchronous compiler and Vulkan conversion
queues. Known cached blur variants are warmed on startup. Pipeline requests are
asynchronous; pending preparation keeps the fallback rather than waiting in a draw.

A nonuniform 4K GPU benchmark of the actual input-conversion shader measured:

| API | Median conversion | Maximum trial |
| --- | ---: | ---: |
| D3D12 | 0.212 ms | 0.248 ms |
| Vulkan | 0.213 ms | 0.250 ms |

This is additional work on a consumed scene version. It excludes the blur replay,
driver first-use allocation cost and whole-game pacing. It is **not** evidence of
equal gameplay FPS. Do not enable this by default on the strength of synthetic
tests. Retain the existing fallback if matched average FPS, 1% lows and P95/P99
frametimes regress consistently.

## Validation

224 CPU cases / 1,105,279 assertions pass. Tests cover lease rejection for changed
frame/owner/write/memory, canonical fetch/resolve restrictions, R/B channel mapping
and allocation limits.

22 D3D12/Vulkan GPU invocations pass: authored scene-filter pixel checks, actual
4K encoder timing, both captured blur shader variants through the asynchronous
cache and real pipelines, original UI/direct-stencil coverage and Gears 2/Gears 3
scene/off/SMAA checks. Authored tests compare bounded and extended DXBC/HLSL paths,
including R/B swaps, original alpha, untouched scissored HDR and poisoned padding.
Premultiplied checks include zero-alpha emission and blend composition over an HDR
background. Captured shader microcode stays local under ignored `work/`; it is not committed.

Artifacts: `work/hdr-scene-effects-20261115/`. Both baseline and candidate packages
have matched runtime DLLs and pass dependency/hash checks. Baseline packages retain
the pre-change automatic-UI executables. The candidate is not installed into the
normal launcher paths while performance acceptance remains pending.

Use `launch-comparison.ps1 -Game Gears3 -Variant baseline` and then `-Variant candidate`.
Each preserves the game's last normal launch settings, enables the same frame
logger and writes separate logs/CSV. Warm both builds and repeat the same
blur-producing sequence; inspect `scene effects:` for actual accepted replays.
If there are zero replays, the run establishes no active blur-path coverage.
SMAA T2x/DLSS pacing remains a separate issue and must not be inferred fixed here.
