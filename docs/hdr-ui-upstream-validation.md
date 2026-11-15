# Automatic HDR UI candidate — 15 November 2026

Baseline: upstream `066465f20fa292f7c75e287c734f3e693f4cb602` (0.6.2).
Branch: `codex/hdr-ui-upstream-20261115`.

This is stage 1 of the supplied fallback investigation plan. It adds bounded,
scene-independent UI coverage. It does not establish gameplay performance parity
or eliminate all SDR fallbacks. Blur, distortion, scene-reading post-processing,
and general resource transport remain deferred until the gameplay comparison passes.

## Implementation

- Retain the existing compatible UI profile path and upstream renderer optimizations.
- Classify additional screen UI from vertex evidence, blend/output state and bound
  resources. Reject unknown scene-reading inputs, cube/volume inputs and side effects.
- Replay supported UI directly into the persistent FP16 scene target before the
  original draw. Preserve geometry, alpha tests/discards and depth/stencil tests.
  Disable depth/stencil writes only in the HDR replay; the original draw remains
  the sole writer of that state.
- Reuse shader variants and backend pipeline caches. Prepare extra variants off
  the draw thread and request pipelines asynchronously. Pending preparation keeps
  the existing fallback. Vulkan conversion uses upstream's live/warm queues and
  warm-up concurrency limit.
- Keep CPU-only RGBA8 eligibility cached by resource version using page provenance;
  do not hash texture contents or scan all texture allocations per draw. Retain
  the upstream failed-load retry behavior and texture-binding memoization.
- Allocate one persistent FP16 scene target matching the existing attachment height
  so stencil-tested replays need no stencil or scene-preservation copies. At
  3840×2160 visible / 3840×6144 allocated, this adds about 117 MiB versus upstream's
  visible-only target. Native HDR SMAA samples only the visible region.

The historical full transport implementation was reference material only. No
global target upgrade, companion resolve/copy/alias system or per-overlay
preservation dispatch was restored.

`--gow2_hdr_ui_automatic=true` enables the candidate (default in this test build).
`--gow2_hdr_ui_automatic=false` selects the upstream UI/allocation path. Restart
between comparisons: the SMAA pipeline is cached for the process lifetime.

## Completed checks

Unchanged baseline: 218 CPU cases / 1,105,214 assertions.
Candidate: 221 CPU cases / 1,105,243 assertions.

Sixteen GPU test invocations passed: automatic UI, direct stencil replay, async UI
cache, original Gears 2/Gears 3 scene mapping, original UI, off and SMAA on D3D12
and Vulkan. Automatic UI checks cover 512 draws per API using the production
DXBC/HLSL generators, premultiplied emission, modulation/2x, untouched HDR,
independent UI white, shader kill and alpha-test rejection. Direct replay checks
verify original stencil/depth state is not updated twice and padded SMAA sampling
does not read the unused region. These are authored tests, not full-game coverage.

A synthetic 4K test with 35 overlays and identical pixels measured visible versus
padded target GPU medians: D3D12 2.395 / 2.364 ms; Vulkan 2.357 / 2.387 ms. Both
paths issue zero preservation copies. This measures target shape only, not the
cost of additional supported UI or gameplay FPS.

Both games' baseline and candidate executable dependency checks passed against
the same isolated SDK build. The SDK includes upstream patches 96–103, which the
reset upstream game sources require. They are present in both reference and
candidate builds, not counted as HDR changes.

Artifacts and logs: `work/hdr-upstream-ui-20261115/`. `final-checks.json` records
the sixteen GPU exit codes. Separate baseline/candidate directories retain both
executables with matching runtime DLLs.

## Gameplay acceptance still pending

Use `work/hdr-upstream-ui-20261115/launch-ab.ps1 -Variant baseline`, then repeat
with `-Variant candidate`. This reuses the last logged Gears 3 settings and save
folder, enables the same existing frame logger, and writes separate logs/CSVs.
Close the game normally between launches. Warm each build, then repeat the same
checkpoint and camera route, stencil menu, reload and damage effect. Use separate
matched gameplay-only CSV time windows; loading/menu duration must not distort
the comparison. The normal 120-FPS cap is retained, so capped measurements alone
cannot establish equal rendering headroom.

Compare average FPS, slowest-1%-frame mean FPS, P95/P99 frametimes, ≥250 ms stalls,
shader/pipeline waits, fallback counts and memory. Check colors, UI clipping and
HDR headroom visually. Repeat on both APIs and Gears 2 before widening coverage.
Reject a consistent regression rather than expanding into post-processing.

Runtime counters are logged every 300 native/reference HDR presentations:
`automatic UI: ... direct replays, ... preparation misses, ... input rejections`.
Counters are cumulative. Remaining fallbacks are expected for unsupported inputs
or pending preparation; never suppress them just to improve the counter.
