# Automatic native HDR overlay handling

Gears 2 and Gears 3 share this renderer path. It extends the native FP16 scene
through common late UI draws without requiring a pixel-shader hash for every
material. The main scene replacement and PsychoV35 grading remain unchanged.

## Independent UI

Eligibility now uses the current content-versioned HDR target, resolved scene
input lineage, shader outputs and blend state. An independent layer no longer
needs a captured vertex/pixel shader pair. Scene-reading inputs receive their
own HDR companions through the generic material path described in
[HDR resource transport](hdr-resource-transport.md). The texture provenance
diagnostics remain available for other renderer users and investigation.

Supported RGB blends include opaque replacement, straight alpha, additive,
premultiplied alpha, destination-color modulation and double modulation.
Shader variants are cached by both shader identity and blend mode. Ordinary UI
emission is bounded before gamma decoding and scaled by UI white. Premultiplied
coverage is decoded separately from additive emission; alpha-zero glow remains
visible. Modulation is a dimensionless factor and ignores UI white and scene
peak. Double modulation retains its gain above one.

The replay runs before the original draw. It retains the original scissor,
geometry and depth/stencil tests, but suppresses replay depth/stencil writes.
The original draw remains their sole writer. Gears' EDRAM target allocations can
be taller than the visible scene (3840x6144 versus 3840x2160 at 3x). When stencil
or depth is tested, a cached FP16 replay target matches the original attachment
allocation. The persistent target companion now matches that allocation directly while
retaining separate visible dimensions. Ordinary overlays need no copy in/back.
Scene-pair scratch targets copy only the intersecting scissor rectangle. This preserves clipping without optional mixed-size
attachment support and leaves normalized sampling of the visible HDR image intact.
Mismatched guest attachments and multisampling remain guarded. 

A padded FP16 allocation costs 180 MiB at 3840x6144. Allocations are pooled;
immutable full resolves share them and later writes copy only when necessary.
This avoids the earlier per-overlay copy cost. Whole-game performance still
needs live validation; synthetic timings are recorded in the transport document.

## Scene-reading effects

Scene-reading materials use versioned, immediately frozen resolve snapshots.
Their content lifetime is independent of the current frame and latest scene.
Offset/scaled/partial copies, compatible colour aliases and regional clears
are handled explicitly; CPU writes and overlapping incompatible views cannot
reuse stale HDR. See [HDR resource transport](hdr-resource-transport.md) for
the ownership model, cache budget and supported interpretations.

The material evaluates original bounded inputs first for discard, alpha tests
and alpha. The extended evaluation redirects every recognised HDR input to an
unused texture binding, including explicit-LOD/gradient/load operations.
Dependency analysis extends upper RGB clamps fed by HDR samples. Both paired
outputs preserve the original blending and replay read-only depth/stencil tests,
using equally padded attachments where needed. The existing scene-energy
compositor keeps additional positive luminance in the original bounded result's
colour direction, preserving black fades and avoiding channel inversion.

The previously verified Gears 2 scene/UI material retains its specific path
when a generic transported input is unavailable.

## Limits and cost

This reduces per-material profiling; it does not establish complete coverage
for every chapter, menu, post-process permutation or multiplayer map. Unsupported
writers invalidate the affected resource companion and use the
existing reference fallback. Arbitrary clamped world inputs cannot have their
lost range recovered simply by widening the output target.

Ordinary overlays need one extra draw, as the previous profiled UI path did.
Scene pairing adds snapshot/preparation/composite GPU work. Snapshot allocation
and pipeline compilation are cached. Whole-game frame-time cost has not been
measured for this change.

## Validation

The core suite includes classifier rejection, depth/stencil ownership, mixed
sample rejection, cache hits, mip provenance, CPU reuse and unrelated resolves.
The `padded_stencil` GPU mode uses the production replay helper to verify
matching allocation sizes, nonzero copy origins, clipped and untouched HDR
pixels, allocation reuse and two successive tests of the unchanged depth/stencil.
The `gn_hdrtest automatic_overlays` mode executes the production DXBC and HLSL
epilogues with real blending: 224 draws per API, including negative/over-range
inputs, partial/zero alpha, untouched HDR pixels, premultiplied emission,
modulation and double modulation, and independent UI-white/peak settings.
The `automatic_scene` mode executes a newly authored scene-sampling material
through production pairing, freezing an older scene resolve while changing
the current background. The transport mode additionally tests resource copies, clears, aliases,
multiple inputs and bounded coverage/alpha. All modes run on Direct3D 12 and
Vulkan. These are
GPU regression checks, not a substitute for playthrough coverage.

## Fallback diagnostics

Every 300 presentations the renderer reports cumulative reference-frame reasons:
no scene, resolve generation, target state, depth/stencil attachment, material
eligibility, scene geometry, shader/pipeline, resource allocation or vertex twin.
Later-writer details include visible/allocated/depth dimensions, historical and
current rendered texture masks, sampled slots, vertex evidence, source readiness,
side effects, alpha test, clipping/discard and scene-source eligibility. Distinct
reason/texture-mask states can produce separate diagnostics for the same shader.
The padded replay count shows whether the stencil fix is being exercised.
